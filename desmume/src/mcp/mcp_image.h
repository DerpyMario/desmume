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

#ifndef MCP_IMAGE_H
#define MCP_IMAGE_H

#include <string>
#include <vector>

#include "../types.h"

/* Encoding the MCP server needs to hand images to a client or to a file. */

std::string Base64Encode(const u8 *data, size_t length);

/* The native framebuffer and palette format, X1B5G5R5, to packed 24 bit RGB. */
void ConvertNative15ToRGB24(const u16 *src, u8 *dst, size_t pixelCount);

std::vector<u8> BuildBMP24(const u8 *rgb, int width, int height);

/* Returns an empty vector when compression fails. */
std::vector<u8> BuildPNG24(const u8 *rgb, int width, int height);

bool WriteFileBytes(const char *path, const u8 *data, size_t length);

/* Case insensitive, for choosing an output format from a file name. */
bool HasExtension(const std::string &path, const char *extension);

#endif
