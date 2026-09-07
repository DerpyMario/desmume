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
#include "mcp_http.h"
#include "mcp_json.h"

#include "../NDSSystem.h"
#include "../MMU.h"
#include "../SPU.h"
#include "../armcpu.h"
#include "../GPU.h"
#include "../movie.h"
#include "../saves.h"
#include "../frontend/modules/Disassembler.h"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
	#include <io.h>
	#define MCP_DUP _dup
	#define MCP_DUP2 _dup2
	#define MCP_FILENO _fileno
	#define MCP_FDOPEN _fdopen
#else
	#include <unistd.h>
	#define MCP_DUP dup
	#define MCP_DUP2 dup2
	#define MCP_FILENO fileno
	#define MCP_FDOPEN fdopen
#endif

/* Protocol versions this server understands, newest first. */
static const char *SUPPORTED_PROTOCOL_VERSIONS[] = { "2025-06-18", "2025-03-26", "2024-11-05" };
static const char *DEFAULT_PROTOCOL_VERSION = "2024-11-05";

static const char *SERVER_NAME = "desmume-mcp";
static const char *SERVER_VERSION = "1.0.0";

static const size_t MAX_MEMORY_READ = 4096;
static const int MAX_RUN_FRAMES = 3600;

static mcp_set_execute_fn g_setExecute = NULL;
static mcp_get_execute_fn g_getExecute = NULL;
static bool g_quitRequested = false;

//---------------------------------------------------------------------------
// JSON-RPC plumbing
//---------------------------------------------------------------------------

static std::string IDLiteral(const mcpjson::Value *id)
{
	if (id == NULL || id->IsNull())
		return "null";
	if (id->IsNumber())
		return id->raw;
	if (id->IsString())
		return mcpjson::Quote(id->str);
	if (id->IsBool())
		return id->boolean ? "true" : "false";
	return "null";
}

static std::string MakeResult(const std::string &idLiteral, const std::string &resultJSON)
{
	std::string out("{\"jsonrpc\":\"2.0\",\"id\":");
	out += idLiteral;
	out += ",\"result\":";
	out += resultJSON;
	out += "}";
	return out;
}

static std::string MakeError(const std::string &idLiteral, int code, const std::string &message)
{
	char codeText[32];
	snprintf(codeText, sizeof(codeText), "%d", code);

	std::string out("{\"jsonrpc\":\"2.0\",\"id\":");
	out += idLiteral;
	out += ",\"error\":{\"code\":";
	out += codeText;
	out += ",\"message\":";
	out += mcpjson::Quote(message);
	out += "}}";
	return out;
}

/* A tools/call result carrying a single block of text. */
static std::string TextResult(const std::string &text, bool isError = false)
{
	std::string out("{\"content\":[{\"type\":\"text\",\"text\":");
	out += mcpjson::Quote(text);
	out += "}],\"isError\":";
	out += isError ? "true" : "false";
	out += "}";
	return out;
}

static std::string ErrorResult(const std::string &text)
{
	return TextResult(text, true);
}

static std::string Format(const char *format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	const int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length < 0)
		return std::string();
	if ((size_t)length < sizeof(buffer))
		return std::string(buffer, (size_t)length);

	std::vector<char> large((size_t)length + 1);
	va_start(args, format);
	vsnprintf(&large[0], large.size(), format, args);
	va_end(args);
	return std::string(&large[0], (size_t)length);
}

//---------------------------------------------------------------------------
// Image helpers
//---------------------------------------------------------------------------

static const char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const u8 *data, size_t length)
{
	std::string out;
	out.reserve(((length + 2) / 3) * 4);
	for (size_t i = 0; i < length; i += 3)
	{
		unsigned int n = (unsigned int)data[i] << 16;
		if (i + 1 < length) n |= (unsigned int)data[i + 1] << 8;
		if (i + 2 < length) n |= (unsigned int)data[i + 2];
		out.push_back(BASE64_TABLE[(n >> 18) & 63]);
		out.push_back(BASE64_TABLE[(n >> 12) & 63]);
		out.push_back((i + 1 < length) ? BASE64_TABLE[(n >> 6) & 63] : '=');
		out.push_back((i + 2 < length) ? BASE64_TABLE[n & 63] : '=');
	}
	return out;
}

static void ConvertNative15ToRGB24(const u16 *src, u8 *dst, size_t pixelCount)
{
	for (size_t i = 0; i < pixelCount; i++)
	{
		const u16 pixel = src[i];
		dst[i * 3 + 0] = (u8)(((pixel >> 0) & 0x1F) << 3);
		dst[i * 3 + 1] = (u8)(((pixel >> 5) & 0x1F) << 3);
		dst[i * 3 + 2] = (u8)(((pixel >> 10) & 0x1F) << 3);
	}
}

static std::vector<u8> BuildBMP24(const u8 *rgb, int width, int height)
{
	const int rowStride = ((width * 3 + 3) / 4) * 4;
	const int dataSize = rowStride * height;
	const int fileSize = 54 + dataSize;
	std::vector<u8> bmp((size_t)fileSize, 0);

	bmp[0] = 'B';
	bmp[1] = 'M';
	*(u32 *)&bmp[2] = (u32)fileSize;
	*(u32 *)&bmp[10] = 54;
	*(u32 *)&bmp[14] = 40;
	*(u32 *)&bmp[18] = (u32)width;
	*(u32 *)&bmp[22] = (u32)height;
	*(u16 *)&bmp[26] = 1;
	*(u16 *)&bmp[28] = 24;
	*(u32 *)&bmp[34] = (u32)dataSize;

	u8 *dst = &bmp[54];
	for (int y = height - 1; y >= 0; y--)
	{
		const u8 *srcRow = rgb + (size_t)y * (size_t)width * 3;
		//BMP stores BGR
		for (int x = 0; x < width; x++)
		{
			dst[x * 3 + 0] = srcRow[x * 3 + 2];
			dst[x * 3 + 1] = srcRow[x * 3 + 1];
			dst[x * 3 + 2] = srcRow[x * 3 + 0];
		}
		dst += width * 3;
		for (int pad = rowStride - width * 3; pad > 0; pad--)
			*dst++ = 0;
	}

	return bmp;
}

static void PNGAppendU32(std::vector<u8> &out, u32 value)
{
	out.push_back((u8)((value >> 24) & 0xFF));
	out.push_back((u8)((value >> 16) & 0xFF));
	out.push_back((u8)((value >> 8) & 0xFF));
	out.push_back((u8)(value & 0xFF));
}

static void PNGAppendChunk(std::vector<u8> &out, const char *type, const u8 *data, size_t length)
{
	PNGAppendU32(out, (u32)length);

	const size_t crcStart = out.size();
	out.insert(out.end(), type, type + 4);
	if (length > 0)
		out.insert(out.end(), data, data + length);

	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, &out[crcStart], (uInt)(4 + length));
	PNGAppendU32(out, (u32)crc);
}

/* Returns an empty vector when compression fails. */
static std::vector<u8> BuildPNG24(const u8 *rgb, int width, int height)
{
	std::vector<u8> png;

	//raw scanlines, each prefixed with filter type 0
	std::vector<u8> raw((size_t)height * ((size_t)width * 3 + 1));
	for (int y = 0; y < height; y++)
	{
		u8 *row = &raw[(size_t)y * ((size_t)width * 3 + 1)];
		row[0] = 0;
		memcpy(row + 1, rgb + (size_t)y * (size_t)width * 3, (size_t)width * 3);
	}

	uLongf compressedSize = compressBound((uLong)raw.size());
	std::vector<u8> compressed((size_t)compressedSize);
	if (compress2(&compressed[0], &compressedSize, &raw[0], (uLong)raw.size(), Z_DEFAULT_COMPRESSION) != Z_OK)
		return png;
	compressed.resize((size_t)compressedSize);

	static const u8 signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	png.insert(png.end(), signature, signature + 8);

	u8 ihdr[13];
	ihdr[0] = (u8)((width >> 24) & 0xFF);
	ihdr[1] = (u8)((width >> 16) & 0xFF);
	ihdr[2] = (u8)((width >> 8) & 0xFF);
	ihdr[3] = (u8)(width & 0xFF);
	ihdr[4] = (u8)((height >> 24) & 0xFF);
	ihdr[5] = (u8)((height >> 16) & 0xFF);
	ihdr[6] = (u8)((height >> 8) & 0xFF);
	ihdr[7] = (u8)(height & 0xFF);
	ihdr[8] = 8;  //bit depth
	ihdr[9] = 2;  //color type: truecolor
	ihdr[10] = 0; //compression
	ihdr[11] = 0; //filter
	ihdr[12] = 0; //interlace
	PNGAppendChunk(png, "IHDR", ihdr, sizeof(ihdr));
	PNGAppendChunk(png, "IDAT", &compressed[0], compressed.size());
	PNGAppendChunk(png, "IEND", NULL, 0);

	return png;
}

static bool WriteFileBytes(const char *path, const u8 *data, size_t length)
{
	FILE *fp = fopen(path, "wb");
	if (fp == NULL)
		return false;
	const size_t written = fwrite(data, 1, length, fp);
	fclose(fp);
	return (written == length);
}

static bool HasExtension(const std::string &path, const char *extension)
{
	const size_t extensionLength = strlen(extension);
	if (path.size() < extensionLength)
		return false;
	for (size_t i = 0; i < extensionLength; i++)
	{
		const char a = (char)tolower((unsigned char)path[path.size() - extensionLength + i]);
		if (a != extension[i])
			return false;
	}
	return true;
}

//---------------------------------------------------------------------------
// Tools
//---------------------------------------------------------------------------

static int ProcessorFromArgs(const mcpjson::Value &args)
{
	const mcpjson::Value *proc = args.Find("proc");
	if (proc == NULL || proc->IsNull())
		return 0;
	if (proc->IsString())
	{
		const std::string name = proc->str;
		if (name == "7" || name == "arm7" || name == "ARM7")
			return 1;
		return 0;
	}
	return (proc->AsInt() != 0) ? 1 : 0;
}

static armcpu_t& CPUFromArgs(const mcpjson::Value &args)
{
	return (ProcessorFromArgs(args) != 0) ? NDS_ARM7 : NDS_ARM9;
}

static bool IsROMLoaded()
{
	return (gameInfo.reader != NULL);
}

/*
	Runs frames on the calling (emulation) thread and returns how many actually ran.

	A paused emulator has its CPUs stalled, so running frames has to lift that first,
	otherwise the frame counter would advance without a single instruction executing.
	A memory breakpoint clears the global execute flag, which ends the run early.
*/
/*
	A breakpoint on the address a CPU is sitting at must not fire before that
	instruction has had a chance to run, otherwise resuming from a breakpoint (or
	setting one on the current PC) would stop again without making progress.
*/
static void ArmBreakpointSkip()
{
	NDS_ARM9.breakpointSkipAddress = NDS_ARM9.instruct_adr;
	NDS_ARM7.breakpointSkipAddress = NDS_ARM7.instruct_adr;
}

static void SetExecute(bool run)
{
	if (g_setExecute != NULL)
		g_setExecute(run ? 1 : 0);
	else
		execute = run;
}

static int RunFrames(int frames)
{
	//a pause can be either of these, and both have to be lifted for a frame to run
	const bool wasStalled = (NDS_ARM9.stalled != 0 || NDS_ARM7.stalled != 0);
	const bool wasRunning = (g_getExecute != NULL) ? (g_getExecute() != 0) : execute;

	if (wasStalled)
		NDS_debug_continue();
	ArmBreakpointSkip();
	SetExecute(true);

	int ran = 0;
	for (int i = 0; i < frames; i++)
	{
		NDS_exec<false>();
		SPU_Emulate_user();
		ran++;

		if (!execute)
			break;  //a breakpoint stopped us
	}

	const bool stoppedByBreakpoint = !execute;

	if (wasStalled && !stoppedByBreakpoint)
		NDS_debug_break();
	if (!wasRunning || stoppedByBreakpoint)
		SetExecute(false);

	return ran;
}

/* "execute breakpoint at 0x02000008 on ARM9", or empty when nothing is pending. */
static std::string BreakpointHitDescription()
{
	const char *kind = NULL;
	switch (nds_breakpointHit.type)
	{
		case NDS_BREAKPOINT_EXECUTE: kind = "execute"; break;
		case NDS_BREAKPOINT_READ:    kind = "read";    break;
		case NDS_BREAKPOINT_WRITE:   kind = "write";   break;
		default:                     return std::string();
	}

	return Format("%s breakpoint at 0x%08X on ARM%d", kind,
		(unsigned)nds_breakpointHit.address,
		(nds_breakpointHit.procnum != 0) ? 7 : 9);
}

static std::string ToolGetState()
{
	const bool running = (g_getExecute != NULL) ? (g_getExecute() != 0) : false;

	std::string text = Format(
		"running=%s rom_loaded=%s frame=%d\n"
		"ARM9: PC=0x%08X CPSR=0x%08X %s\n"
		"ARM7: PC=0x%08X CPSR=0x%08X %s",
		running ? "true" : "false",
		IsROMLoaded() ? "true" : "false",
		currFrameCounter,
		(unsigned)NDS_ARM9.instruct_adr, (unsigned)NDS_ARM9.CPSR.val, NDS_ARM9.CPSR.bits.T ? "thumb" : "arm",
		(unsigned)NDS_ARM7.instruct_adr, (unsigned)NDS_ARM7.CPSR.val, NDS_ARM7.CPSR.bits.T ? "thumb" : "arm");

	if (IsROMLoaded())
		text += Format("\nROM: title=%s serial=%s size=%u", gameInfo.ROMname, gameInfo.ROMserial, (unsigned)gameInfo.romsize);

	const std::string breakpoint = BreakpointHitDescription();
	if (!breakpoint.empty())
		text += "\nstopped on " + breakpoint;

	return TextResult(text);
}

static std::string ToolPause()
{
	if (g_setExecute != NULL)
		g_setExecute(0);
	NDS_debug_break();
	return TextResult("paused");
}

static std::string ToolResume()
{
	NDS_ClearBreakpointHit();
	ArmBreakpointSkip();
	if (g_setExecute != NULL)
		g_setExecute(1);
	NDS_debug_continue();
	return TextResult("running");
}

/* Shared by nds_step and by nds_step_over when it has nothing to step over. */
static long StepInstructions(armcpu_t &cpu, long count)
{
	//a paused emulator has its CPUs stalled, which would keep them from executing
	const bool wasStalled = (NDS_ARM9.stalled != 0 || NDS_ARM7.stalled != 0);
	if (wasStalled)
		NDS_debug_continue();

	/*
		debugStep makes the CPU loop run a single instruction on that core and then
		drop the global execute flag, which ends the frame early. That is the same
		mechanism the Windows debugger steps with.
	*/
	long stepped = 0;
	for (long i = 0; i < count; i++)
	{
		cpu.debugStep = true;
		SetExecute(true);

		NDS_exec<false>();
		SPU_Emulate_user();

		if (cpu.debugStep)
		{
			//the core never got to run: it is halted or waiting for an interrupt
			cpu.debugStep = false;
			break;
		}

		stepped++;
	}

	//leave nothing armed behind, or the next run would be cut short
	NDS_ARM9.debugStep = false;
	NDS_ARM7.debugStep = false;

	if (wasStalled)
		NDS_debug_break();
	SetExecute(false);

	return stepped;
}

/*
	Runs until an armed step over or step out fires, a breakpoint stops us, or the
	frame budget runs out. Returns the number of frames that ran.
*/
static int RunUntilStepCompletes(armcpu_t &cpu, int maxFrames)
{
	const bool wasStalled = (NDS_ARM9.stalled != 0 || NDS_ARM7.stalled != 0);
	if (wasStalled)
		NDS_debug_continue();
	ArmBreakpointSkip();
	SetExecute(true);

	int ran = 0;
	for (int i = 0; i < maxFrames; i++)
	{
		NDS_exec<false>();
		SPU_Emulate_user();
		ran++;

		if (!execute)
			break;  //the step finished, or a breakpoint stopped us
	}

	NDS_debug_cancelStepping(cpu);
	if (wasStalled)
		NDS_debug_break();
	SetExecute(false);

	return ran;
}

static int StepFrameBudget(const mcpjson::Value &args)
{
	long frames = args.GetInt("max_frames", 120);
	if (frames < 1) frames = 1;
	if (frames > MAX_RUN_FRAMES) frames = MAX_RUN_FRAMES;
	return (int)frames;
}

static std::string ToolStep(const mcpjson::Value &args)
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	long count = args.GetInt("count", 1);
	if (count < 1) count = 1;
	if (count > 1000) count = 1000;

	armcpu_t &cpu = CPUFromArgs(args);
	const int proc = ProcessorFromArgs(args);

	NDS_ClearBreakpointHit();

	const long stepped = StepInstructions(cpu, count);

	std::string text = Format("stepped %ld instruction(s) on ARM%d; ARM9 PC=0x%08X ARM7 PC=0x%08X",
		stepped, (proc != 0) ? 7 : 9, (unsigned)NDS_ARM9.instruct_adr, (unsigned)NDS_ARM7.instruct_adr);

	const std::string breakpoint = BreakpointHitDescription();
	if (!breakpoint.empty())
		text += Format(" (stopped on %s)", breakpoint.c_str());
	else if (stepped < count)
		text += " (stopped early: the CPU is halted)";

	return TextResult(text);
}

static std::string ToolStepOver(const mcpjson::Value &args)
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	armcpu_t &cpu = CPUFromArgs(args);
	const int proc = ProcessorFromArgs(args);
	const u32 from = cpu.instruct_adr;

	NDS_ClearBreakpointHit();

	u32 returnAddress = 0;
	if (!NDS_debug_getStepOverTarget(cpu, returnAddress))
	{
		//nothing to step over, so this is an ordinary single step
		const long stepped = StepInstructions(cpu, 1);

		std::string text = Format("no call at 0x%08X, stepped %ld instruction(s) on ARM%d; PC=0x%08X",
			(unsigned)from, stepped, (proc != 0) ? 7 : 9, (unsigned)cpu.instruct_adr);

		const std::string breakpoint = BreakpointHitDescription();
		if (!breakpoint.empty())
			text += Format(" (stopped on %s)", breakpoint.c_str());

		return TextResult(text);
	}

	NDS_debug_armStepOver(cpu, returnAddress);
	const int ran = RunUntilStepCompletes(cpu, StepFrameBudget(args));

	std::string text;
	if (cpu.instruct_adr == returnAddress)
		text = Format("stepped over the call at 0x%08X on ARM%d; PC=0x%08X",
			(unsigned)from, (proc != 0) ? 7 : 9, (unsigned)cpu.instruct_adr);
	else
		text = Format("stepping over the call at 0x%08X on ARM%d did not come back to 0x%08X; PC=0x%08X",
			(unsigned)from, (proc != 0) ? 7 : 9, (unsigned)returnAddress, (unsigned)cpu.instruct_adr);

	const std::string breakpoint = BreakpointHitDescription();
	if (!breakpoint.empty())
		text += Format(" (stopped on %s)", breakpoint.c_str());
	else if (cpu.instruct_adr != returnAddress)
		text += Format(" (gave up after %d frame(s))", ran);

	return TextResult(text, cpu.instruct_adr != returnAddress && breakpoint.empty());
}

static std::string ToolStepOut(const mcpjson::Value &args)
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	armcpu_t &cpu = CPUFromArgs(args);
	const int proc = ProcessorFromArgs(args);
	const u32 from = cpu.instruct_adr;
	const u32 startSP = cpu.R[13];

	NDS_ClearBreakpointHit();

	NDS_debug_armStepOut(cpu);
	const int ran = RunUntilStepCompletes(cpu, StepFrameBudget(args));

	const bool returned = (cpu.R[13] > startSP);

	std::string text;
	if (returned)
		text = Format("returned from 0x%08X to 0x%08X on ARM%d; SP 0x%08X -> 0x%08X",
			(unsigned)from, (unsigned)cpu.instruct_adr, (proc != 0) ? 7 : 9,
			(unsigned)startSP, (unsigned)cpu.R[13]);
	else
		text = Format("did not return from 0x%08X on ARM%d; PC=0x%08X SP=0x%08X",
			(unsigned)from, (proc != 0) ? 7 : 9, (unsigned)cpu.instruct_adr, (unsigned)cpu.R[13]);

	const std::string breakpoint = BreakpointHitDescription();
	if (!breakpoint.empty())
		text += Format(" (stopped on %s)", breakpoint.c_str());
	else if (!returned)
		text += Format(" (gave up after %d frame(s))", ran);

	return TextResult(text, !returned && breakpoint.empty());
}

static std::string ToolRunFrames(const mcpjson::Value &args)
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	long frames = args.GetInt("frames", 1);
	if (frames < 1) frames = 1;
	if (frames > MAX_RUN_FRAMES) frames = MAX_RUN_FRAMES;

	NDS_ClearBreakpointHit();

	const int ran = RunFrames((int)frames);

	std::string text = Format("ran %d frame(s); frame=%d ARM9 PC=0x%08X", ran, currFrameCounter, (unsigned)NDS_ARM9.instruct_adr);

	const std::string breakpoint = BreakpointHitDescription();
	if (!breakpoint.empty())
		text += Format(" (stopped early on %s)", breakpoint.c_str());
	else if (ran < frames)
		text += " (stopped early)";

	return TextResult(text);
}

static std::string ToolReset()
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");
	NDS_Reset();
	return TextResult("reset");
}

static std::string ToolQuit()
{
	g_quitRequested = true;
	if (g_setExecute != NULL)
		g_setExecute(0);
	return TextResult("shutting down");
}

static std::string ToolLoadROM(const mcpjson::Value &args)
{
	const std::string path = args.GetString("path");
	if (path.empty())
		return ErrorResult("missing path");

	const int result = NDS_LoadROM(path.c_str(), NULL, NULL);
	if (result < 0)
		return ErrorResult(Format("failed to load %s (error %d)", path.c_str(), result));

	return TextResult(Format("loaded %s (title=%s serial=%s)", path.c_str(), gameInfo.ROMname, gameInfo.ROMserial));
}

static std::string ToolReloadROM()
{
	const char *path = NDS_GetLastRomPath();
	if (path == NULL || path[0] == '\0')
		return ErrorResult("no ROM loaded to reload");

	const int result = NDS_LoadROM(path, NULL, NULL);
	if (result < 0)
		return ErrorResult(Format("failed to reload %s (error %d)", path, result));

	return TextResult(Format("reloaded %s", path));
}

static std::string ToolGetROMInfo()
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	const NDS_header *header = NDS_getROMHeader();
	std::string text = Format("title=%s serial=%s size=%u", gameInfo.ROMname, gameInfo.ROMserial, (unsigned)gameInfo.romsize);
	if (header != NULL)
	{
		char gameCode[5];
		memcpy(gameCode, header->gameCode, 4);
		gameCode[4] = '\0';
		text += Format(" code=%s maker=0x%04X version=%u arm9_entry=0x%08X arm7_entry=0x%08X",
			gameCode,
			(unsigned)header->makerCode, (unsigned)header->romversion,
			(unsigned)header->ARM9exe, (unsigned)header->ARM7exe);
	}
	const char *path = NDS_GetLastRomPath();
	if (path != NULL && path[0] != '\0')
		text += Format(" path=%s", path);

	return TextResult(text);
}

static std::string ToolReadMemory(const mcpjson::Value &args)
{
	u32 address = 0;
	if (!args.GetAddress("address", address))
		return ErrorResult("missing or malformed address");

	const int proc = ProcessorFromArgs(args);
	long size = args.GetInt("size", 16);
	if (size < 1) size = 1;
	if ((size_t)size > MAX_MEMORY_READ)
		size = (long)MAX_MEMORY_READ;

	std::vector<u8> bytes((size_t)size);
	for (long i = 0; i < size; i++)
		bytes[(size_t)i] = _MMU_read08(proc, MMU_AT_DEBUG, address + (u32)i);

	//classic hex dump: 16 bytes per line with an ASCII column
	std::string text;
	for (long offset = 0; offset < size; offset += 16)
	{
		text += Format("%08X:", (unsigned)(address + (u32)offset));

		const long lineLength = ((size - offset) < 16) ? (size - offset) : 16;
		for (long i = 0; i < 16; i++)
		{
			if (i < lineLength)
				text += Format(" %02X", (unsigned)bytes[(size_t)(offset + i)]);
			else
				text += "   ";
		}

		text += "  ";
		for (long i = 0; i < lineLength; i++)
		{
			const u8 value = bytes[(size_t)(offset + i)];
			text.push_back((value >= 0x20 && value < 0x7F) ? (char)value : '.');
		}

		if (offset + 16 < size)
			text.push_back('\n');
	}

	return TextResult(text);
}

static bool ParseHexBytes(const std::string &text, std::vector<u8> &out)
{
	std::string compact;
	for (size_t i = 0; i < text.size(); i++)
	{
		const char c = text[i];
		if (c == ' ' || c == '\t' || c == ',' || c == '\n' || c == '\r')
			continue;
		compact.push_back(c);
	}

	if (compact.size() >= 2 && compact[0] == '0' && (compact[1] == 'x' || compact[1] == 'X'))
		compact.erase(0, 2);

	if (compact.empty() || (compact.size() % 2) != 0)
		return false;

	out.clear();
	out.reserve(compact.size() / 2);
	for (size_t i = 0; i < compact.size(); i += 2)
	{
		char byteText[3] = { compact[i], compact[i + 1], '\0' };
		char *endPtr = NULL;
		const unsigned long value = strtoul(byteText, &endPtr, 16);
		if (endPtr != byteText + 2)
			return false;
		out.push_back((u8)value);
	}

	return true;
}

static std::string ToolWriteMemory(const mcpjson::Value &args)
{
	u32 address = 0;
	if (!args.GetAddress("address", address))
		return ErrorResult("missing or malformed address");

	const int proc = ProcessorFromArgs(args);

	std::vector<u8> bytes;
	const mcpjson::Value *value = args.Find("value");
	if (value == NULL || value->IsNull())
		return ErrorResult("missing value");

	if (value->IsArray())
	{
		for (size_t i = 0; i < value->items.size(); i++)
			bytes.push_back((u8)value->items[i].AsInt());
	}
	else if (value->IsNumber())
	{
		//a bare number is written using the requested width, little endian
		long width = args.GetInt("size", 4);
		if (width != 1 && width != 2 && width != 4)
			width = 4;
		const u32 raw = (u32)value->AsInt();
		for (long i = 0; i < width; i++)
			bytes.push_back((u8)((raw >> (8 * i)) & 0xFF));
	}
	else if (!ParseHexBytes(value->AsString(), bytes))
	{
		return ErrorResult("value must be an even length hex string, a number, or an array of bytes");
	}

	if (bytes.empty())
		return ErrorResult("nothing to write");
	if (bytes.size() > MAX_MEMORY_READ)
		return ErrorResult(Format("refusing to write more than %u bytes at once", (unsigned)MAX_MEMORY_READ));

	for (size_t i = 0; i < bytes.size(); i++)
		_MMU_write08(proc, MMU_AT_DEBUG, address + (u32)i, bytes[i]);

	return TextResult(Format("wrote %u byte(s) to 0x%08X on ARM%d", (unsigned)bytes.size(), (unsigned)address, (proc != 0) ? 7 : 9));
}

static std::string ToolSearchMemory(const mcpjson::Value &args)
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	const int proc = ProcessorFromArgs(args);

	std::vector<u8> pattern;
	const mcpjson::Value *value = args.Find("value");
	if (value == NULL || value->IsNull())
		return ErrorResult("missing value");

	if (value->IsNumber())
	{
		long width = args.GetInt("size", 4);
		if (width != 1 && width != 2 && width != 4)
			width = 4;
		const u32 raw = (u32)value->AsInt();
		for (long i = 0; i < width; i++)
			pattern.push_back((u8)((raw >> (8 * i)) & 0xFF));
	}
	else if (value->IsString() && !ParseHexBytes(value->str, pattern))
	{
		return ErrorResult("value must be a hex byte string or a number");
	}
	else if (value->IsArray())
	{
		for (size_t i = 0; i < value->items.size(); i++)
			pattern.push_back((u8)value->items[i].AsInt());
	}

	if (pattern.empty())
		return ErrorResult("empty search pattern");

	u32 start = 0x02000000;
	u32 end = 0x02400000;
	args.GetAddress("start", start);
	args.GetAddress("end", end);
	if (end <= start)
		return ErrorResult("end must be greater than start");
	if ((end - start) > 0x01000000)
		return ErrorResult("search range is limited to 16 MiB");

	long maxResults = args.GetInt("max_results", 32);
	if (maxResults < 1) maxResults = 1;
	if (maxResults > 256) maxResults = 256;

	std::vector<u32> hits;
	const u32 last = end - (u32)pattern.size();
	for (u32 address = start; address <= last; address++)
	{
		size_t i = 0;
		while (i < pattern.size() && _MMU_read08(proc, MMU_AT_DEBUG, address + (u32)i) == pattern[i])
			i++;
		if (i == pattern.size())
		{
			hits.push_back(address);
			if ((long)hits.size() >= maxResults)
				break;
		}
	}

	if (hits.empty())
		return TextResult("no matches");

	std::string text = Format("%u match(es):", (unsigned)hits.size());
	for (size_t i = 0; i < hits.size(); i++)
		text += Format(" 0x%08X", (unsigned)hits[i]);
	if ((long)hits.size() >= maxResults)
		text += " (truncated)";

	return TextResult(text);
}

static std::string ToolGetRegisters(const mcpjson::Value &args)
{
	const armcpu_t &cpu = CPUFromArgs(args);
	const int proc = ProcessorFromArgs(args);

	std::string text = Format("ARM%d PC=0x%08X CPSR=0x%08X (%s, mode=0x%02X) next=0x%08X\n",
		(proc != 0) ? 7 : 9,
		(unsigned)cpu.instruct_adr, (unsigned)cpu.CPSR.val,
		cpu.CPSR.bits.T ? "thumb" : "arm", (unsigned)cpu.CPSR.bits.mode,
		(unsigned)cpu.next_instruction);

	for (int i = 0; i < 16; i++)
	{
		text += Format("R%-2d=0x%08X", i, (unsigned)cpu.R[i]);
		text += ((i % 4) == 3) ? "\n" : "  ";
	}
	text += Format("SPSR=0x%08X", (unsigned)cpu.SPSR.val);

	return TextResult(text);
}

static std::string ToolSetRegister(const mcpjson::Value &args)
{
	armcpu_t &cpu = CPUFromArgs(args);
	const int proc = ProcessorFromArgs(args);

	std::string name = args.GetString("register");
	if (name.empty())
		return ErrorResult("missing register (R0-R15, PC, SP, LR, CPSR)");
	for (size_t i = 0; i < name.size(); i++)
		name[i] = (char)toupper((unsigned char)name[i]);

	u32 value = 0;
	if (!args.GetAddress("value", value))
		return ErrorResult("missing or malformed value");

	int index = -1;
	if (name == "PC") index = 15;
	else if (name == "SP") index = 13;
	else if (name == "LR") index = 14;
	else if (name == "CPSR")
	{
		cpu.CPSR.val = value;
		return TextResult(Format("ARM%d CPSR=0x%08X", (proc != 0) ? 7 : 9, (unsigned)value));
	}
	else if (name.size() >= 2 && name[0] == 'R')
	{
		index = (int)strtol(name.c_str() + 1, NULL, 10);
	}

	if (index < 0 || index > 15)
		return ErrorResult("register must be one of R0-R15, PC, SP, LR, CPSR");

	cpu.R[index] = value;
	if (index == 15)
	{
		//keep the fetch state in sync with the new PC
		const u32 instructionSize = cpu.CPSR.bits.T ? 2 : 4;
		cpu.instruct_adr = value;
		cpu.next_instruction = value;
		cpu.R[15] = value + instructionSize;
	}

	return TextResult(Format("ARM%d %s=0x%08X", (proc != 0) ? 7 : 9, name.c_str(), (unsigned)value));
}

static std::string ToolDisassemble(const mcpjson::Value &args)
{
	const armcpu_t &cpu = CPUFromArgs(args);
	const int proc = ProcessorFromArgs(args);

	u32 address = cpu.instruct_adr;
	args.GetAddress("address", address);

	long count = args.GetInt("count", 8);
	if (count < 1) count = 1;
	if (count > 128) count = 128;

	bool thumb = (cpu.CPSR.bits.T != 0);
	const mcpjson::Value *thumbArg = args.Find("thumb");
	if (thumbArg != NULL && !thumbArg->IsNull())
		thumb = args.GetBool("thumb", thumb);

	std::string text;
	for (long i = 0; i < count; i++)
	{
		char disassembly[128] = { 0 };

		if (thumb)
		{
			const u16 opcode = (u16)(_MMU_read08(proc, MMU_AT_DEBUG, address) |
				((u16)_MMU_read08(proc, MMU_AT_DEBUG, address + 1) << 8));
			des_thumb_instructions_set[(opcode >> 6) & 1023](address, opcode, disassembly);
			text += Format("%08X  %04X      %s", (unsigned)address, (unsigned)opcode, disassembly);
			address += 2;
		}
		else
		{
			u32 opcode = 0;
			for (int b = 0; b < 4; b++)
				opcode |= ((u32)_MMU_read08(proc, MMU_AT_DEBUG, address + (u32)b)) << (8 * b);
			des_arm_instructions_set[INSTRUCTION_INDEX(opcode)](address, opcode, disassembly);
			text += Format("%08X  %08X  %s", (unsigned)address, (unsigned)opcode, disassembly);
			address += 4;
		}

		if (i + 1 < count)
			text.push_back('\n');
	}

	return TextResult(text);
}

static std::vector<u32>* ExecuteBreakpointList(int proc)
{
	return (proc != 0) ? NDS_ARM7.breakPoints : NDS_ARM9.breakPoints;
}

static std::string ToolSetBreakpoint(const mcpjson::Value &args)
{
	const std::string type = args.GetString("type", "execute");
	u32 address = 0;
	if (!args.GetAddress("address", address))
		return ErrorResult("missing or malformed address");

	const int proc = ProcessorFromArgs(args);
	std::vector<u32> *list = NULL;

	if (type == "execute")
	{
		list = ExecuteBreakpointList(proc);
		if (list == NULL)
			return ErrorResult("CPU breakpoint list is not available yet");
	}
	else if (type == "read")
	{
		list = &memReadBreakPoints;
	}
	else if (type == "write")
	{
		list = &memWriteBreakPoints;
	}
	else
	{
		return ErrorResult("type must be execute, read, or write");
	}

	if (std::find(list->begin(), list->end(), address) != list->end())
		return TextResult(Format("%s breakpoint at 0x%08X was already set", type.c_str(), (unsigned)address));

	list->push_back(address);

	if (type == "execute")
		return TextResult(Format("execute breakpoint at 0x%08X on ARM%d", (unsigned)address, (proc != 0) ? 7 : 9));
	return TextResult(Format("%s breakpoint at 0x%08X", type.c_str(), (unsigned)address));
}

static std::string ToolClearBreakpoint(const mcpjson::Value &args)
{
	const std::string type = args.GetString("type", "execute");
	u32 address = 0;
	if (!args.GetAddress("address", address))
		return ErrorResult("missing or malformed address");

	const int proc = ProcessorFromArgs(args);
	std::vector<u32> *list = NULL;

	if (type == "execute")
	{
		list = ExecuteBreakpointList(proc);
		if (list == NULL)
			return ErrorResult("CPU breakpoint list is not available yet");
	}
	else if (type == "read")
		list = &memReadBreakPoints;
	else if (type == "write")
		list = &memWriteBreakPoints;
	else
		return ErrorResult("type must be execute, read, or write");

	const std::vector<u32>::iterator it = std::find(list->begin(), list->end(), address);
	if (it == list->end())
		return TextResult(Format("no %s breakpoint at 0x%08X", type.c_str(), (unsigned)address));

	list->erase(it);
	return TextResult(Format("cleared %s breakpoint at 0x%08X", type.c_str(), (unsigned)address));
}

static std::string ToolClearAllBreakpoints()
{
	if (NDS_ARM9.breakPoints != NULL) NDS_ARM9.breakPoints->clear();
	if (NDS_ARM7.breakPoints != NULL) NDS_ARM7.breakPoints->clear();
	memReadBreakPoints.clear();
	memWriteBreakPoints.clear();
	return TextResult("all breakpoints cleared");
}

static void AppendBreakpointList(std::string &text, const char *label, const std::vector<u32> *list)
{
	text += label;
	if (list == NULL || list->empty())
	{
		text += " (none)";
		return;
	}
	for (size_t i = 0; i < list->size(); i++)
		text += Format(" 0x%08X", (unsigned)(*list)[i]);
}

static std::string ToolListBreakpoints()
{
	std::string text;
	AppendBreakpointList(text, "execute ARM9:", NDS_ARM9.breakPoints);
	text.push_back('\n');
	AppendBreakpointList(text, "execute ARM7:", NDS_ARM7.breakPoints);
	text.push_back('\n');
	AppendBreakpointList(text, "read:", &memReadBreakPoints);
	text.push_back('\n');
	AppendBreakpointList(text, "write:", &memWriteBreakPoints);
	return TextResult(text);
}

static std::string ToolSaveState(const mcpjson::Value &args)
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	const std::string path = args.GetString("path");
	if (!path.empty())
	{
		if (!savestate_save(path.c_str()))
			return ErrorResult(Format("failed to write savestate to %s", path.c_str()));
		return TextResult(Format("saved state to %s", path.c_str()));
	}

	const mcpjson::Value *slot = args.Find("slot");
	if (slot == NULL || slot->IsNull())
		return ErrorResult("provide either path or slot");

	const long slotNumber = slot->AsInt();
	if (slotNumber < 0 || slotNumber >= NB_STATES)
		return ErrorResult(Format("slot must be between 0 and %d", NB_STATES - 1));

	savestate_slot((int)slotNumber);
	return TextResult(Format("saved state to slot %ld", slotNumber));
}

static std::string ToolLoadState(const mcpjson::Value &args)
{
	if (!IsROMLoaded())
		return ErrorResult("no ROM loaded");

	const std::string path = args.GetString("path");
	if (!path.empty())
	{
		if (!savestate_load(path.c_str()))
			return ErrorResult(Format("failed to load savestate from %s", path.c_str()));
		return TextResult(Format("loaded state from %s", path.c_str()));
	}

	const mcpjson::Value *slot = args.Find("slot");
	if (slot == NULL || slot->IsNull())
		return ErrorResult("provide either path or slot");

	const long slotNumber = slot->AsInt();
	if (slotNumber < 0 || slotNumber >= NB_STATES)
		return ErrorResult(Format("slot must be between 0 and %d", NB_STATES - 1));

	loadstate_slot((int)slotNumber);
	return TextResult(Format("loaded state from slot %ld", slotNumber));
}

static std::string ToolScreenshot(const mcpjson::Value &args)
{
	if (GPU == NULL)
		return ErrorResult("GPU is not initialized");

	const NDSDisplayInfo &displayInfo = GPU->GetDisplayInfo();
	const u16 *frameBuffer = displayInfo.masterNativeBuffer16;
	if (frameBuffer == NULL)
		return ErrorResult("no framebuffer available");

	const std::string screen = args.GetString("screen", "both");
	const std::string path = args.GetString("path");
	std::string format = args.GetString("format", "png");
	for (size_t i = 0; i < format.size(); i++)
		format[i] = (char)tolower((unsigned char)format[i]);

	const int screenWidth = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const int screenHeight = GPU_FRAMEBUFFER_NATIVE_HEIGHT;
	const size_t pixelsPerScreen = (size_t)screenWidth * (size_t)screenHeight;

	const u16 *source = frameBuffer;
	int width = screenWidth;
	int height = screenHeight * 2;

	if (screen == "main" || screen == "top")
	{
		height = screenHeight;
	}
	else if (screen == "touch" || screen == "bottom")
	{
		source = frameBuffer + pixelsPerScreen;
		height = screenHeight;
	}
	else if (screen != "both")
	{
		return ErrorResult("screen must be both, main, or touch");
	}

	std::vector<u8> rgb((size_t)width * (size_t)height * 3);
	ConvertNative15ToRGB24(source, &rgb[0], (size_t)width * (size_t)height);

	//an explicit extension in the output path wins over the format argument
	bool wantBMP = (format == "bmp");
	if (!path.empty())
	{
		if (HasExtension(path, ".bmp"))
			wantBMP = true;
		else if (HasExtension(path, ".png"))
			wantBMP = false;
	}

	std::vector<u8> encoded = wantBMP ? BuildBMP24(&rgb[0], width, height) : BuildPNG24(&rgb[0], width, height);
	if (encoded.empty())
		return ErrorResult("failed to encode the screenshot");

	if (!path.empty())
	{
		if (!WriteFileBytes(path.c_str(), &encoded[0], encoded.size()))
			return ErrorResult(Format("failed to write %s", path.c_str()));
		return TextResult(Format("saved %dx%d %s to %s", width, height, wantBMP ? "BMP" : "PNG", path.c_str()));
	}

	std::string out("{\"content\":[{\"type\":\"image\",\"data\":\"");
	out += Base64Encode(&encoded[0], encoded.size());
	out += "\",\"mimeType\":\"";
	out += wantBMP ? "image/bmp" : "image/png";
	out += "\"},{\"type\":\"text\",\"text\":";
	out += mcpjson::Quote(Format("%dx%d %s (%s screen)", width, height, wantBMP ? "BMP" : "PNG", screen.c_str()));
	out += "}],\"isError\":false}";
	return out;
}

//---------------------------------------------------------------------------
// Input
//---------------------------------------------------------------------------

static bool SetButtonByName(UserButtons &buttons, const std::string &name, bool pressed)
{
	std::string key;
	for (size_t i = 0; i < name.size(); i++)
		key.push_back((char)tolower((unsigned char)name[i]));

	if (key == "a") buttons.A = pressed;
	else if (key == "b") buttons.B = pressed;
	else if (key == "x") buttons.X = pressed;
	else if (key == "y") buttons.Y = pressed;
	else if (key == "start") buttons.S = pressed;
	else if (key == "select") buttons.T = pressed;
	else if (key == "up") buttons.U = pressed;
	else if (key == "down") buttons.D = pressed;
	else if (key == "left") buttons.L = pressed;
	else if (key == "right") buttons.R = pressed;
	else if (key == "l") buttons.W = pressed;
	else if (key == "r") buttons.E = pressed;
	else if (key == "debug") buttons.G = pressed;
	else if (key == "lid") buttons.F = pressed;
	else return false;

	return true;
}

static void ApplyUserButtons(const UserButtons &buttons)
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

static u16 ClampTouchCoordinate(long value, int maximum)
{
	if (value < 0) value = 0;
	if (value >= maximum) value = maximum - 1;
	return (u16)value;
}

static void ApplyUserTouch(u16 x, u16 y, bool touch)
{
	if (touch)
		NDS_setTouchPos(x, y);
	else
		NDS_releaseTouch();

	NDS_beginProcessingInput();
	UserTouch &userTouch = NDS_getProcessingUserInput().touch;
	if (touch)
	{
		userTouch.touchX = (u16)((x << 4) & 0x0FF0);
		userTouch.touchY = (u16)((y << 4) & 0x0FF0);
		userTouch.isTouch = true;
	}
	else
	{
		userTouch.touchX = 0;
		userTouch.touchY = 0;
		userTouch.isTouch = false;
	}
	NDS_endProcessingInput();
}

/* Number of frames an input should be held for, 0 when it should simply latch. */
static int HoldFramesFromArgs(const mcpjson::Value &args)
{
	long frames = args.GetInt("frames", 0);
	if (frames < 0) frames = 0;
	if (frames > MAX_RUN_FRAMES) frames = MAX_RUN_FRAMES;
	return (int)frames;
}

static std::string ToolInputKey(const mcpjson::Value &args)
{
	const std::string button = args.GetString("button");
	if (button.empty())
		return ErrorResult("missing button (A, B, X, Y, start, select, up, down, left, right, L, R, debug, lid)");

	const bool pressed = args.GetBool("pressed", true);
	const int holdFrames = HoldFramesFromArgs(args);

	UserButtons buttons = NDS_getRawUserInput().buttons;
	if (!SetButtonByName(buttons, button, pressed))
		return ErrorResult(Format("unknown button '%s'", button.c_str()));

	ApplyUserButtons(buttons);

	if (holdFrames > 0)
	{
		if (!IsROMLoaded())
			return ErrorResult("no ROM loaded, cannot hold an input for a number of frames");

		RunFrames(holdFrames);

		UserButtons released = NDS_getRawUserInput().buttons;
		SetButtonByName(released, button, !pressed);
		ApplyUserButtons(released);

		return TextResult(Format("%s held for %d frame(s), then released; frame=%d",
			button.c_str(), holdFrames, currFrameCounter));
	}

	return TextResult(Format("%s %s", button.c_str(), pressed ? "pressed" : "released"));
}

static std::string ToolInputTouch(const mcpjson::Value &args)
{
	const mcpjson::Value *xValue = args.Find("x");
	const mcpjson::Value *yValue = args.Find("y");
	const bool touch = args.GetBool("touch", true);
	const int holdFrames = HoldFramesFromArgs(args);

	if (!touch)
	{
		ApplyUserTouch(0, 0, false);
		return TextResult("touch released");
	}

	if (xValue == NULL || yValue == NULL || xValue->IsNull() || yValue->IsNull())
		return ErrorResult("missing x or y");

	const u16 x = ClampTouchCoordinate(xValue->AsInt(), GPU_FRAMEBUFFER_NATIVE_WIDTH);
	const u16 y = ClampTouchCoordinate(yValue->AsInt(), GPU_FRAMEBUFFER_NATIVE_HEIGHT);
	ApplyUserTouch(x, y, true);

	if (holdFrames > 0)
	{
		if (!IsROMLoaded())
			return ErrorResult("no ROM loaded, cannot hold a touch for a number of frames");

		RunFrames(holdFrames);
		ApplyUserTouch(0, 0, false);

		return TextResult(Format("touched (%u,%u) for %d frame(s), then released; frame=%d",
			(unsigned)x, (unsigned)y, holdFrames, currFrameCounter));
	}

	return TextResult(Format("touching (%u,%u)", (unsigned)x, (unsigned)y));
}

static std::string ToolInputReleaseAll()
{
	UserButtons buttons = UserButtons();
	memset(&buttons, 0, sizeof(buttons));
	ApplyUserButtons(buttons);
	ApplyUserTouch(0, 0, false);
	return TextResult("all inputs released");
}

//---------------------------------------------------------------------------
// Tool catalog
//---------------------------------------------------------------------------

static const char *TOOLS_JSON = R"json({"tools":[
{"name":"nds_get_state","description":"Report emulation state: running/paused, frame counter, both CPU program counters and the loaded ROM.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_pause","description":"Pause emulation.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_resume","description":"Resume emulation.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_step","description":"Single step one CPU by a number of instructions.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"count":{"type":"integer","description":"Instructions to step, default 1, max 1000."}}}},
{"name":"nds_step_over","description":"Step one instruction, running a function call or software interrupt to completion instead of stepping into it. Behaves like nds_step when the instruction is not a call.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"max_frames":{"type":"integer","description":"Give up if the call has not returned within this many frames, default 120."}}}},
{"name":"nds_step_out","description":"Run until the function the CPU is in returns, that is until its stack frame is released and execution is back in the caller.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"max_frames":{"type":"integer","description":"Give up if the function has not returned within this many frames, default 120."}}}},
{"name":"nds_run_frames","description":"Run a fixed number of video frames and then return. The emulator is advanced synchronously, which makes scripted play deterministic.","inputSchema":{"type":"object","properties":{"frames":{"type":"integer","description":"Frames to run, default 1, max 3600."}}}},
{"name":"nds_reset","description":"Reset the NDS console.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_quit","description":"Ask the emulator to shut down and exit.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_load_rom","description":"Load an NDS ROM from disk.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Path to the .nds file."}},"required":["path"]}},
{"name":"nds_reload_rom","description":"Reload the ROM that was loaded last.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_get_rom_info","description":"Report the title, serial, size and entry points of the loaded ROM.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_read_memory","description":"Read memory and return it as a hex dump.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"address":{"type":"string","description":"Address, hex by default (e.g. 02000000 or 0x02000000)."},"size":{"type":"integer","description":"Bytes to read, default 16, max 4096."}},"required":["address"]}},
{"name":"nds_write_memory","description":"Write bytes to memory.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"address":{"type":"string","description":"Address, hex by default."},"value":{"description":"Hex byte string (e.g. AABBCCDD), a number, or an array of byte values."},"size":{"type":"integer","description":"Width in bytes when value is a number: 1, 2 or 4 (default 4, little endian)."}},"required":["address","value"]}},
{"name":"nds_search_memory","description":"Search a memory range for a byte pattern.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"value":{"description":"Hex byte string, number, or array of byte values to look for."},"size":{"type":"integer","description":"Width in bytes when value is a number: 1, 2 or 4 (default 4)."},"start":{"type":"string","description":"First address of the range, hex, default 02000000."},"end":{"type":"string","description":"End of the range (exclusive), hex, default 02400000. At most 16 MiB."},"max_results":{"type":"integer","description":"Maximum hits to report, default 32, max 256."}},"required":["value"]}},
{"name":"nds_get_registers","description":"Read the ARM register file of one CPU.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."}}}},
{"name":"nds_set_register","description":"Write one ARM register.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"register":{"type":"string","description":"R0-R15, PC, SP, LR or CPSR."},"value":{"type":"string","description":"New value, hex by default."}},"required":["register","value"]}},
{"name":"nds_disassemble","description":"Disassemble instructions starting at an address.","inputSchema":{"type":"object","properties":{"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7."},"address":{"type":"string","description":"Start address, hex. Defaults to the current PC."},"count":{"type":"integer","description":"Instructions to disassemble, default 8, max 128."},"thumb":{"type":"boolean","description":"Force THUMB decoding. Defaults to the current CPU state."}}}},
{"name":"nds_set_breakpoint","description":"Set an execute, read or write breakpoint. Emulation pauses when one is hit, and nds_get_state reports which one; resuming runs past it.","inputSchema":{"type":"object","properties":{"type":{"type":"string","description":"execute (default), read or write."},"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7, for execute breakpoints."},"address":{"type":"string","description":"Address, hex."}},"required":["address"]}},
{"name":"nds_clear_breakpoint","description":"Clear one breakpoint.","inputSchema":{"type":"object","properties":{"type":{"type":"string","description":"execute (default), read or write."},"proc":{"type":"integer","description":"0 = ARM9 (default), 1 = ARM7, for execute breakpoints."},"address":{"type":"string","description":"Address, hex."}},"required":["address"]}},
{"name":"nds_clear_all_breakpoints","description":"Clear every execute, read and write breakpoint.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_list_breakpoints","description":"List every breakpoint currently set.","inputSchema":{"type":"object","properties":{}}},
{"name":"nds_save_state","description":"Write a savestate to a file or to a numbered slot.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Destination file. Takes precedence over slot."},"slot":{"type":"integer","description":"Savestate slot, 0-9."}}}},
{"name":"nds_load_state","description":"Restore a savestate from a file or from a numbered slot.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Source file. Takes precedence over slot."},"slot":{"type":"integer","description":"Savestate slot, 0-9."}}}},
{"name":"nds_screenshot","description":"Capture the current display. Returns the image inline unless a path is given.","inputSchema":{"type":"object","properties":{"screen":{"type":"string","description":"both (default), main or touch."},"path":{"type":"string","description":"Write the image to this file instead of returning it inline."},"format":{"type":"string","description":"png (default) or bmp. An explicit .png/.bmp extension in path wins."}}}},
{"name":"nds_input_key","description":"Press or release a DS button, optionally holding it for a number of frames.","inputSchema":{"type":"object","properties":{"button":{"type":"string","description":"A, B, X, Y, start, select, up, down, left, right, L, R, debug or lid."},"pressed":{"type":"boolean","description":"true to press (default), false to release."},"frames":{"type":"integer","description":"Hold for this many frames, run them, then release. Default 0 (latch)."}},"required":["button"]}},
{"name":"nds_input_touch","description":"Touch the bottom screen, optionally holding the touch for a number of frames.","inputSchema":{"type":"object","properties":{"x":{"type":"integer","description":"0-255."},"y":{"type":"integer","description":"0-191."},"touch":{"type":"boolean","description":"true to touch (default), false to release."},"frames":{"type":"integer","description":"Hold for this many frames, run them, then release. Default 0 (latch)."}}}},
{"name":"nds_input_release_all","description":"Release every button and the touch screen.","inputSchema":{"type":"object","properties":{}}}
]})json";

/*
	The catalog above is laid out one tool per line for readability, but the stdio
	transport frames messages on newlines, so responses have to stay on one line.
	The line breaks only ever sit between JSON tokens, never inside a string.
*/
static const std::string& CompactToolsJSON()
{
	static std::string compact;
	if (compact.empty())
	{
		for (const char *p = TOOLS_JSON; *p != '\0'; p++)
		{
			if (*p != '\n' && *p != '\r')
				compact.push_back(*p);
		}
	}
	return compact;
}

//---------------------------------------------------------------------------
// JSON-RPC dispatch
//---------------------------------------------------------------------------

static std::string CallTool(const std::string &name, const mcpjson::Value &args)
{
	if (name == "nds_get_state")              return ToolGetState();
	if (name == "nds_pause")                  return ToolPause();
	if (name == "nds_resume")                 return ToolResume();
	if (name == "nds_step")                   return ToolStep(args);
	if (name == "nds_step_over")              return ToolStepOver(args);
	if (name == "nds_step_out")               return ToolStepOut(args);
	if (name == "nds_run_frames")             return ToolRunFrames(args);
	if (name == "nds_reset")                  return ToolReset();
	if (name == "nds_quit")                   return ToolQuit();
	if (name == "nds_load_rom")               return ToolLoadROM(args);
	if (name == "nds_reload_rom")             return ToolReloadROM();
	if (name == "nds_get_rom_info")           return ToolGetROMInfo();
	if (name == "nds_read_memory")            return ToolReadMemory(args);
	if (name == "nds_write_memory")           return ToolWriteMemory(args);
	if (name == "nds_search_memory")          return ToolSearchMemory(args);
	if (name == "nds_get_registers")          return ToolGetRegisters(args);
	if (name == "nds_set_register")           return ToolSetRegister(args);
	if (name == "nds_disassemble")            return ToolDisassemble(args);
	if (name == "nds_set_breakpoint")         return ToolSetBreakpoint(args);
	if (name == "nds_clear_breakpoint")       return ToolClearBreakpoint(args);
	if (name == "nds_clear_all_breakpoints")  return ToolClearAllBreakpoints();
	if (name == "nds_list_breakpoints")       return ToolListBreakpoints();
	if (name == "nds_save_state")             return ToolSaveState(args);
	if (name == "nds_load_state")             return ToolLoadState(args);
	if (name == "nds_screenshot")             return ToolScreenshot(args);
	if (name == "nds_input_key")              return ToolInputKey(args);
	if (name == "nds_input_touch")            return ToolInputTouch(args);
	if (name == "nds_input_release_all")      return ToolInputReleaseAll();

	return std::string();
}

static std::string NegotiateProtocolVersion(const mcpjson::Value *params)
{
	if (params != NULL)
	{
		const std::string requested = params->GetString("protocolVersion");
		for (size_t i = 0; i < sizeof(SUPPORTED_PROTOCOL_VERSIONS) / sizeof(SUPPORTED_PROTOCOL_VERSIONS[0]); i++)
		{
			if (requested == SUPPORTED_PROTOCOL_VERSIONS[i])
				return requested;
		}
	}
	return DEFAULT_PROTOCOL_VERSION;
}

/* Returns false when the message is a notification and needs no response. */
static bool HandleMessage(const mcpjson::Value &message, std::string &outResponse)
{
	if (!message.IsObject())
	{
		outResponse = MakeError("null", -32600, "Invalid Request: expected a JSON-RPC object");
		return true;
	}

	const mcpjson::Value *id = message.Find("id");
	const bool isNotification = (id == NULL);
	const std::string idLiteral = IDLiteral(id);

	const mcpjson::Value *methodValue = message.Find("method");
	if (methodValue == NULL || !methodValue->IsString())
	{
		if (isNotification)
			return false;
		outResponse = MakeError(idLiteral, -32600, "Invalid Request: missing method");
		return true;
	}

	const std::string method = methodValue->str;
	const mcpjson::Value *params = message.Find("params");

	//notifications never get an answer
	if (isNotification)
	{
		//notifications/initialized, notifications/cancelled and friends need no work here
		return false;
	}

	if (method == "initialize")
	{
		std::string result("{\"protocolVersion\":");
		result += mcpjson::Quote(NegotiateProtocolVersion(params));
		result += ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":";
		result += mcpjson::Quote(SERVER_NAME);
		result += ",\"version\":";
		result += mcpjson::Quote(SERVER_VERSION);
		result += "},\"instructions\":\"Debug and drive a Nintendo DS ROM running in DeSmuME. Use nds_get_state first, nds_run_frames to advance emulation deterministically, and nds_screenshot to look at the screens.\"}";
		outResponse = MakeResult(idLiteral, result);
		return true;
	}

	if (method == "ping")
	{
		outResponse = MakeResult(idLiteral, "{}");
		return true;
	}

	if (method == "tools/list")
	{
		outResponse = MakeResult(idLiteral, CompactToolsJSON());
		return true;
	}

	if (method == "resources/list")
	{
		outResponse = MakeResult(idLiteral, "{\"resources\":[]}");
		return true;
	}

	if (method == "resources/templates/list")
	{
		outResponse = MakeResult(idLiteral, "{\"resourceTemplates\":[]}");
		return true;
	}

	if (method == "prompts/list")
	{
		outResponse = MakeResult(idLiteral, "{\"prompts\":[]}");
		return true;
	}

	if (method == "logging/setLevel")
	{
		outResponse = MakeResult(idLiteral, "{}");
		return true;
	}

	if (method == "tools/call")
	{
		if (params == NULL || !params->IsObject())
		{
			outResponse = MakeError(idLiteral, -32602, "Invalid params: expected an object");
			return true;
		}

		const std::string name = params->GetString("name");
		if (name.empty())
		{
			outResponse = MakeError(idLiteral, -32602, "Invalid params: missing tool name");
			return true;
		}

		const mcpjson::Value *arguments = params->Find("arguments");
		const mcpjson::Value emptyArguments;
		const mcpjson::Value &args = (arguments != NULL && arguments->IsObject()) ? *arguments : emptyArguments;

		const std::string result = CallTool(name, args);
		if (result.empty())
		{
			outResponse = MakeError(idLiteral, -32602, "Unknown tool: " + name);
			return true;
		}

		outResponse = MakeResult(idLiteral, result);
		return true;
	}

	outResponse = MakeError(idLiteral, -32601, "Method not found: " + method);
	return true;
}

/* Returns false when nothing should be sent back. */
static bool ProcessRequest(const char *request, std::string &outResponse)
{
	if (request == NULL || request[0] == '\0')
		return false;

	mcpjson::Value message;
	if (!mcpjson::Parse(request, strlen(request), message))
	{
		outResponse = MakeError("null", -32700, "Parse error");
		return true;
	}

	//JSON-RPC batch
	if (message.IsArray())
	{
		if (message.items.empty())
		{
			outResponse = MakeError("null", -32600, "Invalid Request: empty batch");
			return true;
		}

		std::string batch("[");
		bool any = false;
		for (size_t i = 0; i < message.items.size(); i++)
		{
			std::string single;
			if (!HandleMessage(message.items[i], single))
				continue;
			if (any)
				batch += ",";
			batch += single;
			any = true;
		}
		batch += "]";

		if (!any)
			return false;

		outResponse.swap(batch);
		return true;
	}

	return HandleMessage(message, outResponse);
}

static char* DuplicateString(const std::string &text)
{
	char *copy = (char *)malloc(text.size() + 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text.c_str(), text.size() + 1);
	return copy;
}

//---------------------------------------------------------------------------
// Cross thread request queue
//---------------------------------------------------------------------------

namespace
{

struct PendingRequest
{
	PendingRequest(const char *text) : request(text != NULL ? text : ""), hasResponse(false), done(false) {}

	std::string request;
	std::string response;
	bool hasResponse;
	bool done;
};

typedef std::shared_ptr<PendingRequest> PendingRequestPtr;

} //anonymous namespace

static std::mutex g_queueMutex;
static std::condition_variable g_queueSignal;
static std::condition_variable g_doneSignal;
static std::deque<PendingRequestPtr> g_queue;
static bool g_shuttingDown = false;

char* mcp_server_process_alloc(const char *request)
{
	std::string response;
	if (!ProcessRequest(request, response))
		return NULL;
	return DuplicateString(response);
}

char* mcp_server_dispatch(const char *request, int timeout_ms)
{
	PendingRequestPtr pending(new PendingRequest(request));

	{
		std::lock_guard<std::mutex> lock(g_queueMutex);
		if (g_shuttingDown)
			return DuplicateString(MakeError("null", -32000, "Emulator is shutting down"));
		g_queue.push_back(pending);
	}
	g_queueSignal.notify_one();

	{
		std::unique_lock<std::mutex> lock(g_queueMutex);
		if (timeout_ms > 0)
		{
			g_doneSignal.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&pending]{ return pending->done; });
		}
		else
		{
			g_doneSignal.wait(lock, [&pending]{ return pending->done; });
		}

		if (!pending->done)
		{
			//the emulation thread never picked it up; drop it from the queue
			for (std::deque<PendingRequestPtr>::iterator it = g_queue.begin(); it != g_queue.end(); ++it)
			{
				if (*it == pending)
				{
					g_queue.erase(it);
					break;
				}
			}
			return DuplicateString(MakeError("null", -32001, "Timed out waiting for the emulation thread"));
		}

		if (!pending->hasResponse)
			return NULL;

		return DuplicateString(pending->response);
	}
}

int mcp_server_poll(int timeout_ms)
{
	std::unique_lock<std::mutex> lock(g_queueMutex);

	if (g_queue.empty() && timeout_ms > 0)
		g_queueSignal.wait_for(lock, std::chrono::milliseconds(timeout_ms), []{ return !g_queue.empty() || g_shuttingDown; });

	int handled = 0;
	while (!g_queue.empty())
	{
		PendingRequestPtr pending = g_queue.front();
		g_queue.pop_front();

		lock.unlock();
		std::string response;
		const bool hasResponse = ProcessRequest(pending->request.c_str(), response);
		lock.lock();

		pending->response.swap(response);
		pending->hasResponse = hasResponse;
		pending->done = true;
		handled++;
	}

	lock.unlock();
	if (handled > 0)
		g_doneSignal.notify_all();

	return handled;
}

int mcp_server_quit_requested(void)
{
	return g_quitRequested ? 1 : 0;
}

//---------------------------------------------------------------------------
// Transports
//---------------------------------------------------------------------------

/* Tools such as nds_run_frames can legitimately keep the emulation thread busy. */
static const int DISPATCH_TIMEOUT_MS = 120000;

static std::thread g_stdioThread;
static std::atomic<bool> g_stdioRunning(false);
static bool g_httpRunning = false;
static FILE *g_protocolOut = NULL;

static char* HTTPProcess(const char *body)
{
	return mcp_server_dispatch(body, DISPATCH_TIMEOUT_MS);
}

int mcp_server_start_http(int port)
{
	if (g_httpRunning)
		return 0;

	if (mcp_http_start(port, HTTPProcess) != 0)
		return -1;

	g_httpRunning = true;
	return 0;
}

void mcp_server_stop_http(void)
{
	if (!g_httpRunning)
		return;
	mcp_http_stop();
	g_httpRunning = false;
}

/*
	stdout belongs to the protocol once the stdio transport is in use. Anything the
	emulator prints would corrupt the stream, so hand the real stdout to the
	transport and point the process' stdout at stderr. Frontends should call this
	before any other subsystem gets a chance to print.
*/
void mcp_server_capture_stdout(void)
{
	if (g_protocolOut != NULL)
		return;

	fflush(stdout);
	const int duplicated = MCP_DUP(MCP_FILENO(stdout));
	if (duplicated >= 0)
	{
		MCP_DUP2(MCP_FILENO(stderr), MCP_FILENO(stdout));
		g_protocolOut = MCP_FDOPEN(duplicated, "w");
	}
	if (g_protocolOut == NULL)
		g_protocolOut = stdout;
}

static void StdioThreadProc()
{
	std::string line;

	while (g_stdioRunning.load())
	{
		const int c = fgetc(stdin);
		if (c == EOF)
			break;

		if (c != '\n' && c != '\r')
		{
			line.push_back((char)c);
			continue;
		}

		if (line.empty())
			continue;

		char *response = mcp_server_dispatch(line.c_str(), DISPATCH_TIMEOUT_MS);
		line.clear();

		if (response == NULL)
			continue;

		FILE *out = (g_protocolOut != NULL) ? g_protocolOut : stdout;
		fwrite(response, 1, strlen(response), out);
		fputc('\n', out);
		fflush(out);
		free(response);
	}
}

int mcp_server_start_stdio(void)
{
	if (g_stdioRunning.load())
		return 0;

	mcp_server_capture_stdout();

	g_stdioRunning.store(true);
	g_stdioThread = std::thread(StdioThreadProc);
	return 0;
}

void mcp_server_stop_stdio(void)
{
	if (!g_stdioRunning.load())
		return;

	g_stdioRunning.store(false);

	/*
		The reader is parked inside a blocking read on stdin and there is no portable
		way to interrupt that, so let it go and rely on process teardown.
	*/
	if (g_stdioThread.joinable())
		g_stdioThread.detach();
}

//---------------------------------------------------------------------------
// Lifetime
//---------------------------------------------------------------------------

void mcp_server_init(mcp_set_execute_fn set_execute, mcp_get_execute_fn get_execute)
{
	g_setExecute = set_execute;
	g_getExecute = get_execute;
	g_quitRequested = false;

	std::lock_guard<std::mutex> lock(g_queueMutex);
	g_shuttingDown = false;
}

void mcp_server_deinit(void)
{
	/*
		Close the queue before stopping the transports: a transport thread parked in
		mcp_server_dispatch() is waiting for this very thread, so joining it first
		would deadlock until the dispatch timeout expired.
	*/
	{
		std::lock_guard<std::mutex> lock(g_queueMutex);
		g_shuttingDown = true;

		//unblock anything still waiting on the emulation thread
		while (!g_queue.empty())
		{
			PendingRequestPtr pending = g_queue.front();
			g_queue.pop_front();
			pending->response = MakeError("null", -32000, "Emulator is shutting down");
			pending->hasResponse = true;
			pending->done = true;
		}
	}

	g_queueSignal.notify_all();
	g_doneSignal.notify_all();

	mcp_server_stop_http();
	mcp_server_stop_stdio();

	g_setExecute = NULL;
	g_getExecute = NULL;
}
