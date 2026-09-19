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

#ifndef MCP_DUMP_H
#define MCP_DUMP_H

#include <string>
#include <vector>

#include "../types.h"

/*
	Writing the screens, the palettes, the character data and the sprite tables out as
	files. The MCP tools return these one at a time to a client that asks; this is the
	same content written in a batch, which is what the --dump command line wants.
*/

enum MCPDumpSelection
{
	MCP_DUMP_SCREEN  = 1 << 0,
	MCP_DUMP_PALETTE = 1 << 1,
	MCP_DUMP_TILES   = 1 << 2,
	MCP_DUMP_SPRITES = 1 << 3,
	MCP_DUMP_ALL     = MCP_DUMP_SCREEN | MCP_DUMP_PALETTE | MCP_DUMP_TILES | MCP_DUMP_SPRITES
};

/*
	Reads a comma separated list of screen, palette, tiles, sprites and all.
	Returns false and describes the problem when a name is not one of those.
*/
bool MCPDumpParseSelection(const std::string &names, u32 &outSelection, std::string &outError);

/*
	Writes the selected dumps into directory, which must already exist. Returns the
	paths written, and false with outError set when something could not be written.
*/
bool MCPDumpWrite(u32 selection, const std::string &directory,
				  std::vector<std::string> &outPaths, std::string &outError);

#endif
