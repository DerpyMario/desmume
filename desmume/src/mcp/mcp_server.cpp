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

#include "mcp_server.h"
#include "../NDSSystem.h"
#include "../MMU.h"
#include "../armcpu.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static mcp_set_execute_fn g_set_execute;
static mcp_get_execute_fn g_get_execute;

void mcp_server_init(mcp_set_execute_fn set_execute, mcp_get_execute_fn get_execute)
{
	g_set_execute = set_execute;
	g_get_execute = get_execute;
}

/* Minimal JSON-RPC parsing: find "method":"...", "id":... , "params":{...} */
static const char* find_json_string_val(const char* json, const char* key, char* out, size_t out_size)
{
	char search[80];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char* p = strstr(json, search);
	if (!p) return NULL;
	p += strlen(search);
	while (*p && (*p == ' ' || *p == '\t')) p++;
	if (*p != ':') return NULL;
	p++;
	while (*p && (*p == ' ' || *p == '\t')) p++;
	if (*p != '"') return NULL;
	p++;
	const char* start = p;
	while (*p && *p != '"') {
		if (*p == '\\') p++;
		p++;
	}
	size_t len = (size_t)(p - start);
	if (len >= out_size) len = out_size - 1;
	memcpy(out, start, len);
	out[len] = '\0';
	return p;
}

static int find_json_int_val(const char* json, const char* key, int* out)
{
	char search[64];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char* p = strstr(json, search);
	if (!p) return -1;
	p += strlen(search);
	while (*p && (*p == ' ' || *p == '\t')) p++;
	if (*p != ':') return -1;
	p++;
	while (*p && (*p == ' ' || *p == '\t')) p++;
	*out = (int)strtol(p, NULL, 10);
	return 0;
}

/* Get "arguments" object and then key from it (for tools/call). */
static int get_arg_int(const char* json, const char* key, int* out)
{
	const char* args = strstr(json, "\"arguments\"");
	if (!args) return -1;
	args = strchr(args, '{');
	if (!args) return -1;
	char search[80];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char* p = strstr(args, search);
	if (!p) return -1;
	p += strlen(search);
	while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
	*out = (int)strtol(p, NULL, 0);
	return 0;
}

static int get_arg_str(const char* json, const char* key, char* out, size_t out_size)
{
	const char* args = strstr(json, "\"arguments\"");
	if (!args) return -1;
	args = strchr(args, '{');
	if (!args) return -1;
	char search[80];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char* p = strstr(args, search);
	if (!p) return -1;
	p += strlen(search);
	while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
	if (*p != '"') return -1;
	p++;
	const char* start = p;
	while (*p && *p != '"') { if (*p == '\\') p++; p++; }
	size_t len = (size_t)(p - start);
	if (len >= out_size) len = out_size - 1;
	memcpy(out, start, len);
	out[len] = '\0';
	return 0;
}

static void send_response(const char* id_str, int id_num, int use_id_num, const char* result_json)
{
	if (use_id_num)
		fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id_num, result_json);
	else
		fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":%s}\n", id_str, result_json);
}

static void send_error(const char* id_str, int id_num, int use_id_num, int code, const char* message)
{
	if (use_id_num)
		fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}\n", id_num, code, message);
	else
		fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"error\":{\"code\":%d,\"message\":\"%s\"}}\n", id_str, code, message);
}

static void handle_initialize(const char* json, char* id_str, int id_num, int use_id_num)
{
	/* Result: { "protocolVersion": "2024-11-05", "capabilities": { "tools": {} }, "serverInfo": { "name": "desmume-mcp", "version": "0.1.0" } } */
	send_response(id_str, id_num, use_id_num,
		"{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"desmume-mcp\",\"version\":\"0.1.0\"}}");
}

static void handle_tools_list(const char* json, char* id_str, int id_num, int use_id_num)
{
	const char* tools = "{\"tools\":["
		"{\"name\":\"nds_pause\",\"description\":\"Pause NDS emulation (break into debugger)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_resume\",\"description\":\"Resume NDS emulation\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_step\",\"description\":\"Single step one instruction (both CPUs)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_read_memory\",\"description\":\"Read memory from NDS address space. proc: 0=ARM9, 1=ARM7; address and size in hex.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"proc\":{\"type\":\"integer\"},\"address\":{\"type\":\"string\"},\"size\":{\"type\":\"integer\"}},\"required\":[\"proc\",\"address\",\"size\"]}}"
		",{\"name\":\"nds_write_memory\",\"description\":\"Write bytes to NDS memory. proc: 0=ARM9, 1=ARM7; address in hex, value hex string (e.g. AABBCCDD for 4 bytes).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"proc\":{\"type\":\"integer\"},\"address\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"proc\",\"address\",\"value\"]}}"
		",{\"name\":\"nds_get_registers\",\"description\":\"Get ARM registers for one CPU. proc: 0=ARM9, 1=ARM7.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"proc\":{\"type\":\"integer\"}},\"required\":[\"proc\"]}}"
		",{\"name\":\"nds_get_state\",\"description\":\"Get current emulator state: running, PC ARM9/ARM7, ROM info.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_load_rom\",\"description\":\"Load a NDS ROM file. path: file path.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}"
		",{\"name\":\"nds_reload_rom\",\"description\":\"Reload the currently loaded ROM (same path as last load). Fails if no ROM was loaded.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_reset\",\"description\":\"Reset the NDS console.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_get_rom_info\",\"description\":\"Get loaded ROM title and code.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_set_breakpoint\",\"description\":\"Set a breakpoint (can be set before loading ROM). type: execute|read|write; proc: 0=ARM9, 1=ARM7 (for execute); address: hex (e.g. 02000000).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\"},\"proc\":{\"type\":\"integer\"},\"address\":{\"type\":\"string\"}},\"required\":[\"type\",\"address\"]}}"
		",{\"name\":\"nds_clear_breakpoint\",\"description\":\"Clear one breakpoint. Same params as set (type, proc for execute, address in hex).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\"},\"proc\":{\"type\":\"integer\"},\"address\":{\"type\":\"string\"}},\"required\":[\"type\",\"address\"]}}"
		",{\"name\":\"nds_clear_all_breakpoints\",\"description\":\"Clear all breakpoints (execute, read, write).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		",{\"name\":\"nds_list_breakpoints\",\"description\":\"List all breakpoints (execute ARM9/ARM7, read, write).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
		"]}";
	send_response(id_str, id_num, use_id_num, tools);
}

static void tool_nds_pause(char* result, size_t result_size)
{
	if (g_set_execute) g_set_execute(0);
	NDS_debug_break();
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: paused\"}]}");
}

static void tool_nds_resume(char* result, size_t result_size)
{
	if (g_set_execute) g_set_execute(1);
	NDS_debug_continue();
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: running\"}]}");
}

static void tool_nds_step(char* result, size_t result_size)
{
	if (g_set_execute) g_set_execute(1);
	NDS_debug_step();
	NDS_ARM9.debugStep = true;
	NDS_ARM7.debugStep = true;
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: step\"}]}");
}

static void tool_nds_read_memory(const char* json, char* result, size_t result_size)
{
	int proc = 0;
	int size = 4;
	char addr_str[32] = {0};
	if (get_arg_int(json, "proc", &proc) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing proc\"}]}"); return; }
	if (get_arg_str(json, "address", addr_str, sizeof(addr_str)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing address\"}]}"); return; }
	get_arg_int(json, "size", &size);
	if (size <= 0 || size > 256) size = 4;
	u32 addr = (u32)strtoul(addr_str, NULL, 16);
	int p = (proc != 0) ? 1 : 0;
	char hex[600];
	char* h = hex;
	*h = '\0';
	for (int i = 0; i < size; i += 4) {
		u32 v = MMU_read32(p, addr + i);
		h += sprintf(h, "%08X ", (unsigned)v);
	}
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", hex);
}

static void tool_nds_write_memory(const char* json, char* result, size_t result_size)
{
	int proc = 0;
	char addr_str[32] = {0}, val_str[256] = {0};
	if (get_arg_int(json, "proc", &proc) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing proc\"}]}"); return; }
	if (get_arg_str(json, "address", addr_str, sizeof(addr_str)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing address\"}]}"); return; }
	if (get_arg_str(json, "value", val_str, sizeof(val_str)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing value\"}]}"); return; }
	u32 addr = (u32)strtoul(addr_str, NULL, 16);
	int p = (proc != 0) ? 1 : 0;
	size_t len = strlen(val_str);
	if (len & 1) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: value must be even hex length\"}]}"); return; }
	for (size_t i = 0; i < len; i += 2) {
		char byte[3] = { val_str[i], val_str[i+1], '\0' };
		u8 b = (u8)strtoul(byte, NULL, 16);
		MMU_write8(p, addr + (u32)(i/2), b);
	}
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: written %zu bytes\"}]}", len/2);
}

static void tool_nds_get_registers(const char* json, char* result, size_t result_size)
{
	int proc = 0;
	get_arg_int(json, "proc", &proc);
	armcpu_t* cpu = (proc != 0) ? &NDS_ARM7 : &NDS_ARM9;
	char buf[512];
	char* b = buf;
	b += sprintf(b, "PC=0x%08X CPSR=0x%08X ", cpu->instruct_adr, (unsigned)cpu->CPSR.val);
	for (int i = 0; i < 16; i++)
		b += sprintf(b, "R%d=0x%08X ", i, (unsigned)cpu->R[i]);
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", buf);
}

static void tool_nds_get_state(char* result, size_t result_size)
{
	int running = g_get_execute ? g_get_execute() : 0;
	char buf[400];
	snprintf(buf, sizeof(buf),
		"running=%d ARM9_PC=0x%08X ARM7_PC=0x%08X",
		running,
		(unsigned)NDS_ARM9.instruct_adr,
		(unsigned)NDS_ARM7.instruct_adr);
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", buf);
}

static void tool_nds_load_rom(const char* json, char* result, size_t result_size)
{
	char path[512] = {0};
	if (get_arg_str(json, "path", path, sizeof(path)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing path\"}]}"); return; }
	int ret = NDS_LoadROM(path, NULL, NULL);
	if (ret < 0)
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: load failed %d\"}]}", ret);
	else
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: loaded\"}]}");
}

static void tool_nds_reload_rom(char* result, size_t result_size)
{
	const char* path = NDS_GetLastRomPath();
	if (!path || !path[0]) {
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: no ROM loaded to reload\"}]}");
		return;
	}
	int ret = NDS_LoadROM(path, NULL, NULL);
	if (ret < 0)
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: reload failed %d\"}]}", ret);
	else
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: reloaded\"}]}");
}

static void tool_nds_reset(char* result, size_t result_size)
{
	NDS_Reset();
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: reset\"}]}");
}

static void tool_nds_get_rom_info(char* result, size_t result_size)
{
	NDS_header* h = NDS_getROMHeader();
	if (!h) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"no ROM loaded\"}]}"); return; }
	char title[13];
	memcpy(title, h->gameTile, 12);
	title[12] = '\0';
	char code[5];
	memcpy(code, h->gameCode, 4);
	code[4] = '\0';
	char buf[256];
	snprintf(buf, sizeof(buf), "title=%s code=%s", title, code);
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", buf);
}

static void tool_nds_set_breakpoint(const char* json, char* result, size_t result_size)
{
	char type_str[32] = {0}, addr_str[32] = {0};
	int proc = 0;
	if (get_arg_str(json, "type", type_str, sizeof(type_str)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing type (execute|read|write)\"}]}"); return; }
	if (get_arg_str(json, "address", addr_str, sizeof(addr_str)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing address\"}]}"); return; }
	get_arg_int(json, "proc", &proc);
	u32 addr = (u32)strtoul(addr_str, NULL, 16);

	if (strcmp(type_str, "execute") == 0) {
		std::vector<u32>* bp = (proc != 0) ? NDS_ARM7.breakPoints : NDS_ARM9.breakPoints;
		if (!bp) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: CPU breakpoints not ready\"}]}"); return; }
		for (size_t i = 0; i < bp->size(); i++) if ((*bp)[i] == addr) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: already set\"}]}"); return; }
		bp->push_back(addr);
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: execute breakpoint at 0x%08X %s\"}]}", (unsigned)addr, proc ? "ARM7" : "ARM9");
	} else if (strcmp(type_str, "read") == 0) {
		for (size_t i = 0; i < memReadBreakPoints.size(); i++) if (memReadBreakPoints[i] == addr) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: already set\"}]}"); return; }
		memReadBreakPoints.push_back(addr);
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: read breakpoint at 0x%08X\"}]}", (unsigned)addr);
	} else if (strcmp(type_str, "write") == 0) {
		for (size_t i = 0; i < memWriteBreakPoints.size(); i++) if (memWriteBreakPoints[i] == addr) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: already set\"}]}"); return; }
		memWriteBreakPoints.push_back(addr);
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: write breakpoint at 0x%08X\"}]}", (unsigned)addr);
	} else {
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: type must be execute, read, or write\"}]}");
	}
}

static void tool_nds_clear_breakpoint(const char* json, char* result, size_t result_size)
{
	char type_str[32] = {0}, addr_str[32] = {0};
	int proc = 0;
	if (get_arg_str(json, "type", type_str, sizeof(type_str)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing type\"}]}"); return; }
	if (get_arg_str(json, "address", addr_str, sizeof(addr_str)) != 0) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing address\"}]}"); return; }
	get_arg_int(json, "proc", &proc);
	u32 addr = (u32)strtoul(addr_str, NULL, 16);

	if (strcmp(type_str, "execute") == 0) {
		std::vector<u32>* bp = (proc != 0) ? NDS_ARM7.breakPoints : NDS_ARM9.breakPoints;
		if (!bp) { snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: CPU breakpoints not ready\"}]}"); return; }
		for (size_t i = 0; i < bp->size(); i++) {
			if ((*bp)[i] == addr) { bp->erase(bp->begin() + (std::ptrdiff_t)i); snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: cleared\"}]}"); return; }
		}
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"no such breakpoint\"}]}");
	} else if (strcmp(type_str, "read") == 0) {
		for (size_t i = 0; i < memReadBreakPoints.size(); i++) {
			if (memReadBreakPoints[i] == addr) { memReadBreakPoints.erase(memReadBreakPoints.begin() + (std::ptrdiff_t)i); snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: cleared\"}]}"); return; }
		}
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"no such breakpoint\"}]}");
	} else if (strcmp(type_str, "write") == 0) {
		for (size_t i = 0; i < memWriteBreakPoints.size(); i++) {
			if (memWriteBreakPoints[i] == addr) { memWriteBreakPoints.erase(memWriteBreakPoints.begin() + (std::ptrdiff_t)i); snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: cleared\"}]}"); return; }
		}
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"no such breakpoint\"}]}");
	} else {
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: type must be execute, read, or write\"}]}");
	}
}

static void tool_nds_clear_all_breakpoints(char* result, size_t result_size)
{
	if (NDS_ARM9.breakPoints) NDS_ARM9.breakPoints->clear();
	if (NDS_ARM7.breakPoints) NDS_ARM7.breakPoints->clear();
	memReadBreakPoints.clear();
	memWriteBreakPoints.clear();
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: all breakpoints cleared\"}]}");
}

static void tool_nds_list_breakpoints(char* result, size_t result_size)
{
	char buf[2048];
	char* b = buf;
	b += sprintf(b, "execute ARM9:");
	for (size_t i = 0; NDS_ARM9.breakPoints && i < NDS_ARM9.breakPoints->size(); i++)
		b += sprintf(b, " 0x%08X", (unsigned)(*NDS_ARM9.breakPoints)[i]);
	b += sprintf(b, " | execute ARM7:");
	for (size_t i = 0; NDS_ARM7.breakPoints && i < NDS_ARM7.breakPoints->size(); i++)
		b += sprintf(b, " 0x%08X", (unsigned)(*NDS_ARM7.breakPoints)[i]);
	b += sprintf(b, " | read:");
	for (size_t i = 0; i < memReadBreakPoints.size(); i++)
		b += sprintf(b, " 0x%08X", (unsigned)memReadBreakPoints[i]);
	b += sprintf(b, " | write:");
	for (size_t i = 0; i < memWriteBreakPoints.size(); i++)
		b += sprintf(b, " 0x%08X", (unsigned)memWriteBreakPoints[i]);
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", buf);
}

static void handle_tools_call(const char* json, char* id_str, int id_num, int use_id_num)
{
	char name[64] = {0};
	find_json_string_val(json, "name", name, sizeof(name));
	char result_buf[1024];
	if (strcmp(name, "nds_pause") == 0)
		tool_nds_pause(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_resume") == 0)
		tool_nds_resume(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_step") == 0)
		tool_nds_step(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_read_memory") == 0)
		tool_nds_read_memory(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_write_memory") == 0)
		tool_nds_write_memory(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_get_registers") == 0)
		tool_nds_get_registers(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_get_state") == 0)
		tool_nds_get_state(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_load_rom") == 0)
		tool_nds_load_rom(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_reload_rom") == 0)
		tool_nds_reload_rom(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_reset") == 0)
		tool_nds_reset(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_get_rom_info") == 0)
		tool_nds_get_rom_info(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_set_breakpoint") == 0)
		tool_nds_set_breakpoint(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_clear_breakpoint") == 0)
		tool_nds_clear_breakpoint(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_clear_all_breakpoints") == 0)
		tool_nds_clear_all_breakpoints(result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_list_breakpoints") == 0)
		tool_nds_list_breakpoints(result_buf, sizeof(result_buf));
	else
		snprintf(result_buf, sizeof(result_buf), "{\"content\":[{\"type\":\"text\",\"text\":\"unknown tool: %s\"}]}", name);

	/* MCP tools/call result: { "content": [ { "type": "text", "text": "..." } ] } */
	send_response(id_str, id_num, use_id_num, result_buf);
}

void mcp_server_process_line(const char* line_buf)
{
	char method[128] = {0};
	char id_str[64] = {0};
	int id_num = 0;
	int use_id_num = 0;
	find_json_string_val(line_buf, "method", method, sizeof(method));
	if (strstr(line_buf, "\"id\":")) {
		const char* idp = strstr(line_buf, "\"id\":");
		if (idp) {
			idp += 5;
			while (*idp == ' ') idp++;
			if (*idp == '"')
				find_json_string_val(line_buf, "id", id_str, sizeof(id_str));
			else {
				id_num = (int)strtol(idp, NULL, 10);
				use_id_num = 1;
			}
		}
	}

	if (strcmp(method, "initialize") == 0)
		handle_initialize(line_buf, id_str, id_num, use_id_num);
	else if (strcmp(method, "tools/list") == 0)
		handle_tools_list(line_buf, id_str, id_num, use_id_num);
	else if (strcmp(method, "tools/call") == 0)
		handle_tools_call(line_buf, id_str, id_num, use_id_num);
	else
		send_error(id_str, id_num, use_id_num, -32601, "Method not found");
}

void mcp_server_flush(void)
{
	fflush(stdout);
}
