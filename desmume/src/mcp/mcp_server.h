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

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
	Model Context Protocol server.

	The JSON-RPC messages are decoded on whichever transport thread receives them,
	but every tool touches emulator state, so the actual work always happens on the
	thread that drives emulation: transports hand the request over with
	mcp_server_dispatch() and the emulation loop picks it up in mcp_server_poll().
*/

/* Callbacks provided by the frontend so MCP can control emulation. */
typedef void (*mcp_set_execute_fn)(int run);  /* 0 = pause, 1 = run */
typedef int  (*mcp_get_execute_fn)(void);

/* Initialize the server. Call once from the emulation thread before starting a transport. */
void mcp_server_init(mcp_set_execute_fn set_execute, mcp_get_execute_fn get_execute);

/* Stop every transport and release the request queue. */
void mcp_server_deinit(void);

/* --- emulation thread --- */

/*
	Run queued requests. When no request is pending, waits up to timeout_ms for one
	(pass 0 to poll without blocking, which is what a running emulation wants).
	Returns the number of requests handled.
*/
int mcp_server_poll(int timeout_ms);

/* Non-zero once a client asked the emulator to quit (nds_quit tool). */
int mcp_server_quit_requested(void);

/* --- transports --- */

/*
	Serve JSON-RPC over HTTP POST on 127.0.0.1:port (path / or /mcp), in a background
	thread. Returns 0 on success, non-zero when the port could not be bound.
*/
int mcp_server_start_http(int port);
void mcp_server_stop_http(void);

/*
	Serve newline delimited JSON-RPC on stdin/stdout in a background thread. To keep
	the protocol stream clean, stdout is redirected to stderr for the rest of the
	program and the original stdout is kept private for MCP responses.
	Returns 0 on success.
*/
int mcp_server_start_stdio(void);
void mcp_server_stop_stdio(void);

/*
	Claim stdout for the stdio transport straight away. mcp_server_start_stdio()
	does this too, but a frontend that only starts the transport once the emulator
	is up should call this first so that start up messages cannot corrupt the
	protocol stream. Calling it more than once is harmless.
*/
void mcp_server_capture_stdout(void);

/*
	Hand a request to the emulation thread and wait for its response. Returns a
	malloc()ed response string that the caller must free(), or NULL when the message
	needs no response (a JSON-RPC notification).
*/
char* mcp_server_dispatch(const char *request, int timeout_ms);

/*
	Process a request on the calling thread. Only safe to call from the emulation
	thread. Same return convention as mcp_server_dispatch().
*/
char* mcp_server_process_alloc(const char *request);

#ifdef __cplusplus
}
#endif

#endif
