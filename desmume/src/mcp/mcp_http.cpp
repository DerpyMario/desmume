/*
	Copyright (C) 2025 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#include "mcp_http.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef _WIN32

static SOCKET g_listen_sock = INVALID_SOCKET;
static HANDLE g_thread = NULL;
static volatile long g_running = 0;
static mcp_http_process_fn g_process_cb = NULL;

static CRITICAL_SECTION g_sse_cs;
static std::vector<SOCKET> g_sse_sockets;

#define RESP_BUF_SIZE 65536

static int parse_http_request(const char* headers, size_t len, char* method, size_t method_size, char* path, size_t path_size, int* content_length)
{
	method[0] = path[0] = '\0';
	*content_length = 0;
	const char* end = headers + len;
	const char* p = headers;
	while (p < end && (*p == ' ' || *p == '\t')) p++;
	size_t i = 0;
	while (p < end && *p != ' ' && *p != '\r' && i + 1 < method_size) { method[i++] = *p++; }
	method[i] = '\0';
	while (p < end && *p == ' ') p++;
	i = 0;
	while (p < end && *p != ' ' && *p != '\r' && *p != '\n' && i + 1 < path_size) { path[i++] = *p++; }
	path[i] = '\0';
	/* Find Content-Length: (case-insensitive per HTTP) */
	for (const char* cl = headers; cl + 14 <= end; cl++) {
		if ((cl[0] == 'C' || cl[0] == 'c') &&
		    (cl[1] == 'O' || cl[1] == 'o') && (cl[2] == 'N' || cl[2] == 'n') &&
		    (cl[3] == 'T' || cl[3] == 't') && (cl[4] == 'E' || cl[4] == 'e') &&
		    (cl[5] == 'N' || cl[5] == 'n') && (cl[6] == 'T' || cl[6] == 't') &&
		    cl[7] == '-' && (cl[8] == 'L' || cl[8] == 'l') &&
		    (cl[9] == 'E' || cl[9] == 'e') && (cl[10] == 'N' || cl[10] == 'n') &&
		    (cl[11] == 'G' || cl[11] == 'g') && (cl[12] == 'T' || cl[12] == 't') &&
		    (cl[13] == 'H' || cl[13] == 'h') && cl[14] == ':') {
			cl += 15;
			while (cl < end && (*cl == ' ' || *cl == '\t')) cl++;
			*content_length = (int)strtol(cl, NULL, 10);
			break;
		}
	}
	return 0;
}

static void send_all(SOCKET s, const char* buf, int len)
{
	while (len > 0) {
		int n = send(s, buf, len, 0);
		if (n <= 0) break;
		buf += n;
		len -= n;
	}
}

static DWORD WINAPI server_thread_proc(LPVOID param)
{
	(void)param;
	char req_headers[8192];
	char* body = NULL;
	int body_len = 0;
	char resp_buf[RESP_BUF_SIZE];

	while (InterlockedCompareExchange(&g_running, 1, 1) == 1 && g_listen_sock != INVALID_SOCKET) {
		struct sockaddr_in client_addr;
		int addr_len = (int)sizeof(client_addr);
		SOCKET client = accept(g_listen_sock, (struct sockaddr*)&client_addr, &addr_len);
		if (client == INVALID_SOCKET) break;

		size_t header_len = 0;
		for (;;) {
			char c;
			int n = recv(client, &c, 1, 0);
			if (n <= 0) break;
			if (header_len < sizeof(req_headers) - 1) req_headers[header_len++] = c;
			if (header_len >= 4 && memcmp(req_headers + header_len - 4, "\r\n\r\n", 4) == 0) break;
		}
		req_headers[header_len] = '\0';

		char method[32], path[256];
		int content_length = 0;
		parse_http_request(req_headers, header_len, method, sizeof(method), path, sizeof(path), &content_length);
		if (content_length > 0 && (size_t)content_length < 512 * 1024) {
			body = (char*)malloc((size_t)content_length + 1);
			if (body) {
				int got = 0;
				while (got < content_length) {
					int n = recv(client, body + got, content_length - got, 0);
					if (n <= 0) break;
					got += n;
				}
				body[got] = '\0';
				body_len = got;
			}
		}

		/* Accept /mcp, /mcp/, /mcp?..., / (path is already truncated at first space) */
		int is_mcp_path = (path[0] == '/' && (path[1] == '\0' || (path[1] == 'm' && path[2] == 'c' && path[3] == 'p' && (path[4] == '\0' || path[4] == '/' || path[4] == '?'))));

		if (strcmp(method, "POST") == 0 && is_mcp_path && g_process_cb && body) {
			resp_buf[0] = '\0';
			g_process_cb(body, resp_buf, sizeof(resp_buf));
			size_t rlen = strlen(resp_buf);
			char hdr[256];
			int hlen = snprintf(hdr, sizeof(hdr),
				"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
				rlen);
			send_all(client, hdr, hlen);
			send_all(client, resp_buf, (int)rlen);
		} else if (strcmp(method, "GET") == 0 && is_mcp_path) {
			const char* sse_hdr =
				"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
			send_all(client, sse_hdr, (int)strlen(sse_hdr));
			/* SSE comment (':') so Cursor/client does not parse as JSON-RPC; only data: lines are parsed. */
			send_all(client, ": connected\n\n", 14);
			EnterCriticalSection(&g_sse_cs);
			g_sse_sockets.push_back(client);
			LeaveCriticalSection(&g_sse_cs);
			/* don't close client; keep for SSE */
			client = INVALID_SOCKET;
		} else {
			const char* bad = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
			send_all(client, bad, (int)strlen(bad));
		}

		if (body) { free(body); body = NULL; body_len = 0; }
		if (client != INVALID_SOCKET) closesocket(client);
	}
	return 0;
}

void mcp_http_start(int port, mcp_http_process_fn process_cb)
{
	if (g_listen_sock != INVALID_SOCKET) return;
	g_process_cb = process_cb;
	InitializeCriticalSection(&g_sse_cs);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */
	addr.sin_port = htons((unsigned short)port);

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) return;
	int reuse = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		closesocket(s);
		return;
	}
	if (listen(s, 8) != 0) {
		closesocket(s);
		return;
	}
	g_listen_sock = s;
	InterlockedExchange(&g_running, 1);
	g_thread = CreateThread(NULL, 0, server_thread_proc, NULL, 0, NULL);
}

void mcp_http_stop(void)
{
	InterlockedExchange(&g_running, 0);
	if (g_listen_sock != INVALID_SOCKET) {
		closesocket(g_listen_sock);
		g_listen_sock = INVALID_SOCKET;
	}
	if (g_thread) {
		WaitForSingleObject(g_thread, 5000);
		CloseHandle(g_thread);
		g_thread = NULL;
	}
	EnterCriticalSection(&g_sse_cs);
	for (size_t i = 0; i < g_sse_sockets.size(); i++)
		closesocket(g_sse_sockets[i]);
	g_sse_sockets.clear();
	LeaveCriticalSection(&g_sse_cs);
	DeleteCriticalSection(&g_sse_cs);
}

void mcp_http_sse_broadcast(const char* event_data)
{
	if (!event_data) return;
	char line[4096];
	int len = snprintf(line, sizeof(line), "data: %s\n\n", event_data);
	if (len <= 0 || len >= (int)sizeof(line)) return;
	EnterCriticalSection(&g_sse_cs);
	for (size_t i = 0; i < g_sse_sockets.size(); i++) {
		if (send(g_sse_sockets[i], line, len, 0) <= 0) {
			closesocket(g_sse_sockets[i]);
			g_sse_sockets[i] = g_sse_sockets.back();
			g_sse_sockets.pop_back();
			i--;
		}
	}
	LeaveCriticalSection(&g_sse_cs);
}

#else
/* Non-Windows: stub */
void mcp_http_start(int port, mcp_http_process_fn process_cb) { (void)port; (void)process_cb; }
void mcp_http_stop(void) {}
void mcp_http_sse_broadcast(const char* event_data) { (void)event_data; }
#endif
