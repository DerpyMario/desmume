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
#include "../GPU.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static mcp_set_execute_fn g_set_execute;
static mcp_get_execute_fn g_get_execute;

/* When set, send_response/send_error write to this buffer instead of stdout (for HTTP). */
static char* g_http_response_buf = NULL;
static size_t g_http_response_size = 0;

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

static int get_arg_bool(const char* json, const char* key, int* out, int default_val)
{
	const char* args = strstr(json, "\"arguments\"");
	if (!args) { *out = default_val; return -1; }
	args = strchr(args, '{');
	if (!args) { *out = default_val; return -1; }
	char search[80];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char* p = strstr(args, search);
	if (!p) { *out = default_val; return -1; }
	p += strlen(search);
	while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
	if (strncmp(p, "true", 4) == 0) { *out = 1; return 0; }
	if (strncmp(p, "false", 5) == 0) { *out = 0; return 0; }
	*out = (int)strtol(p, NULL, 0);
	return 0;
}

static int str_ieq(const char* a, const char* b)
{
	if (!a || !b) return 0;
	for (; *a && *b; a++, b++) {
		char ca = (char)((*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a);
		char cb = (char)((*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b);
		if (ca != cb) return 0;
	}
	return *a == *b;
}

static const char kBase64Table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const u8* data, size_t len)
{
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		unsigned int n = (unsigned int)data[i] << 16;
		if (i + 1 < len) n |= (unsigned int)data[i + 1] << 8;
		if (i + 2 < len) n |= (unsigned int)data[i + 2];
		out.push_back(kBase64Table[(n >> 18) & 63]);
		out.push_back(kBase64Table[(n >> 12) & 63]);
		out.push_back((i + 1 < len) ? kBase64Table[(n >> 6) & 63] : '=');
		out.push_back((i + 2 < len) ? kBase64Table[n & 63] : '=');
	}
	return out;
}

static void escape_json_string(const char* in, char* out, size_t out_size)
{
	size_t j = 0;
	for (; in && *in && j + 2 < out_size; in++) {
		if (*in == '"' || *in == '\\') { out[j++] = '\\'; out[j++] = *in; }
		else if ((unsigned char)*in < 32) { j += (size_t)snprintf(out + j, out_size - j, "\\u%04x", (unsigned char)*in); }
		else out[j++] = *in;
	}
	out[j] = '\0';
}

static void send_response(const char* id_str, int id_num, int use_id_num, const char* result_json)
{
	if (g_http_response_buf && g_http_response_size > 0) {
		int n = use_id_num
			? snprintf(g_http_response_buf, g_http_response_size, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id_num, result_json)
			: snprintf(g_http_response_buf, g_http_response_size, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":%s}", id_str, result_json);
		if (n < 0 || (size_t)n >= g_http_response_size) n = (int)(g_http_response_size - 1);
		g_http_response_buf[n] = '\0';
		return;
	}
	if (use_id_num)
		fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id_num, result_json);
	else
		fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":%s}\n", id_str, result_json);
}

static void send_error(const char* id_str, int id_num, int use_id_num, int code, const char* message)
{
	if (g_http_response_buf && g_http_response_size > 0) {
		char escaped[512];
		escape_json_string(message, escaped, sizeof(escaped));
		int n = use_id_num
			? snprintf(g_http_response_buf, g_http_response_size, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":\"%s\"}}", id_num, code, escaped)
			: snprintf(g_http_response_buf, g_http_response_size, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"error\":{\"code\":%d,\"message\":\"%s\"}}", id_str, code, escaped);
		if (n < 0 || (size_t)n >= g_http_response_size) n = (int)(g_http_response_size - 1);
		g_http_response_buf[n] = '\0';
		return;
	}
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
		",{\"name\":\"nds_screenshot\",\"description\":\"Capture the current display. screen: both|main|touch (default both). Optional path saves BMP to file instead of returning inline image.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"screen\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}}}}"
		",{\"name\":\"nds_input_key\",\"description\":\"Press or release a DS button. button: A|B|X|Y|start|select|up|down|left|right|L|R|debug|lid. pressed: true (default) or false.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"button\":{\"type\":\"string\"},\"pressed\":{\"type\":\"boolean\"}},\"required\":[\"button\"]}}"
		",{\"name\":\"nds_input_touch\",\"description\":\"Touch the bottom screen at pixel (x,y). Coordinates: x 0-255, y 0-191 (native touch screen). touch: true (default) press, false release.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"touch\":{\"type\":\"boolean\"}},\"required\":[\"x\",\"y\"]}}"
		",{\"name\":\"nds_input_release_all\",\"description\":\"Release all DS buttons and touch.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
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

static void convert_native15_to_rgb24(const u16* src, u8* dst, int pixel_count)
{
	for (int i = 0; i < pixel_count; i++) {
		u16 p = src[i];
		dst[i * 3 + 0] = (u8)(((p >> 0) & 0x1f) << 3);
		dst[i * 3 + 1] = (u8)(((p >> 5) & 0x1f) << 3);
		dst[i * 3 + 2] = (u8)(((p >> 10) & 0x1f) << 3);
	}
}

static std::vector<u8> build_bmp24(const u8* rgb, int width, int height)
{
	const int row_stride = ((width * 3 + 3) / 4) * 4;
	const int data_size = row_stride * height;
	const int file_size = 54 + data_size;
	std::vector<u8> bmp((size_t)file_size, 0);

	bmp[0] = 'B';
	bmp[1] = 'M';
	*(u32*)&bmp[2] = (u32)file_size;
	*(u32*)&bmp[10] = 54;
	*(u32*)&bmp[14] = 40;
	*(u32*)&bmp[18] = (u32)width;
	*(u32*)&bmp[22] = (u32)height;
	*(u16*)&bmp[26] = 1;
	*(u16*)&bmp[28] = 24;
	*(u32*)&bmp[34] = (u32)data_size;

	u8* dst = &bmp[54];
	for (int y = height - 1; y >= 0; y--) {
		const u8* src_row = rgb + (size_t)y * (size_t)width * 3;
		memcpy(dst, src_row, (size_t)width * 3);
		dst += width * 3;
		for (int pad = row_stride - width * 3; pad > 0; pad--)
			*dst++ = 0;
	}
	return bmp;
}

static bool save_bmp_file(const char* path, const u8* rgb, int width, int height)
{
	std::vector<u8> bmp = build_bmp24(rgb, width, height);
	FILE* fp = fopen(path, "wb");
	if (!fp) return false;
	size_t wrote = fwrite(&bmp[0], 1, bmp.size(), fp);
	fclose(fp);
	return wrote == bmp.size();
}

static void tool_nds_screenshot(const char* json, std::string& result)
{
	if (!GPU) {
		result = "{\"content\":[{\"type\":\"text\",\"text\":\"error: GPU not ready\"}]}";
		return;
	}

	char screen[16] = "both";
	char path[512] = {0};
	get_arg_str(json, "screen", screen, sizeof(screen));
	get_arg_str(json, "path", path, sizeof(path));

	const NDSDisplayInfo& disp = GPU->GetDisplayInfo();
	const u16* fb = disp.masterNativeBuffer16;
	if (!fb) {
		result = "{\"content\":[{\"type\":\"text\",\"text\":\"error: no framebuffer\"}]}";
		return;
	}

	const int full_w = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const int screen_h = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	const int full_h = screen_h * 2;
	const int pixels_per_screen = full_w * screen_h;

	const u16* src = fb;
	int width = full_w;
	int height = full_h;

	if (str_ieq(screen, "main")) {
		height = screen_h;
	} else if (str_ieq(screen, "touch")) {
		src = fb + pixels_per_screen;
		height = screen_h;
	} else if (!str_ieq(screen, "both")) {
		result = "{\"content\":[{\"type\":\"text\",\"text\":\"error: screen must be both, main, or touch\"}]}";
		return;
	}

	const int pixel_count = width * height;
	std::vector<u8> rgb((size_t)pixel_count * 3);
	convert_native15_to_rgb24(src, &rgb[0], pixel_count);

	if (path[0]) {
		if (!save_bmp_file(path, &rgb[0], width, height)) {
			result = "{\"content\":[{\"type\":\"text\",\"text\":\"error: failed to write BMP\"}]}";
			return;
		}
		char msg[640];
		snprintf(msg, sizeof(msg), "OK: saved %dx%d BMP to %s", width, height, path);
		char escaped[640];
		escape_json_string(msg, escaped, sizeof(escaped));
		result = std::string("{\"content\":[{\"type\":\"text\",\"text\":\"") + escaped + "\"}]}";
		return;
	}

	std::vector<u8> bmp = build_bmp24(&rgb[0], width, height);
	std::string b64 = base64_encode(&bmp[0], bmp.size());
	char dim[64];
	snprintf(dim, sizeof(dim), "%dx%d", width, height);
	char dim_esc[64];
	escape_json_string(dim, dim_esc, sizeof(dim_esc));

	result.reserve(64 + b64.size());
	result = "{\"content\":[{\"type\":\"image\",\"data\":\"";
	result += b64;
	result += "\",\"mimeType\":\"image/bmp\"},{\"type\":\"text\",\"text\":\"";
	result += dim_esc;
	result += "\"}]}";
}

static bool set_button_by_name(UserButtons& buttons, const char* name, bool pressed)
{
	if (str_ieq(name, "A")) buttons.A = pressed;
	else if (str_ieq(name, "B")) buttons.B = pressed;
	else if (str_ieq(name, "X")) buttons.X = pressed;
	else if (str_ieq(name, "Y")) buttons.Y = pressed;
	else if (str_ieq(name, "start")) buttons.S = pressed;
	else if (str_ieq(name, "select")) buttons.T = pressed;
	else if (str_ieq(name, "up")) buttons.U = pressed;
	else if (str_ieq(name, "down")) buttons.D = pressed;
	else if (str_ieq(name, "left")) buttons.L = pressed;
	else if (str_ieq(name, "right")) buttons.R = pressed;
	else if (str_ieq(name, "L")) buttons.W = pressed;
	else if (str_ieq(name, "R")) buttons.E = pressed;
	else if (str_ieq(name, "debug")) buttons.G = pressed;
	else if (str_ieq(name, "lid")) buttons.F = pressed;
	else return false;
	return true;
}

static void apply_user_buttons(const UserButtons& buttons)
{
	NDS_setPad(
		buttons.R, buttons.L, buttons.D, buttons.U,
		buttons.T, buttons.S, buttons.B, buttons.A,
		buttons.Y, buttons.X, buttons.W, buttons.E,
		buttons.G, buttons.F);
	NDS_beginProcessingInput();
	NDS_getProcessingUserInput().buttons = buttons;
	NDS_endProcessingInput();
}

static u16 clamp_touch_coord(int value, int maximum)
{
	if (value < 0) value = 0;
	if (value >= maximum) value = maximum - 1;
	return (u16)value;
}

static void apply_user_touch(u16 x, u16 y, bool touch)
{
	if (touch) {
		NDS_setTouchPos(x, y);
	} else {
		NDS_releaseTouch();
	}
	NDS_beginProcessingInput();
	UserTouch& t = NDS_getProcessingUserInput().touch;
	if (touch) {
		t.touchX = (u16)((x << 4) & 0x0FF0);
		t.touchY = (u16)((y << 4) & 0x0FF0);
		t.isTouch = true;
	} else {
		t.touchX = 0;
		t.touchY = 0;
		t.isTouch = false;
	}
	NDS_endProcessingInput();
}

static void tool_nds_input_touch(const char* json, char* result, size_t result_size)
{
	int x = 0, y = 0;
	int touch_on = 1;
	if (get_arg_int(json, "x", &x) != 0 || get_arg_int(json, "y", &y) != 0) {
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing x or y\"}]}");
		return;
	}
	get_arg_bool(json, "touch", &touch_on, 1);

	if (touch_on) {
		u16 px = clamp_touch_coord(x, GPU_FRAMEBUFFER_NATIVE_WIDTH);
		u16 py = clamp_touch_coord(y, GPU_FRAMEBUFFER_NATIVE_HEIGHT);
		apply_user_touch(px, py, true);
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: touch at (%u,%u)\"}]}", (unsigned)px, (unsigned)py);
	} else {
		apply_user_touch(0, 0, false);
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: touch released\"}]}");
	}
}

static void tool_nds_input_key(const char* json, char* result, size_t result_size)
{
	char button[32] = {0};
	if (get_arg_str(json, "button", button, sizeof(button)) != 0) {
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: missing button\"}]}");
		return;
	}

	int pressed = 1;
	get_arg_bool(json, "pressed", &pressed, 1);

	UserButtons buttons = NDS_getRawUserInput().buttons;
	if (!set_button_by_name(buttons, button, pressed != 0)) {
		snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"error: unknown button (A,B,X,Y,start,select,up,down,left,right,L,R,debug,lid)\"}]}");
		return;
	}

	apply_user_buttons(buttons);
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: %s %s\"}]}", button, pressed ? "pressed" : "released");
}

static void tool_nds_input_release_all(char* result, size_t result_size)
{
	UserButtons buttons = {};
	apply_user_buttons(buttons);
	NDS_releaseTouch();
	snprintf(result, result_size, "{\"content\":[{\"type\":\"text\",\"text\":\"OK: all keys released\"}]}");
}

static void handle_tools_call(const char* json, char* id_str, int id_num, int use_id_num)
{
	char name[64] = {0};
	find_json_string_val(json, "name", name, sizeof(name));
	std::string result_large;
	char result_buf[1024];
	const char* result_ptr = result_buf;

	if (strcmp(name, "nds_screenshot") == 0) {
		tool_nds_screenshot(json, result_large);
		result_ptr = result_large.c_str();
	} else if (strcmp(name, "nds_pause") == 0)
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
	else if (strcmp(name, "nds_input_key") == 0)
		tool_nds_input_key(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_input_touch") == 0)
		tool_nds_input_touch(json, result_buf, sizeof(result_buf));
	else if (strcmp(name, "nds_input_release_all") == 0)
		tool_nds_input_release_all(result_buf, sizeof(result_buf));
	else
		snprintf(result_buf, sizeof(result_buf), "{\"content\":[{\"type\":\"text\",\"text\":\"unknown tool: %s\"}]}", name);

	/* MCP tools/call result: { "content": [ { "type": "text", "text": "..." } ] } */
	send_response(id_str, id_num, use_id_num, result_ptr);
}

void mcp_server_process_line_http(const char* line_buf, char* out_buf, size_t out_size)
{
	if (!out_buf || out_size == 0) return;
	g_http_response_buf = out_buf;
	g_http_response_size = out_size;
	out_buf[0] = '\0';
	mcp_server_process_line(line_buf);
	g_http_response_buf = NULL;
	g_http_response_size = 0;
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
