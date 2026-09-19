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
| `nds_run_to_address` | Run to cursor: stop the moment execution reaches an address |
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
| `nds_dump_palette` | 256 colours as a `#RRGGBB` listing or a grid of swatches |
| `nds_dump_tiles` | Character data decoded into a grid of 8x8 tiles, as an image |
| `nds_dump_sprites` | An engine's object attribute memory, one line per sprite |
| `nds_input_key` / `nds_input_touch` / `nds_input_release_all` | Buttons and touch screen, optionally held for a number of frames |

Addresses are hex by default, so `"02000000"` and `"0x02000000"` mean the same thing.

Breakpoints pause emulation wherever DeSmuME runs. `nds_get_state` reports what
stopped it (`stopped on execute breakpoint at 0x0200000C on ARM9`), and so do
`nds_run_frames` and `nds_step` when they end early. Resuming runs past the
breakpoint that stopped you instead of stopping on it again, so a breakpoint inside a
loop yields one iteration per resume.

Reads and writes the debugger itself performs (`nds_read_memory`, `nds_write_memory`,
`nds_search_memory`) never trip read or write breakpoints, only the emulated CPUs do.

`nds_step` counts a THUMB `BL` as the one instruction it reads as, rather than
stopping between its two halfwords, and stepping does not move the frame counter.

`nds_step_over` looks at the instruction at the program counter: `BL`, `BLX`, and
`SWI` (in ARM and in THUMB) are run to completion and execution stops on the
instruction after them; anything else is an ordinary single step.

`nds_step_out` stops when the function returns, which it recognises either by the
stack frame being released or by execution reaching the return address the link
register held when the step was armed. The second is what makes stepping into a call
and straight back out work: a function that has not run its prologue yet returns to
the stack pointer it was entered with rather than past it. The flip side is that a
step out armed in the middle of a function that loops around a call can stop where
that call returns; step out again to leave the function.

Both ignore calls made from within the code they are running through, so recursion
and nested calls do not end them early, and a stack that moves by more than a frame's
worth is taken for a different stack, so an interrupt handler is not mistaken for a
return. Both stop on a breakpoint if one is hit first and say so. Neither can run
forever: `max_frames` (120 by default) bounds the wait, and they report it when they
give up.

`nds_run_to_address` is the run to cursor of a graphical debugger: it stops the first
time execution reaches the address, from wherever it gets there, so it can be used to
run into a function as well as out of one. It is one shot and leaves nothing behind,
which is what makes it different from setting a breakpoint and clearing it again.

## Looking at the graphics

The three dump tools read what the 2D engines are drawing with, through the debug side
of the MMU, so they see the VRAM bank mapping the game set up and disturb nothing.

`nds_dump_palette` reads one of the four standard palettes — `engine` main or sub,
`type` bg or obj — or any 256 colours you point `address` at. It answers with a
listing by default, or a grid of swatches with `format: "image"` or a `.png` path.

`nds_dump_tiles` decodes 8x8 character data into an image. Point it at a raw
`address`, or name a background with `bg` and let the engine's own registers say
where that layer's tiles live and how deep they are:

```json
{"name": "nds_dump_tiles", "arguments": {"bg": 0, "scale": 2, "path": "/tmp/bg0.png"}}
```
```
192 8bpp tile(s) from 0x06010000 with the main engine bg palette,
main engine BG0 (on, mode 0, 8bpp, tiles at 0x06010000, map at 0x06032000)
```

`nds_dump_sprites` lists an engine's object attribute memory, and heads the listing
with where that engine keeps its sprite character data and how tile numbers step
through it, which is what aims `nds_dump_tiles` at a particular sprite.

The same content is available without a client at all. `--dump` runs the ROM for a
while, writes the dumps and exits:

```sh
desmume-cli --dump all --dump-frames 700 --dump-dir shots game.nds
```

`--dump` takes any of `screen`, `palette`, `tiles`, `sprites` and `all`. It writes
`screen.png`, a swatch image per palette plus `palettes.txt`, a tile sheet per
background and per object engine, and `sprites.txt`. `--dump-frames` is how long to
run first and defaults to 60, which is rarely enough: most games need a few hundred
frames to get past their boot screens. `--dump-dir` has to exist. Dumping implies
`--headless`, and it cannot be combined with `--mcp`, which serves rather than exits.

Two things are worth knowing. Colour 0 is drawn as it is stored rather than as
transparency, because a tile viewer wants to see it. And where an engine has extended
palettes switched on, its colours come from VRAM rather than from palette memory: the
tools say so rather than quietly colouring from the wrong place, and `palette_address`
points the decoder at the colours you want.

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
| `mcp_image.cpp` | PNG and BMP encoding, and base64, for everything that returns a picture |
| `mcp_gfx.cpp` | Reading palettes, character data and sprites out of the 2D engines |
| `mcp_dump.cpp` | The same content written out as files, for the `--dump` command line |
| `mcp_server.cpp` | Protocol handling and the tools themselves |
| `mcp_http.cpp` | HTTP transport (Winsock on Windows, BSD sockets elsewhere) |

Every tool touches emulator state, which is not thread safe, so a transport never runs
one itself: `mcp_server_dispatch()` hands the request to the emulation thread and waits,
and the emulation loop picks it up in `mcp_server_poll()`. A frontend therefore only has
to call `mcp_server_init()`, start a transport, and call `mcp_server_poll()` once per
frame (and with a small timeout while paused, so requests are still answered).
