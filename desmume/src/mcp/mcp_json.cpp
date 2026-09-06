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

#include "mcp_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mcpjson
{

static const int MAX_DEPTH = 32;

namespace
{

class Reader
{
public:
	Reader(const char *text, size_t length) : p(text), end(text + length) {}

	bool ParseValue(Value &out, int depth);

private:
	const char *p;
	const char *end;

	void SkipWhitespace();
	bool ParseString(std::string &out);
	bool ParseNumber(Value &out);
	bool ParseLiteral(const char *literal);
	static void AppendUTF8(std::string &out, unsigned int codepoint);
};

void Reader::SkipWhitespace()
{
	while (this->p < this->end)
	{
		const char c = *this->p;
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			this->p++;
		else
			break;
	}
}

void Reader::AppendUTF8(std::string &out, unsigned int codepoint)
{
	if (codepoint < 0x80)
	{
		out.push_back((char)codepoint);
	}
	else if (codepoint < 0x800)
	{
		out.push_back((char)(0xC0 | (codepoint >> 6)));
		out.push_back((char)(0x80 | (codepoint & 0x3F)));
	}
	else if (codepoint < 0x10000)
	{
		out.push_back((char)(0xE0 | (codepoint >> 12)));
		out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (codepoint & 0x3F)));
	}
	else
	{
		out.push_back((char)(0xF0 | (codepoint >> 18)));
		out.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
		out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
		out.push_back((char)(0x80 | (codepoint & 0x3F)));
	}
}

static int HexDigit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool Reader::ParseString(std::string &out)
{
	if (this->p >= this->end || *this->p != '"')
		return false;
	this->p++;

	out.clear();
	while (this->p < this->end)
	{
		const char c = *this->p++;
		if (c == '"')
			return true;

		if (c != '\\')
		{
			out.push_back(c);
			continue;
		}

		if (this->p >= this->end)
			return false;

		const char esc = *this->p++;
		switch (esc)
		{
			case '"':  out.push_back('"');  break;
			case '\\': out.push_back('\\'); break;
			case '/':  out.push_back('/');  break;
			case 'b':  out.push_back('\b'); break;
			case 'f':  out.push_back('\f'); break;
			case 'n':  out.push_back('\n'); break;
			case 'r':  out.push_back('\r'); break;
			case 't':  out.push_back('\t'); break;
			case 'u':
			{
				if (this->end - this->p < 4)
					return false;
				unsigned int cp = 0;
				for (int i = 0; i < 4; i++)
				{
					const int digit = HexDigit(this->p[i]);
					if (digit < 0)
						return false;
					cp = (cp << 4) | (unsigned int)digit;
				}
				this->p += 4;

				//combine surrogate pairs when both halves are present
				if (cp >= 0xD800 && cp <= 0xDBFF && (this->end - this->p) >= 6 && this->p[0] == '\\' && this->p[1] == 'u')
				{
					unsigned int low = 0;
					bool lowOK = true;
					for (int i = 0; i < 4; i++)
					{
						const int digit = HexDigit(this->p[2 + i]);
						if (digit < 0) { lowOK = false; break; }
						low = (low << 4) | (unsigned int)digit;
					}
					if (lowOK && low >= 0xDC00 && low <= 0xDFFF)
					{
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						this->p += 6;
					}
				}
				AppendUTF8(out, cp);
				break;
			}
			default:
				return false;
		}
	}

	return false;
}

bool Reader::ParseNumber(Value &out)
{
	const char *start = this->p;
	if (this->p < this->end && (*this->p == '-' || *this->p == '+'))
		this->p++;
	bool anyDigits = false;
	while (this->p < this->end && *this->p >= '0' && *this->p <= '9') { this->p++; anyDigits = true; }
	if (this->p < this->end && *this->p == '.')
	{
		this->p++;
		while (this->p < this->end && *this->p >= '0' && *this->p <= '9') { this->p++; anyDigits = true; }
	}
	if (anyDigits && this->p < this->end && (*this->p == 'e' || *this->p == 'E'))
	{
		this->p++;
		if (this->p < this->end && (*this->p == '-' || *this->p == '+'))
			this->p++;
		while (this->p < this->end && *this->p >= '0' && *this->p <= '9')
			this->p++;
	}
	if (!anyDigits)
		return false;

	out.type = TYPE_NUMBER;
	out.raw.assign(start, (size_t)(this->p - start));
	out.number = strtod(out.raw.c_str(), NULL);
	return true;
}

bool Reader::ParseLiteral(const char *literal)
{
	const size_t len = strlen(literal);
	if ((size_t)(this->end - this->p) < len || strncmp(this->p, literal, len) != 0)
		return false;
	this->p += len;
	return true;
}

bool Reader::ParseValue(Value &out, int depth)
{
	if (depth > MAX_DEPTH)
		return false;

	this->SkipWhitespace();
	if (this->p >= this->end)
		return false;

	switch (*this->p)
	{
		case '{':
		{
			this->p++;
			out.type = TYPE_OBJECT;
			out.members.clear();
			this->SkipWhitespace();
			if (this->p < this->end && *this->p == '}') { this->p++; return true; }
			for (;;)
			{
				this->SkipWhitespace();
				std::string key;
				if (!this->ParseString(key))
					return false;
				this->SkipWhitespace();
				if (this->p >= this->end || *this->p != ':')
					return false;
				this->p++;
				Value member;
				if (!this->ParseValue(member, depth + 1))
					return false;
				out.members.push_back(std::make_pair(key, member));
				this->SkipWhitespace();
				if (this->p >= this->end)
					return false;
				if (*this->p == ',') { this->p++; continue; }
				if (*this->p == '}') { this->p++; return true; }
				return false;
			}
		}

		case '[':
		{
			this->p++;
			out.type = TYPE_ARRAY;
			out.items.clear();
			this->SkipWhitespace();
			if (this->p < this->end && *this->p == ']') { this->p++; return true; }
			for (;;)
			{
				Value item;
				if (!this->ParseValue(item, depth + 1))
					return false;
				out.items.push_back(item);
				this->SkipWhitespace();
				if (this->p >= this->end)
					return false;
				if (*this->p == ',') { this->p++; continue; }
				if (*this->p == ']') { this->p++; return true; }
				return false;
			}
		}

		case '"':
			out.type = TYPE_STRING;
			return this->ParseString(out.str);

		case 't':
			if (!this->ParseLiteral("true")) return false;
			out.type = TYPE_BOOL;
			out.boolean = true;
			return true;

		case 'f':
			if (!this->ParseLiteral("false")) return false;
			out.type = TYPE_BOOL;
			out.boolean = false;
			return true;

		case 'n':
			if (!this->ParseLiteral("null")) return false;
			out.type = TYPE_NULL;
			return true;

		default:
			return this->ParseNumber(out);
	}
}

} //anonymous namespace

const Value* Value::Find(const char *key) const
{
	if (this->type != TYPE_OBJECT || key == NULL)
		return NULL;
	for (size_t i = 0; i < this->members.size(); i++)
	{
		if (this->members[i].first == key)
			return &this->members[i].second;
	}
	return NULL;
}

std::string Value::AsString() const
{
	switch (this->type)
	{
		case TYPE_STRING: return this->str;
		case TYPE_NUMBER: return this->raw;
		case TYPE_BOOL:   return this->boolean ? "true" : "false";
		default:          return std::string();
	}
}

long Value::AsInt() const
{
	switch (this->type)
	{
		case TYPE_NUMBER: return (long)this->number;
		case TYPE_BOOL:   return this->boolean ? 1 : 0;
		case TYPE_STRING: return strtol(this->str.c_str(), NULL, 0);
		default:          return 0;
	}
}

std::string Value::GetString(const char *key, const char *defaultValue) const
{
	const Value *v = this->Find(key);
	if (v == NULL || v->IsNull())
		return std::string(defaultValue != NULL ? defaultValue : "");
	return v->AsString();
}

long Value::GetInt(const char *key, long defaultValue) const
{
	const Value *v = this->Find(key);
	if (v == NULL || v->IsNull())
		return defaultValue;
	return v->AsInt();
}

bool Value::GetBool(const char *key, bool defaultValue) const
{
	const Value *v = this->Find(key);
	if (v == NULL || v->IsNull())
		return defaultValue;
	if (v->IsBool())
		return v->boolean;
	if (v->IsNumber())
		return v->number != 0.0;
	if (v->IsString())
		return (v->str == "true" || v->str == "1");
	return defaultValue;
}

bool Value::GetAddress(const char *key, u32 &outValue) const
{
	const Value *v = this->Find(key);
	if (v == NULL || v->IsNull())
		return false;

	if (v->IsNumber())
	{
		outValue = (u32)strtoull(v->raw.c_str(), NULL, 10);
		return true;
	}

	if (v->IsString())
	{
		const char *s = v->str.c_str();
		while (*s == ' ' || *s == '\t') s++;
		int base = 16;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			s += 2;
		else if (s[0] == '#')
			s += 1;
		char *endPtr = NULL;
		const unsigned long long parsed = strtoull(s, &endPtr, base);
		if (endPtr == s)
			return false;
		outValue = (u32)parsed;
		return true;
	}

	return false;
}

bool Parse(const char *text, size_t length, Value &outValue)
{
	if (text == NULL)
		return false;
	Reader reader(text, length);
	return reader.ParseValue(outValue, 0);
}

bool Parse(const std::string &text, Value &outValue)
{
	return Parse(text.c_str(), text.size(), outValue);
}

std::string Escape(const std::string &in)
{
	std::string out;
	out.reserve(in.size() + 8);
	for (size_t i = 0; i < in.size(); i++)
	{
		const unsigned char c = (unsigned char)in[i];
		switch (c)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b";  break;
			case '\f': out += "\\f";  break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
					out += buf;
				}
				else
				{
					out.push_back((char)c);
				}
				break;
		}
	}
	return out;
}

std::string Quote(const std::string &in)
{
	std::string out("\"");
	out += Escape(in);
	out += "\"";
	return out;
}

} //namespace mcpjson
