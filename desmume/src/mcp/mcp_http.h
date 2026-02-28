/*
	Copyright (C) 2025 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/

#ifndef MCP_HTTP_H
#define MCP_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Process callback: (request_body, response_buffer, response_size). Callback blocks until response is filled. */
typedef void (*mcp_http_process_fn)(const char* body, char* resp_buf, size_t resp_size);

/* Start HTTP server on 127.0.0.1:port. POST /mcp -> JSON-RPC; GET /mcp -> SSE. Runs in a background thread. */
void mcp_http_start(int port, mcp_http_process_fn process_cb);

/* Stop server and join thread. */
void mcp_http_stop(void);

/* Send SSE event to all connected GET clients. Form: "data: " + event_data + "\n\n". */
void mcp_http_sse_broadcast(const char* event_data);

#ifdef __cplusplus
}
#endif

#endif
