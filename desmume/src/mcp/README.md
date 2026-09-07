# DeSmuME MCP server

An [MCP](https://modelcontextprotocol.io) server built into the emulator, so an MCP
client can load a ROM, drive the buttons and the touch screen, take screenshots and
poke at memory, registers and breakpoints.

## Running it

The server drives emulation itself, so it runs headless.

### Linux / BSD (`desmume-cli`)

```sh
# newline delimited JSON-RPC on stdin/stdout (what most MCP clients launch)
desmume-cli --mcp game.nds

# JSON-RPC over HTTP instead, for clients that connect to a running emulator
desmume-cli --mcp-port 8765 game.nds
```

The ROM is optional: without one the server starts idle and waits for `nds_load_rom`.

`--headless` on its own runs the emulator with no window and no MCP server, which is
useful for scripted runs; `--mcp` implies it.

With the stdio transport, stdout carries the protocol and everything the emulator
would normally print there is redirected to stderr.

### Windows

The Windows build is a GUI subsystem program with no usable stdio, so it always uses
the HTTP transport:

```
DeSmuME.exe --mcp game.nds              # http://127.0.0.1:8765/mcp
DeSmuME.exe --mcp-port 9000 game.nds
```

Tools ▸ Start MCP Server does the same thing from the GUI, in a new console window.

### HTTP transport details

The server listens on 127.0.0.1 only. `POST /mcp` (or `POST /`) carries a JSON-RPC
message and answers with the JSON-RPC response, or `202 Accepted` with an empty body
for a notification. `GET` is answered with `405` because there is no server initiated
stream, `DELETE` with `204`, and CORS preflights are answered.

## Client configuration

```json
{
  "mcpServers": {
    "desmume": {
      "command": "desmume-cli",
      "args": ["--mcp", "/path/to/game.nds"]
    }
  }
}
```

## Tools

| Tool | What it does |
| --- | --- |
| `nds_get_state` | Running/paused, frame counter, both PCs, loaded ROM |
| `nds_pause` / `nds_resume` | Stop and start emulation |
| `nds_step` | Single step instructions (`count`) |
| `nds_step_over` | Step one instruction, running a call or SWI to completion instead of into it |
| `nds_step_out` | Run until the current function returns to its caller |
| `nds_run_frames` | Run exactly N frames and return, which makes scripted play deterministic |
| `nds_reset` | Reset the console |
| `nds_quit` | Shut the emulator down |
| `nds_load_rom` / `nds_reload_rom` / `nds_get_rom_info` | ROM handling |
| `nds_read_memory` / `nds_write_memory` | Hex dump and write bytes (`proc` selects ARM9/ARM7) |
| `nds_search_memory` | Find a byte pattern in a memory range |
| `nds_get_registers` / `nds_set_register` | ARM register file |
| `nds_disassemble` | Disassemble ARM or THUMB at an address |
| `nds_set_breakpoint` / `nds_clear_breakpoint` / `nds_clear_all_breakpoints` / `nds_list_breakpoints` | Execute, read and write breakpoints |
| `nds_save_state` / `nds_load_state` | Savestates, to a file or a numbered slot |
| `nds_screenshot` | PNG (or BMP) of both screens, the main screen or the touch screen; inline or written to a file |
| `nds_input_key` / `nds_input_touch` / `nds_input_release_all` | Buttons and touch screen, optionally held for a number of frames |

Addresses are hex by default, so `"02000000"` and `"0x02000000"` mean the same thing.

Breakpoints pause emulation wherever DeSmuME runs. `nds_get_state` reports what
stopped it (`stopped on execute breakpoint at 0x0200000C on ARM9`), and so do
`nds_run_frames` and `nds_step` when they end early. Resuming runs past the
breakpoint that stopped you instead of stopping on it again, so a breakpoint inside a
loop yields one iteration per resume.

Reads and writes the debugger itself performs (`nds_read_memory`, `nds_write_memory`,
`nds_search_memory`) never trip read or write breakpoints, only the emulated CPUs do.

`nds_step_over` looks at the instruction at the program counter: `BL`, `BLX`, and
`SWI` (in ARM and in THUMB) are run to completion and execution stops on the
instruction after them; anything else is an ordinary single step. `nds_step_out` runs
until the stack frame the CPU is in has been released. Both ignore calls made from
within the code they are running through, so recursion and nested calls do not end
them early, and both stop on a breakpoint if one is hit first and say so. Neither can
run forever: `max_frames` (120 by default) bounds the wait, and they report it when
they give up.

A typical scripted interaction presses a button for a few frames and then looks at the
result:

```json
{"name": "nds_input_key",   "arguments": {"button": "start", "frames": 4}}
{"name": "nds_run_frames",  "arguments": {"frames": 60}}
{"name": "nds_screenshot",  "arguments": {"screen": "main"}}
```

## How it fits together

| File | Role |
| --- | --- |
| `mcp_json.cpp` | Small JSON reader, so no new dependency is pulled into the core |
| `mcp_server.cpp` | Protocol handling and the tools themselves |
| `mcp_http.cpp` | HTTP transport (Winsock on Windows, BSD sockets elsewhere) |

Every tool touches emulator state, which is not thread safe, so a transport never runs
one itself: `mcp_server_dispatch()` hands the request to the emulation thread and waits,
and the emulation loop picks it up in `mcp_server_poll()`. A frontend therefore only has
to call `mcp_server_init()`, start a transport, and call `mcp_server_poll()` once per
frame (and with a small timeout while paused, so requests are still answered).
