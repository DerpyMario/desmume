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

#include "mcp_image.h"

#include <zlib.h>

#include <cctype>
#include <cstdio>
#include <cstring>


static const char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const u8 *data, size_t length)
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

void ConvertNative15ToRGB24(const u16 *src, u8 *dst, size_t pixelCount)
{
	for (size_t i = 0; i < pixelCount; i++)
	{
		const u16 pixel = src[i];
		dst[i * 3 + 0] = (u8)(((pixel >> 0) & 0x1F) << 3);
		dst[i * 3 + 1] = (u8)(((pixel >> 5) & 0x1F) << 3);
		dst[i * 3 + 2] = (u8)(((pixel >> 10) & 0x1F) << 3);
	}
}

std::vector<u8> BuildBMP24(const u8 *rgb, int width, int height)
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
std::vector<u8> BuildPNG24(const u8 *rgb, int width, int height)
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

bool WriteFileBytes(const char *path, const u8 *data, size_t length)
{
	FILE *fp = fopen(path, "wb");
	if (fp == NULL)
		return false;
	const size_t written = fwrite(data, 1, length, fp);
	fclose(fp);
	return (written == length);
}

bool HasExtension(const std::string &path, const char *extension)
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
