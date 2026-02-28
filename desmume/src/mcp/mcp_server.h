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

#ifdef __cplusplus
extern "C" {
#endif

/* Callbacks provided by the frontend so MCP can control emulation. */
typedef void (*mcp_set_execute_fn)(int run);  /* 0 = pause, 1 = run */
typedef int  (*mcp_get_execute_fn)(void);

/* Initialize MCP server with stdio transport. Call once at startup when --mcp. */
void mcp_server_init(mcp_set_execute_fn set_execute, mcp_get_execute_fn get_execute);

/* Process one JSON-RPC request line (newline already stripped). Response is written to stdout. */
void mcp_server_process_line(const char* line);

/* Flush stdout after writing responses. */
void mcp_server_flush(void);

#ifdef __cplusplus
}
#endif

#endif
