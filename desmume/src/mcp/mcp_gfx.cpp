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

#include "mcp_gfx.h"

#include "../armcpu.h"
#include "../MMU.h"

#include <cstdio>
#include <cstring>

/* Palette memory: main BG, main OBJ, sub BG, sub OBJ, 512 bytes each. */
static const u32 PALETTE_BASE = 0x05000000;

/* Object attribute memory, one 1 KiB table per engine. */
static const u32 OAM_BASE = 0x07000000;
static const int OAM_SPRITE_COUNT = 128;

static u8 ReadByte(u32 address)
{
	return _MMU_read08(ARMCPU_ARM9, MMU_AT_DEBUG, address);
}

static u16 ReadHalfword(u32 address)
{
	return _MMU_read16(ARMCPU_ARM9, MMU_AT_DEBUG, address);
}

//---------------------------------------------------------------------------
// Naming
//---------------------------------------------------------------------------

bool MCPGfxParseEngine(const std::string &name, MCPGfxEngine &outEngine)
{
	if (name.empty() || name == "main" || name == "a" || name == "A" || name == "0")
	{
		outEngine = MCP_GFX_ENGINE_MAIN;
		return true;
	}
	if (name == "sub" || name == "b" || name == "B" || name == "1")
	{
		outEngine = MCP_GFX_ENGINE_SUB;
		return true;
	}
	return false;
}

bool MCPGfxParsePaletteType(const std::string &name, MCPGfxPaletteType &outType)
{
	if (name.empty() || name == "bg" || name == "background")
	{
		outType = MCP_GFX_PALETTE_BG;
		return true;
	}
	if (name == "obj" || name == "sprite" || name == "sprites")
	{
		outType = MCP_GFX_PALETTE_OBJ;
		return true;
	}
	return false;
}

const char* MCPGfxEngineName(MCPGfxEngine engine)
{
	return (engine == MCP_GFX_ENGINE_SUB) ? "sub" : "main";
}

const char* MCPGfxPaletteTypeName(MCPGfxPaletteType type)
{
	return (type == MCP_GFX_PALETTE_OBJ) ? "obj" : "bg";
}

u32 MCPGfxPaletteAddress(MCPGfxEngine engine, MCPGfxPaletteType type)
{
	return PALETTE_BASE + (u32)engine * 0x400 + (u32)type * 0x200;
}

//---------------------------------------------------------------------------
// Palettes
//---------------------------------------------------------------------------

void MCPGfxReadPaletteAt(u32 address, u16 outColors[256])
{
	for (int i = 0; i < 256; i++)
		outColors[i] = ReadHalfword(address + (u32)i * 2);
}

void MCPGfxReadPalette(MCPGfxEngine engine, MCPGfxPaletteType type, u16 outColors[256])
{
	MCPGfxReadPaletteAt(MCPGfxPaletteAddress(engine, type), outColors);
}

static void ColorToRGB(u16 color, u8 &r, u8 &g, u8 &b)
{
	r = (u8)(((color >> 0) & 0x1F) << 3);
	g = (u8)(((color >> 5) & 0x1F) << 3);
	b = (u8)(((color >> 10) & 0x1F) << 3);
}

std::string MCPGfxFormatPaletteText(const u16 colors[256])
{
	std::string text;
	char entry[32];

	for (int i = 0; i < 256; i++)
	{
		if ((i % 8) == 0)
		{
			if (i != 0)
				text.push_back('\n');
			snprintf(entry, sizeof(entry), "%3d:", i);
			text += entry;
		}

		u8 r, g, b;
		ColorToRGB(colors[i], r, g, b);
		snprintf(entry, sizeof(entry), " %04X=#%02X%02X%02X", (unsigned)colors[i], r, g, b);
		text += entry;
	}

	return text;
}

std::vector<u8> MCPGfxBuildPaletteImage(const u16 colors[256], int cellSize, int &outWidth, int &outHeight)
{
	if (cellSize < 1) cellSize = 1;
	if (cellSize > 64) cellSize = 64;

	outWidth = 16 * cellSize;
	outHeight = 16 * cellSize;

	std::vector<u8> rgb((size_t)outWidth * (size_t)outHeight * 3);
	for (int index = 0; index < 256; index++)
	{
		u8 r, g, b;
		ColorToRGB(colors[index], r, g, b);

		const int cellX = (index % 16) * cellSize;
		const int cellY = (index / 16) * cellSize;

		for (int y = 0; y < cellSize; y++)
		{
			u8 *row = &rgb[((size_t)(cellY + y) * (size_t)outWidth + (size_t)cellX) * 3];
			for (int x = 0; x < cellSize; x++)
			{
				row[x * 3 + 0] = r;
				row[x * 3 + 1] = g;
				row[x * 3 + 2] = b;
			}
		}
	}

	return rgb;
}

//---------------------------------------------------------------------------
// Tiles
//---------------------------------------------------------------------------

std::vector<u8> MCPGfxBuildTileImage(u32 address, int tileCount, int bitsPerPixel,
									 const u16 colors[256], int paletteIndex,
									 int columns, int scale, int &outWidth, int &outHeight)
{
	std::vector<u8> rgb;

	if (bitsPerPixel != 4 && bitsPerPixel != 8)
		return rgb;
	if (tileCount < 1 || columns < 1 || scale < 1)
		return rgb;

	const int rows = (tileCount + columns - 1) / columns;
	outWidth = columns * 8 * scale;
	outHeight = rows * 8 * scale;

	rgb.assign((size_t)outWidth * (size_t)outHeight * 3, 0);

	//4 bit tiles take their colours from one 16 colour slice of the palette
	const int paletteBase = (bitsPerPixel == 4) ? ((paletteIndex & 0x0F) * 16) : 0;
	const u32 bytesPerTile = (bitsPerPixel == 4) ? 32 : 64;

	for (int tile = 0; tile < tileCount; tile++)
	{
		const u32 tileAddress = address + (u32)tile * bytesPerTile;
		const int tileX = (tile % columns) * 8 * scale;
		const int tileY = (tile / columns) * 8 * scale;

		for (int y = 0; y < 8; y++)
		{
			for (int x = 0; x < 8; x++)
			{
				int colorIndex;
				if (bitsPerPixel == 4)
				{
					const u8 pair = ReadByte(tileAddress + (u32)(y * 4 + x / 2));
					colorIndex = ((x & 1) != 0) ? (pair >> 4) : (pair & 0x0F);
					colorIndex += paletteBase;
				}
				else
				{
					colorIndex = ReadByte(tileAddress + (u32)(y * 8 + x));
				}

				u8 r, g, b;
				ColorToRGB(colors[colorIndex & 0xFF], r, g, b);

				for (int sy = 0; sy < scale; sy++)
				{
					u8 *row = &rgb[((size_t)(tileY + y * scale + sy) * (size_t)outWidth + (size_t)(tileX + x * scale)) * 3];
					for (int sx = 0; sx < scale; sx++)
					{
						row[sx * 3 + 0] = r;
						row[sx * 3 + 1] = g;
						row[sx * 3 + 2] = b;
					}
				}
			}
		}
	}

	return rgb;
}

//---------------------------------------------------------------------------
// Where the engines keep their character data
//---------------------------------------------------------------------------

/* Display control, and the four background control registers after it. */
static u32 DisplayControlAddress(MCPGfxEngine engine)
{
	return (engine == MCP_GFX_ENGINE_SUB) ? 0x04001000 : 0x04000000;
}

/* The VRAM window each engine reads backgrounds and objects through. */
static u32 BackgroundVRAMBase(MCPGfxEngine engine)
{
	return (engine == MCP_GFX_ENGINE_SUB) ? 0x06200000 : 0x06000000;
}

static u32 ObjectVRAMBase(MCPGfxEngine engine)
{
	return (engine == MCP_GFX_ENGINE_SUB) ? 0x06600000 : 0x06400000;
}

bool MCPGfxGetBackgroundInfo(MCPGfxEngine engine, int layer, MCPGfxBackgroundInfo &outInfo)
{
	if (layer < 0 || layer > 3)
		return false;

	const u32 controlAddress = DisplayControlAddress(engine);
	const u32 displayControl = _MMU_read32(ARMCPU_ARM9, MMU_AT_DEBUG, controlAddress);
	const u16 backgroundControl = ReadHalfword(controlAddress + 8 + (u32)layer * 2);

	outInfo.enabled = ((displayControl >> (8 + layer)) & 1) != 0;
	outInfo.mode = (int)(displayControl & 7);
	outInfo.priority = backgroundControl & 3;
	outInfo.bitsPerPixel = ((backgroundControl & 0x0080) != 0) ? 8 : 4;

	//only the main engine offsets its bases, by 64 KiB steps out of DISPCNT
	const u32 characterOffset = (engine == MCP_GFX_ENGINE_MAIN) ? (((displayControl >> 24) & 7) * 0x10000) : 0;
	const u32 screenOffset = (engine == MCP_GFX_ENGINE_MAIN) ? (((displayControl >> 27) & 7) * 0x10000) : 0;

	outInfo.characterBase = BackgroundVRAMBase(engine) + characterOffset + (u32)((backgroundControl >> 2) & 0x0F) * 0x4000;
	outInfo.screenBase = BackgroundVRAMBase(engine) + screenOffset + (u32)((backgroundControl >> 8) & 0x1F) * 0x800;
	outInfo.extendedPalettes = ((displayControl & 0x40000000) != 0);

	return true;
}

void MCPGfxGetObjectInfo(MCPGfxEngine engine, MCPGfxObjectInfo &outInfo)
{
	const u32 displayControl = _MMU_read32(ARMCPU_ARM9, MMU_AT_DEBUG, DisplayControlAddress(engine));

	outInfo.enabled = ((displayControl >> 12) & 1) != 0;
	outInfo.oneDimensional = ((displayControl >> 4) & 1) != 0;
	outInfo.boundary = outInfo.oneDimensional ? (32 << ((displayControl >> 20) & 3)) : 32;
	outInfo.characterBase = ObjectVRAMBase(engine);
	outInfo.extendedPalettes = ((displayControl & 0x80000000u) != 0);
}

//---------------------------------------------------------------------------
// Sprites
//---------------------------------------------------------------------------

/* [shape][size] to width and height, as the hardware lays them out. */
static const u8 SPRITE_WIDTH[4][4] =
{
	{  8, 16, 32, 64 },  //square
	{ 16, 32, 32, 64 },  //wide
	{  8,  8, 16, 32 },  //tall
	{  8,  8,  8,  8 }   //reserved
};

static const u8 SPRITE_HEIGHT[4][4] =
{
	{  8, 16, 32, 64 },
	{  8,  8, 16, 32 },
	{ 16, 32, 32, 64 },
	{  8,  8,  8,  8 }
};

std::string MCPGfxFormatOAMText(MCPGfxEngine engine, bool onlyVisible, int maxSprites, int &outShown)
{
	if (maxSprites < 1) maxSprites = 1;
	if (maxSprites > OAM_SPRITE_COUNT) maxSprites = OAM_SPRITE_COUNT;

	const u32 base = OAM_BASE + (u32)engine * 0x400;

	MCPGfxObjectInfo objectInfo;
	MCPGfxGetObjectInfo(engine, objectInfo);

	char heading[160];
	snprintf(heading, sizeof(heading),
		"objects %s, character data at 0x%08X, %s mapping, %d bytes per tile number%s\n"
		"idx   x   y    w   h  tile  pal pri mode        flags",
		objectInfo.enabled ? "on" : "off", (unsigned)objectInfo.characterBase,
		objectInfo.oneDimensional ? "1D" : "2D", objectInfo.boundary,
		objectInfo.extendedPalettes ? ", extended palettes on" : "");

	std::string text = heading;
	outShown = 0;

	for (int i = 0; i < maxSprites; i++)
	{
		const u32 entry = base + (u32)i * 8;
		const u16 attr0 = ReadHalfword(entry + 0);
		const u16 attr1 = ReadHalfword(entry + 2);
		const u16 attr2 = ReadHalfword(entry + 4);

		const int shape = (attr1 >> 14) & 3;
		const int size = (attr0 >> 14) & 3;
		const bool rotationScaling = ((attr0 & 0x0100) != 0);
		//without rotation and scaling, this bit disables the sprite instead
		const bool disabled = !rotationScaling && ((attr0 & 0x0200) != 0);
		const bool doubleSize = rotationScaling && ((attr0 & 0x0200) != 0);

		if (onlyVisible && disabled)
			continue;

		const int y = attr0 & 0xFF;
		const int x = attr1 & 0x1FF;
		const int mode = (attr0 >> 10) & 3;
		const bool mosaic = ((attr0 & 0x1000) != 0);
		const bool is256Color = ((attr0 & 0x2000) != 0);
		const int tile = attr2 & 0x03FF;
		const int priority = (attr2 >> 10) & 3;
		const int palette = (attr2 >> 12) & 0x0F;

		static const char *MODE_NAMES[4] = { "normal", "blend", "window", "bitmap" };

		std::string flags;
		if (disabled) flags += "disabled ";
		if (rotationScaling) flags += "rotscale ";
		if (doubleSize) flags += "double ";
		if (!rotationScaling && (attr1 & 0x1000)) flags += "flipH ";
		if (!rotationScaling && (attr1 & 0x2000)) flags += "flipV ";
		if (mosaic) flags += "mosaic ";
		flags += is256Color ? "256col" : "16col";

		char line[192];
		snprintf(line, sizeof(line), "\n%3d %3d %3d  %3d %3d  %4d  %3d  %d  %-10s %s",
			i, x, y,
			(int)SPRITE_WIDTH[shape][size], (int)SPRITE_HEIGHT[shape][size],
			tile, palette, priority, MODE_NAMES[mode], flags.c_str());
		text += line;
		outShown++;
	}

	if (outShown == 0)
		text += "\n(no sprites)";

	return text;
}
