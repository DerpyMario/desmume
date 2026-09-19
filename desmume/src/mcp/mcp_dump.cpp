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

#include "mcp_dump.h"

#include "mcp_gfx.h"
#include "mcp_image.h"

#include "../GPU.h"

#include <cstdio>
#include <cstring>

/* Enough character data to cover a layer's worth, laid out as a square. */
static const int DUMP_TILE_COUNT = 1024;
static const int DUMP_TILE_COLUMNS = 32;

bool MCPDumpParseSelection(const std::string &names, u32 &outSelection, std::string &outError)
{
	outSelection = 0;

	size_t start = 0;
	while (start <= names.size())
	{
		size_t end = names.find(',', start);
		if (end == std::string::npos)
			end = names.size();

		std::string name = names.substr(start, end - start);
		//trim, so that a list written with spaces still works
		while (!name.empty() && (name[0] == ' ' || name[0] == '\t'))
			name.erase(0, 1);
		while (!name.empty() && (name[name.size() - 1] == ' ' || name[name.size() - 1] == '\t'))
			name.erase(name.size() - 1);

		if (!name.empty())
		{
			if (name == "all")           outSelection |= MCP_DUMP_ALL;
			else if (name == "screen")   outSelection |= MCP_DUMP_SCREEN;
			else if (name == "palette")  outSelection |= MCP_DUMP_PALETTE;
			else if (name == "tiles")    outSelection |= MCP_DUMP_TILES;
			else if (name == "sprites")  outSelection |= MCP_DUMP_SPRITES;
			else
			{
				outError = "unknown dump '" + name + "', expected screen, palette, tiles, sprites or all";
				return false;
			}
		}

		if (end == names.size())
			break;
		start = end + 1;
	}

	if (outSelection == 0)
	{
		outError = "nothing to dump, expected screen, palette, tiles, sprites or all";
		return false;
	}

	return true;
}

static std::string JoinPath(const std::string &directory, const std::string &name)
{
	if (directory.empty())
		return name;
	if (directory[directory.size() - 1] == '/' || directory[directory.size() - 1] == '\\')
		return directory + name;
	return directory + "/" + name;
}

static bool WriteImage(const std::vector<u8> &rgb, int width, int height, const std::string &path,
					   std::vector<std::string> &outPaths, std::string &outError)
{
	if (rgb.empty())
	{
		outError = "could not build " + path;
		return false;
	}

	const std::vector<u8> png = BuildPNG24(&rgb[0], width, height);
	if (png.empty() || !WriteFileBytes(path.c_str(), &png[0], png.size()))
	{
		outError = "could not write " + path;
		return false;
	}

	outPaths.push_back(path);
	return true;
}

static bool WriteText(const std::string &text, const std::string &path,
					  std::vector<std::string> &outPaths, std::string &outError)
{
	if (!WriteFileBytes(path.c_str(), (const u8 *)text.c_str(), text.size()))
	{
		outError = "could not write " + path;
		return false;
	}

	outPaths.push_back(path);
	return true;
}

static bool DumpScreen(const std::string &directory, std::vector<std::string> &outPaths, std::string &outError)
{
	const NDSDisplayInfo &displayInfo = GPU->GetDisplayInfo();
	const u16 *frameBuffer = displayInfo.masterNativeBuffer16;
	if (frameBuffer == NULL)
	{
		outError = "no framebuffer to dump";
		return false;
	}

	const int width = GPU_FRAMEBUFFER_NATIVE_WIDTH;
	const int height = GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2;

	std::vector<u8> rgb((size_t)width * (size_t)height * 3);
	ConvertNative15ToRGB24(frameBuffer, &rgb[0], (size_t)width * (size_t)height);

	return WriteImage(rgb, width, height, JoinPath(directory, "screen.png"), outPaths, outError);
}

static bool DumpPalettes(const std::string &directory, std::vector<std::string> &outPaths, std::string &outError)
{
	std::string listing;

	for (int engineIndex = 0; engineIndex < 2; engineIndex++)
	{
		for (int typeIndex = 0; typeIndex < 2; typeIndex++)
		{
			const MCPGfxEngine engine = (MCPGfxEngine)engineIndex;
			const MCPGfxPaletteType type = (MCPGfxPaletteType)typeIndex;

			u16 colors[256];
			MCPGfxReadPalette(engine, type, colors);

			char name[64];
			snprintf(name, sizeof(name), "palette-%s-%s.png",
				MCPGfxEngineName(engine), MCPGfxPaletteTypeName(type));

			int width = 0;
			int height = 0;
			const std::vector<u8> rgb = MCPGfxBuildPaletteImage(colors, 8, width, height);
			if (!WriteImage(rgb, width, height, JoinPath(directory, name), outPaths, outError))
				return false;

			char heading[128];
			snprintf(heading, sizeof(heading), "%s engine %s palette at 0x%08X\n",
				MCPGfxEngineName(engine), MCPGfxPaletteTypeName(type),
				(unsigned)MCPGfxPaletteAddress(engine, type));

			if (!listing.empty())
				listing += "\n\n";
			listing += heading;
			listing += MCPGfxFormatPaletteText(colors);
			listing += "\n";
		}
	}

	return WriteText(listing, JoinPath(directory, "palettes.txt"), outPaths, outError);
}

static bool DumpTiles(const std::string &directory, std::vector<std::string> &outPaths, std::string &outError)
{
	for (int engineIndex = 0; engineIndex < 2; engineIndex++)
	{
		const MCPGfxEngine engine = (MCPGfxEngine)engineIndex;

		u16 backgroundColors[256];
		MCPGfxReadPalette(engine, MCP_GFX_PALETTE_BG, backgroundColors);

		for (int layer = 0; layer < 4; layer++)
		{
			MCPGfxBackgroundInfo info;
			if (!MCPGfxGetBackgroundInfo(engine, layer, info))
				continue;

			int width = 0;
			int height = 0;
			const std::vector<u8> rgb = MCPGfxBuildTileImage(info.characterBase, DUMP_TILE_COUNT,
				info.bitsPerPixel, backgroundColors, 0, DUMP_TILE_COLUMNS, 1, width, height);

			char name[64];
			snprintf(name, sizeof(name), "tiles-%s-bg%d.png", MCPGfxEngineName(engine), layer);
			if (!WriteImage(rgb, width, height, JoinPath(directory, name), outPaths, outError))
				return false;
		}

		u16 objectColors[256];
		MCPGfxReadPalette(engine, MCP_GFX_PALETTE_OBJ, objectColors);

		MCPGfxObjectInfo objectInfo;
		MCPGfxGetObjectInfo(engine, objectInfo);

		int width = 0;
		int height = 0;
		const std::vector<u8> rgb = MCPGfxBuildTileImage(objectInfo.characterBase, DUMP_TILE_COUNT,
			4, objectColors, 0, DUMP_TILE_COLUMNS, 1, width, height);

		char name[64];
		snprintf(name, sizeof(name), "tiles-%s-obj.png", MCPGfxEngineName(engine));
		if (!WriteImage(rgb, width, height, JoinPath(directory, name), outPaths, outError))
			return false;
	}

	return true;
}

static bool DumpSprites(const std::string &directory, std::vector<std::string> &outPaths, std::string &outError)
{
	std::string listing;

	for (int engineIndex = 0; engineIndex < 2; engineIndex++)
	{
		const MCPGfxEngine engine = (MCPGfxEngine)engineIndex;

		int shown = 0;
		const std::string table = MCPGfxFormatOAMText(engine, false, 128, shown);

		char heading[96];
		snprintf(heading, sizeof(heading), "%s engine, %d sprite(s)\n", MCPGfxEngineName(engine), shown);

		if (!listing.empty())
			listing += "\n\n";
		listing += heading;
		listing += table;
		listing += "\n";
	}

	return WriteText(listing, JoinPath(directory, "sprites.txt"), outPaths, outError);
}

bool MCPDumpWrite(u32 selection, const std::string &directory,
				  std::vector<std::string> &outPaths, std::string &outError)
{
	if (GPU == NULL)
	{
		outError = "the GPU is not initialized";
		return false;
	}

	if ((selection & MCP_DUMP_SCREEN) && !DumpScreen(directory, outPaths, outError))
		return false;
	if ((selection & MCP_DUMP_PALETTE) && !DumpPalettes(directory, outPaths, outError))
		return false;
	if ((selection & MCP_DUMP_TILES) && !DumpTiles(directory, outPaths, outError))
		return false;
	if ((selection & MCP_DUMP_SPRITES) && !DumpSprites(directory, outPaths, outError))
		return false;

	return true;
}
