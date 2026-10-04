// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Json.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace xrpilot
{

namespace
{

class Parser
{
public:
    explicit Parser(const std::string& text)
        : s_(text)
    {
    }

    bool document(Json& out)
    {
        skipSpace();
        if (!value(out, 0))
            return false;
        skipSpace();
        return i_ == s_.size();
    }

private:
    void skipSpace()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
            ++i_;
    }

    bool literal(const char* word)
    {
        size_t n = 0;
        while (word[n] != '\0')
            ++n;
        if (s_.compare(i_, n, word) != 0)
            return false;
        i_ += n;
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t c)
    {
        if (c < 0x80)
            out += char(c);
        else if (c < 0x800)
        {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
        else
        {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }

    bool hex4(uint32_t& out)
    {
        if (i_ + 4 > s_.size())
            return false;
        out = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= uint32_t(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool string(std::string& out)
    {
        if (i_ >= s_.size() || s_[i_] != '"')
            return false;
        ++i_;
        while (i_ < s_.size())
        {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                return false;
            const char e = s_[i_++];
            switch (e)
            {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u':
            {
                uint32_t c1 = 0;
                if (!hex4(c1))
                    return false;
                if (c1 >= 0xD800 && c1 < 0xDC00 && s_.compare(i_, 2, "\\u") == 0)
                {
                    i_ += 2;
                    uint32_t c2 = 0;
                    if (!hex4(c2) || c2 < 0xDC00 || c2 >= 0xE000)
                        return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                appendUtf8(out, c1);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool number(std::string& out)
    {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-')
            ++i_;
        const size_t digits = i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' ||
                                  s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
            ++i_;
        if (i_ == digits)
            return false;
        out = s_.substr(start, i_ - start);
        return true;
    }

    bool value(Json& out, int depth)
    {
        if (depth > 64 || i_ >= s_.size())
            return false;
        const char c = s_[i_];
        if (c == '{')
        {
            out.kind = Json::Kind::Object;
            ++i_;
            skipSpace();
            if (i_ < s_.size() && s_[i_] == '}')
            {
                ++i_;
                return true;
            }
            while (true)
            {
                skipSpace();
                std::string key;
                if (!string(key))
                    return false;
                skipSpace();
                if (i_ >= s_.size() || s_[i_++] != ':')
                    return false;
                skipSpace();
                Json member;
                if (!value(member, depth + 1))
                    return false;
                out.members.emplace_back(std::move(key), std::move(member));
                skipSpace();
                if (i_ < s_.size() && s_[i_] == ',')
                {
                    ++i_;
                    continue;
                }
                return i_ < s_.size() && s_[i_++] == '}';
            }
        }
        if (c == '[')
        {
            out.kind = Json::Kind::Array;
            ++i_;
            skipSpace();
            if (i_ < s_.size() && s_[i_] == ']')
            {
                ++i_;
                return true;
            }
            while (true)
            {
                skipSpace();
                Json item;
                if (!value(item, depth + 1))
                    return false;
                out.items.push_back(std::move(item));
                skipSpace();
                if (i_ < s_.size() && s_[i_] == ',')
                {
                    ++i_;
                    continue;
                }
                return i_ < s_.size() && s_[i_++] == ']';
            }
        }
        if (c == '"')
        {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (literal("true"))
        {
            out.kind = Json::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false"))
        {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (literal("null"))
        {
            out.kind = Json::Kind::Null;
            return true;
        }
        out.kind = Json::Kind::Number;
        return number(out.text);
    }

    const std::string& s_;
    size_t i_ = 0;
};

void writeString(std::string& out, const std::string& s)
{
    out += '"';
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(c));
                out += escaped;
            }
            else
                out += c;
        }
    }
    out += '"';
}

void write(std::string& out, const Json& v, int indent)
{
    const std::string pad(size_t(indent + 1) * 4, ' ');
    const std::string close(size_t(indent) * 4, ' ');
    switch (v.kind)
    {
    case Json::Kind::Null:
        out += "null";
        break;
    case Json::Kind::Bool:
        out += v.boolean ? "true" : "false";
        break;
    case Json::Kind::Number:
        out += v.text;
        break;
    case Json::Kind::String:
        writeString(out, v.text);
        break;
    case Json::Kind::Array:
        if (v.items.empty())
        {
            out += "[]";
            break;
        }
        out += "[\n";
        for (size_t i = 0; i < v.items.size(); ++i)
        {
            out += pad;
            write(out, v.items[i], indent + 1);
            out += i + 1 < v.items.size() ? ",\n" : "\n";
        }
        out += close + "]";
        break;
    case Json::Kind::Object:
        if (v.members.empty())
        {
            out += "{}";
            break;
        }
        out += "{\n";
        for (size_t i = 0; i < v.members.size(); ++i)
        {
            out += pad;
            writeString(out, v.members[i].first);
            out += " : ";
            write(out, v.members[i].second, indent + 1);
            out += i + 1 < v.members.size() ? ",\n" : "\n";
        }
        out += close + "}";
        break;
    }
}

} // namespace

Json Json::string(std::string value)
{
    Json j;
    j.kind = Kind::String;
    j.text = std::move(value);
    return j;
}

const Json* Json::find(const std::string& key) const
{
    if (kind != Kind::Object)
        return nullptr;
    for (const std::pair<std::string, Json>& m : members)
    {
        if (m.first == key)
            return &m.second;
    }
    return nullptr;
}

Json* Json::find(const std::string& key)
{
    return const_cast<Json*>(static_cast<const Json*>(this)->find(key));
}

void Json::set(const std::string& key, Json value)
{
    kind = Kind::Object;
    if (Json* existing = find(key))
        *existing = std::move(value);
    else
        members.emplace_back(key, std::move(value));
}

bool Json::erase(const std::string& key)
{
    for (size_t i = 0; i < members.size(); ++i)
    {
        if (members[i].first == key)
        {
            members.erase(members.begin() + std::ptrdiff_t(i));
            return true;
        }
    }
    return false;
}

const std::string& Json::str() const
{
    static const std::string empty;
    return kind == Kind::String ? text : empty;
}

bool parseJson(const std::string& text, Json& out)
{
    out = Json();
    return Parser(text).document(out);
}

std::string writeJson(const Json& value)
{
    std::string out;
    write(out, value, 0);
    out += '\n';
    return out;
}

} // namespace xrpilot
