/*
	Copyright (C) 2025 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this software.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mcp_http.h"

#include <atomic>
#include <string>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment(lib, "ws2_32.lib")
	typedef SOCKET mcp_socket_t;
	#define MCP_INVALID_SOCKET INVALID_SOCKET
	#define MCP_CLOSE_SOCKET closesocket
	#define MCP_SEND_FLAGS 0
	typedef int mcp_socklen_t;
#else
	#include <arpa/inet.h>
	#include <errno.h>
	#include <fcntl.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <sys/select.h>
	#include <sys/socket.h>
	#include <sys/types.h>
	#include <unistd.h>
	typedef int mcp_socket_t;
	#define MCP_INVALID_SOCKET (-1)
	#define MCP_CLOSE_SOCKET close
	#ifdef MSG_NOSIGNAL
		#define MCP_SEND_FLAGS MSG_NOSIGNAL
	#else
		#define MCP_SEND_FLAGS 0
	#endif
	typedef socklen_t mcp_socklen_t;
#endif

/* Largest request body accepted, mostly a guard against a runaway client. */
static const size_t MAX_BODY_SIZE = 8 * 1024 * 1024;
static const size_t MAX_HEADER_SIZE = 16 * 1024;

static mcp_socket_t g_listenSocket = MCP_INVALID_SOCKET;
static std::thread g_serverThread;
static std::atomic<bool> g_running(false);
static mcp_http_process_fn g_processCallback = NULL;
static int g_boundPort = 0;

static bool SocketsStartup()
{
#ifdef _WIN32
	static bool initialized = false;
	if (initialized)
		return true;
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return false;
	initialized = true;
#endif
	return true;
}

static void SetNonBlocking(mcp_socket_t s)
{
#ifdef _WIN32
	u_long mode = 1;
	ioctlsocket(s, FIONBIO, &mode);
#else
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags >= 0)
		fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void SetBlocking(mcp_socket_t s)
{
#ifdef _WIN32
	u_long mode = 0;
	ioctlsocket(s, FIONBIO, &mode);
#else
	const int flags = fcntl(s, F_GETFL, 0);
	if (flags >= 0)
		fcntl(s, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

/* Waits until the socket is readable. Returns false on timeout or error. */
static bool WaitReadable(mcp_socket_t s, int timeoutMS)
{
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(s, &readSet);

	timeval tv;
	tv.tv_sec = timeoutMS / 1000;
	tv.tv_usec = (timeoutMS % 1000) * 1000;

	const int result = select((int)(s + 1), &readSet, NULL, NULL, &tv);
	return (result > 0);
}

static void SendAll(mcp_socket_t s, const char *buffer, size_t length)
{
	while (length > 0)
	{
		const int chunk = (length > 0x10000) ? 0x10000 : (int)length;
		const int sent = (int)send(s, buffer, chunk, MCP_SEND_FLAGS);
		if (sent <= 0)
			break;
		buffer += sent;
		length -= (size_t)sent;
	}
}

static void SendSimpleResponse(mcp_socket_t s, const char *status, const char *extraHeaders)
{
	char header[512];
	const int length = snprintf(header, sizeof(header),
		"HTTP/1.1 %s\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"%s"
		"Content-Length: 0\r\n"
		"Connection: close\r\n\r\n",
		status, (extraHeaders != NULL) ? extraHeaders : "");
	if (length > 0)
		SendAll(s, header, (size_t)length);
}

static void SendJSONResponse(mcp_socket_t s, const char *body, size_t bodyLength)
{
	char header[512];
	const int length = snprintf(header, sizeof(header),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Content-Length: %lu\r\n"
		"Connection: close\r\n\r\n",
		(unsigned long)bodyLength);
	if (length > 0)
		SendAll(s, header, (size_t)length);
	SendAll(s, body, bodyLength);
}

static bool HeaderMatches(const std::string &headers, const char *name, size_t &outValuePos)
{
	const size_t nameLength = strlen(name);
	for (size_t i = 0; i + nameLength < headers.size(); i++)
	{
		//header names start at the beginning of a line
		if (i != 0 && headers[i - 1] != '\n')
			continue;

		size_t j = 0;
		while (j < nameLength)
		{
			const char a = (char)tolower((unsigned char)headers[i + j]);
			const char b = (char)tolower((unsigned char)name[j]);
			if (a != b)
				break;
			j++;
		}
		if (j != nameLength)
			continue;

		size_t pos = i + nameLength;
		while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t'))
			pos++;
		outValuePos = pos;
		return true;
	}
	return false;
}

static size_t ParseContentLength(const std::string &headers)
{
	size_t valuePos = 0;
	if (!HeaderMatches(headers, "content-length:", valuePos))
		return 0;
	const long value = strtol(headers.c_str() + valuePos, NULL, 10);
	return (value > 0) ? (size_t)value : 0;
}

/* /mcp, /mcp/, /mcp?..., and / are all accepted. */
static bool IsMCPPath(const std::string &path)
{
	if (path == "/" || path == "/mcp" || path == "/mcp/")
		return true;
	if (path.compare(0, 5, "/mcp?") == 0)
		return true;
	if (path.compare(0, 2, "/?") == 0)
		return true;
	return false;
}

static void HandleClient(mcp_socket_t client)
{
	SetBlocking(client);

	//keep a slow or dead peer from stalling the emulator
#ifdef _WIN32
	DWORD timeout = 15000;
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
	timeval timeout;
	timeout.tv_sec = 15;
	timeout.tv_usec = 0;
	setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

	std::string buffer;
	char chunk[4096];
	size_t headerEnd = std::string::npos;

	while (headerEnd == std::string::npos)
	{
		const int received = (int)recv(client, chunk, (int)sizeof(chunk), 0);
		if (received <= 0)
			return;
		buffer.append(chunk, (size_t)received);
		headerEnd = buffer.find("\r\n\r\n");
		if (headerEnd == std::string::npos && buffer.size() > MAX_HEADER_SIZE)
		{
			SendSimpleResponse(client, "431 Request Header Fields Too Large", NULL);
			return;
		}
	}

	const std::string headers = buffer.substr(0, headerEnd + 4);

	//request line: METHOD SP PATH SP VERSION
	std::string method;
	std::string path;
	{
		size_t i = 0;
		while (i < headers.size() && headers[i] != ' ' && headers[i] != '\r') { method.push_back(headers[i]); i++; }
		while (i < headers.size() && headers[i] == ' ') i++;
		while (i < headers.size() && headers[i] != ' ' && headers[i] != '\r' && headers[i] != '\n') { path.push_back(headers[i]); i++; }
	}

	if (method == "OPTIONS")
	{
		SendSimpleResponse(client, "204 No Content",
			"Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
			"Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version\r\n"
			"Access-Control-Max-Age: 86400\r\n");
		return;
	}

	if (!IsMCPPath(path))
	{
		SendSimpleResponse(client, "404 Not Found", NULL);
		return;
	}

	if (method == "GET")
	{
		//no server initiated stream: tell the client that only POST is useful here
		SendSimpleResponse(client, "405 Method Not Allowed", "Allow: POST, DELETE, OPTIONS\r\n");
		return;
	}

	if (method == "DELETE")
	{
		//stateless server, nothing to tear down
		SendSimpleResponse(client, "204 No Content", NULL);
		return;
	}

	if (method != "POST")
	{
		SendSimpleResponse(client, "405 Method Not Allowed", "Allow: POST, DELETE, OPTIONS\r\n");
		return;
	}

	const size_t contentLength = ParseContentLength(headers);
	if (contentLength == 0)
	{
		SendSimpleResponse(client, "400 Bad Request", NULL);
		return;
	}
	if (contentLength > MAX_BODY_SIZE)
	{
		SendSimpleResponse(client, "413 Payload Too Large", NULL);
		return;
	}

	std::string body = buffer.substr(headerEnd + 4);
	body.reserve(contentLength);
	while (body.size() < contentLength)
	{
		const size_t want = contentLength - body.size();
		const int received = (int)recv(client, chunk, (int)((want < sizeof(chunk)) ? want : sizeof(chunk)), 0);
		if (received <= 0)
			return;
		body.append(chunk, (size_t)received);
	}

	if (g_processCallback == NULL)
	{
		SendSimpleResponse(client, "503 Service Unavailable", NULL);
		return;
	}

	char *response = g_processCallback(body.c_str());
	if (response == NULL)
	{
		//JSON-RPC notification or response-less message
		SendSimpleResponse(client, "202 Accepted", NULL);
		return;
	}

	SendJSONResponse(client, response, strlen(response));
	free(response);
}

static void ServerThreadProc()
{
	while (g_running.load())
	{
		if (!WaitReadable(g_listenSocket, 100))
			continue;

		sockaddr_in clientAddress;
		mcp_socklen_t addressLength = (mcp_socklen_t)sizeof(clientAddress);
		const mcp_socket_t client = accept(g_listenSocket, (sockaddr *)&clientAddress, &addressLength);
		if (client == MCP_INVALID_SOCKET)
			continue;

		if (g_running.load())
			HandleClient(client);

		MCP_CLOSE_SOCKET(client);
	}
}

int mcp_http_start(int port, mcp_http_process_fn process_cb)
{
	if (g_running.load())
		return 0;

	if (!SocketsStartup())
		return -1;

	g_processCallback = process_cb;

	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((unsigned short)port);

	const mcp_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == MCP_INVALID_SOCKET)
		return -1;

	int reuse = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	if (bind(s, (sockaddr *)&address, sizeof(address)) != 0)
	{
		MCP_CLOSE_SOCKET(s);
		return -1;
	}

	if (listen(s, 8) != 0)
	{
		MCP_CLOSE_SOCKET(s);
		return -1;
	}

	//report the real port so that "bind port 0" is usable
	mcp_socklen_t addressLength = (mcp_socklen_t)sizeof(address);
	if (getsockname(s, (sockaddr *)&address, &addressLength) == 0)
		g_boundPort = (int)ntohs(address.sin_port);
	else
		g_boundPort = port;

	SetNonBlocking(s);

	g_listenSocket = s;
	g_running.store(true);
	g_serverThread = std::thread(ServerThreadProc);

	return 0;
}

void mcp_http_stop(void)
{
	if (!g_running.load() && !g_serverThread.joinable())
		return;

	g_running.store(false);
	if (g_serverThread.joinable())
		g_serverThread.join();

	if (g_listenSocket != MCP_INVALID_SOCKET)
	{
		MCP_CLOSE_SOCKET(g_listenSocket);
		g_listenSocket = MCP_INVALID_SOCKET;
	}

	g_boundPort = 0;
	g_processCallback = NULL;
}

int mcp_http_port(void)
{
	return g_running.load() ? g_boundPort : 0;
}
