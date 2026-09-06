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

#ifndef MCP_HTTP_H
#define MCP_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
	Called with the body of a POST request. Must return a malloc()ed response that
	the HTTP server frees, or NULL when the message needs no response (the server
	then answers 202 Accepted with an empty body).
*/
typedef char* (*mcp_http_process_fn)(const char *body);

/*
	Start the HTTP server on 127.0.0.1:port in a background thread.
	POST /mcp (or /) carries the JSON-RPC traffic.
	Returns 0 on success, non-zero when the port could not be bound.
*/
int mcp_http_start(int port, mcp_http_process_fn process_cb);

/* Stop the server and join its thread. Safe to call when not running. */
void mcp_http_stop(void);

/* Port the server is actually listening on, or 0 when not running. */
int mcp_http_port(void);

#ifdef __cplusplus
}
#endif

#endif
