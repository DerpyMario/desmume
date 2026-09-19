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

#ifndef MCP_GFX_H
#define MCP_GFX_H

#include <string>
#include <vector>

#include "../types.h"

/*
	Reading what the 2D engines are drawing with: the palettes, the tiles behind them,
	and the sprite table. Everything is read through the debug side of the MMU, so the
	VRAM bank mapping in force is the one the game set up, and nothing here disturbs
	emulation.
*/

/* Which 2D engine, and which of its two palettes. */
enum MCPGfxEngine { MCP_GFX_ENGINE_MAIN = 0, MCP_GFX_ENGINE_SUB = 1 };
enum MCPGfxPaletteType { MCP_GFX_PALETTE_BG = 0, MCP_GFX_PALETTE_OBJ = 1 };

/* Parse "main"/"sub" and "bg"/"obj". Returns false when the name is not one of them. */
bool MCPGfxParseEngine(const std::string &name, MCPGfxEngine &outEngine);
bool MCPGfxParsePaletteType(const std::string &name, MCPGfxPaletteType &outType);

const char* MCPGfxEngineName(MCPGfxEngine engine);
const char* MCPGfxPaletteTypeName(MCPGfxPaletteType type);

/* Address in palette memory of one of the four standard 256 colour palettes. */
u32 MCPGfxPaletteAddress(MCPGfxEngine engine, MCPGfxPaletteType type);

/* Reads 256 colours in their native X1B5G5R5 form. */
void MCPGfxReadPalette(MCPGfxEngine engine, MCPGfxPaletteType type, u16 outColors[256]);

/* The same, from any address, for palettes that do not live in palette memory. */
void MCPGfxReadPaletteAt(u32 address, u16 outColors[256]);

/* A listing of the 256 colours: index, raw halfword and #RRGGBB, 8 per line. */
std::string MCPGfxFormatPaletteText(const u16 colors[256]);

/*
	The palette as a 16 by 16 grid of swatches, cellSize pixels each, as 24 bit RGB.
	outWidth and outHeight receive the image size.
*/
std::vector<u8> MCPGfxBuildPaletteImage(const u16 colors[256], int cellSize, int &outWidth, int &outHeight);

/*
	Decodes character data into a grid of 8x8 tiles, as 24 bit RGB.

	bitsPerPixel is 4 or 8. A 4 bit tile takes its colours from one 16 colour slice of
	the palette, chosen by paletteIndex; an 8 bit tile uses the whole 256 colours.
	Colour 0 is drawn as it is stored, which is what a tile viewer wants to see, so a
	fully transparent tile shows up as a block of the background colour.
	Returns an empty vector when the arguments do not describe a usable image.
*/
std::vector<u8> MCPGfxBuildTileImage(u32 address, int tileCount, int bitsPerPixel,
									 const u16 colors[256], int paletteIndex,
									 int columns, int scale, int &outWidth, int &outHeight);

/*
	Where an engine keeps the character data a layer draws with, read from its display
	control registers, so a tile dump can be aimed without knowing the register layout.
*/
struct MCPGfxBackgroundInfo
{
	bool enabled;
	int mode;              //the engine's BG mode, 0-6
	int bitsPerPixel;      //4 or 8
	int priority;
	u32 characterBase;
	u32 screenBase;
	bool extendedPalettes;  //colours come from VRAM slots, not from palette memory
};

struct MCPGfxObjectInfo
{
	bool enabled;
	bool oneDimensional;   //tile mapping, 1D or 2D
	int boundary;          //bytes per tile number step in 1D mapping
	u32 characterBase;
	bool extendedPalettes;  //colours come from VRAM slots, not from palette memory
};

/* layer is 0-3. Returns false when the layer number is out of range. */
bool MCPGfxGetBackgroundInfo(MCPGfxEngine engine, int layer, MCPGfxBackgroundInfo &outInfo);
void MCPGfxGetObjectInfo(MCPGfxEngine engine, MCPGfxObjectInfo &outInfo);

/* One line per sprite: position, size, tile, palette and the flags that matter. */
std::string MCPGfxFormatOAMText(MCPGfxEngine engine, bool onlyVisible, int maxSprites, int &outShown);

#endif
