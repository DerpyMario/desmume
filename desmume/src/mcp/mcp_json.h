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

#ifndef MCP_JSON_H
#define MCP_JSON_H

#include <string>
#include <utility>
#include <vector>

#include "../types.h"

/*
	A tiny JSON reader used by the MCP server. It is deliberately self contained
	so that no new third party dependency is pulled into the emulator core.
*/
namespace mcpjson
{

enum Type
{
	TYPE_NULL,
	TYPE_BOOL,
	TYPE_NUMBER,
	TYPE_STRING,
	TYPE_ARRAY,
	TYPE_OBJECT
};

class Value
{
public:
	Value() : type(TYPE_NULL), boolean(false), number(0.0) {}

	Type type;
	bool boolean;
	double number;
	std::string raw;    /* verbatim source text of a number, used to echo ids back */
	std::string str;
	std::vector<Value> items;
	std::vector<std::pair<std::string, Value> > members;

	bool IsNull() const { return this->type == TYPE_NULL; }
	bool IsObject() const { return this->type == TYPE_OBJECT; }
	bool IsArray() const { return this->type == TYPE_ARRAY; }
	bool IsString() const { return this->type == TYPE_STRING; }
	bool IsNumber() const { return this->type == TYPE_NUMBER; }
	bool IsBool() const { return this->type == TYPE_BOOL; }

	/* Object member lookup. Returns NULL when absent or when this is not an object. */
	const Value* Find(const char *key) const;

	std::string GetString(const char *key, const char *defaultValue = "") const;
	long GetInt(const char *key, long defaultValue = 0) const;
	bool GetBool(const char *key, bool defaultValue = false) const;

	/* Reads an unsigned value written either as a JSON number or as a
	   (optionally 0x prefixed) hex/decimal string. Returns false when absent. */
	bool GetAddress(const char *key, u32 &outValue) const;

	std::string AsString() const;
	long AsInt() const;
};

/* Parses a complete JSON document. Returns false on malformed input. */
bool Parse(const char *text, size_t length, Value &outValue);
bool Parse(const std::string &text, Value &outValue);

/* Escapes a string for embedding between JSON quotes (quotes not included). */
std::string Escape(const std::string &in);

/* Convenience: returns "\"escaped\"". */
std::string Quote(const std::string &in);

} //namespace mcpjson

#endif
