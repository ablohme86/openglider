/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: Json.cpp
 *
 *  Desc:
 *  Implementation of Json (common utility functions, math, logging, and
 *  data structures).
 *
 *
 */
#include "og/common/Json.h"

#include <charconv>
#include <cmath>
#include <format>

namespace og
{

namespace
{

const Json kNull;
const std::string kEmptyString;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

class Parser
{
public:
    explicit Parser(std::string_view text) : text_(text)
    {
    }

    Json parseDocument()
    {
        skipWhitespace();
        Json v = parseValue(0);
        skipWhitespace();

        if (pos_ != text_.size())
        {
            fail("unexpected trailing characters");
        }

        return v;
    }

private:
    static constexpr int kMaxDepth = 512;

    [[noreturn]] void fail(const std::string& msg) const
    {
        std::size_t line = 1, col = 1;

        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i)
        {
            if (text_[i] == '\n')
            {
                ++line;
                col = 1;
            }
            else
            {
                ++col;
            }
        }

        throw JsonError(std::format("JSON parse error at {}:{}: {}", line, col, msg), line, col);
    }

    bool atEnd() const
    {
        return pos_ >= text_.size();
    }

    char peek() const
    {
        return atEnd() ? '\0' : text_[pos_];
    }

    void skipWhitespace()
    {
        while (!atEnd())
        {
            const char c = text_[pos_];

            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                ++pos_;
            }
            else
            {
                break;
            }
        }
    }

    void expect(char c)
    {
        if (peek() != c)
        {
            fail(std::format("expected '{}'", c));
        }

        ++pos_;
    }

    bool consumeLiteral(std::string_view lit)
    {
        if (text_.substr(pos_, lit.size()) == lit)
        {
            pos_ += lit.size();
            return true;
        }

        return false;
    }

    Json parseValue(int depth)
    {
        if (depth > kMaxDepth)
        {
            fail("nesting too deep");
        }

        switch (peek())
        {
        case '{':
            return parseObject(depth);
        case '[':
            return parseArray(depth);
        case '"':
            return Json(parseString());
        case 't':
            if (consumeLiteral("true"))
            {
                return Json(true);
            }

            break;
        case 'f':
            if (consumeLiteral("false"))
            {
                return Json(false);
            }

            break;
        case 'n':
            if (consumeLiteral("null"))
            {
                return Json(nullptr);
            }

            break;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
            {
                return parseNumber();
            }

            break;
        }

        fail("unexpected character");
    }

    Json parseObject(int depth)
    {
        expect('{');
        Json::Object obj;
        skipWhitespace();

        if (peek() == '}')
        {
            ++pos_;
            return Json(std::move(obj));
        }

        while (true)
        {
            skipWhitespace();

            if (peek() != '"')
            {
                fail("expected string key");
            }

            std::string key = parseString();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            Json value = parseValue(depth + 1);
            bool replaced = false;

            for (auto& m : obj)
            {
                if (m.first == key)
                {
                    m.second = std::move(value);
                    replaced = true;
                    break;
                }
            }

            if (!replaced)
            {
                obj.emplace_back(std::move(key), std::move(value));
            }

            skipWhitespace();

            if (peek() == ',')
            {
                ++pos_;
                continue;
            }

            if (peek() == '}')
            {
                ++pos_;
                break;
            }

            fail("expected ',' or '}'");
        }

        return Json(std::move(obj));
    }

    Json parseArray(int depth)
    {
        expect('[');
        Json::Array arr;
        skipWhitespace();

        if (peek() == ']')
        {
            ++pos_;
            return Json(std::move(arr));
        }

        while (true)
        {
            skipWhitespace();
            arr.push_back(parseValue(depth + 1));
            skipWhitespace();

            if (peek() == ',')
            {
                ++pos_;
                continue;
            }

            if (peek() == ']')
            {
                ++pos_;
                break;
            }

            fail("expected ',' or ']'");
        }

        return Json(std::move(arr));
    }

    static void appendUtf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::uint32_t parseHex4()
    {
        if (pos_ + 4 > text_.size())
        {
            fail("truncated \\u escape");
        }

        std::uint32_t v = 0;

        for (int i = 0; i < 4; ++i)
        {
            const char c = text_[pos_++];
            v <<= 4;

            if (c >= '0' && c <= '9')
            {
                v |= static_cast<std::uint32_t>(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            }
            else
            {
                fail("invalid hex digit in \\u escape");
            }
        }

        return v;
    }

    std::string parseString()
    {
        expect('"');
        std::string out;

        while (true)
        {
            if (atEnd())
            {
                fail("unterminated string");
            }

            const char c = text_[pos_++];

            if (c == '"')
            {
                break;
            }

            if (static_cast<unsigned char>(c) < 0x20)
            {
                fail("control character in string");
            }

            if (c != '\\')
            {
                out += c;
                continue;
            }

            if (atEnd())
            {
                fail("unterminated escape");
            }

            const char e = text_[pos_++];

            switch (e)
            {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
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
                std::uint32_t cp = parseHex4();

                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    if (!consumeLiteral("\\u"))
                    {
                        fail("missing low surrogate");
                    }

                    const std::uint32_t lo = parseHex4();

                    if (lo < 0xDC00 || lo > 0xDFFF)
                    {
                        fail("invalid low surrogate");
                    }

                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                {
                    fail("unexpected low surrogate");
                }

                appendUtf8(out, cp);
                break;
            }

            default:
                fail("invalid escape");
            }
        }

        return out;
    }

    Json parseNumber()
    {
        const std::size_t start = pos_;
        bool isFloat = false;

        if (peek() == '-')
        {
            ++pos_;
        }

        if (peek() == '0')
        {
            ++pos_;
        }
        else if (peek() >= '1' && peek() <= '9')
        {
            while (peek() >= '0' && peek() <= '9')
            {
                ++pos_;
            }
        }
        else
        {
            fail("invalid number");
        }

        if (peek() == '.')
        {
            isFloat = true;
            ++pos_;

            if (!(peek() >= '0' && peek() <= '9'))
            {
                fail("invalid number");
            }

            while (peek() >= '0' && peek() <= '9')
            {
                ++pos_;
            }
        }

        if (peek() == 'e' || peek() == 'E')
        {
            isFloat = true;
            ++pos_;

            if (peek() == '+' || peek() == '-')
            {
                ++pos_;
            }

            if (!(peek() >= '0' && peek() <= '9'))
            {
                fail("invalid number");
            }

            while (peek() >= '0' && peek() <= '9')
            {
                ++pos_;
            }
        }

        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;

        if (!isFloat)
        {
            std::int64_t iv = 0;
            auto [ptr, ec] = std::from_chars(first, last, iv);

            if (ec == std::errc() && ptr == last)
            {
                return Json(iv);
            }

            // Out of int64 range: fall through to double.
        }

        double dv = 0.0;
        auto [ptr, ec] = std::from_chars(first, last, dv);

        if (ec != std::errc() || ptr != last)
        {
            fail("invalid number");
        }

        return Json(dv);
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

void appendIndent(std::string& out, int indent, int depth)
{
    out += '\n';
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}

} // namespace

std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);

    for (const char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
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
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            }
            else
            {
                out += c;
            }
        }
    }

    return out;
}

bool Json::asBool(bool fallback) const
{
    if (const auto* b = std::get_if<bool>(&value_))
    {
        return *b;
    }

    return fallback;
}

double Json::asDouble(double fallback) const
{
    if (const auto* d = std::get_if<double>(&value_))
    {
        return *d;
    }

    if (const auto* i = std::get_if<std::int64_t>(&value_))
    {
        return static_cast<double>(*i);
    }

    return fallback;
}

std::int64_t Json::asInt(std::int64_t fallback) const
{
    if (const auto* i = std::get_if<std::int64_t>(&value_))
    {
        return *i;
    }

    if (const auto* d = std::get_if<double>(&value_))
    {
        return static_cast<std::int64_t>(*d);
    }

    return fallback;
}

const std::string& Json::asString() const
{
    if (const auto* s = std::get_if<std::string>(&value_))
    {
        return *s;
    }

    return kEmptyString;
}

std::string Json::asString(std::string_view fallback) const
{
    if (const auto* s = std::get_if<std::string>(&value_))
    {
        return *s;
    }

    return std::string(fallback);
}

const Json::Array& Json::asArray() const
{
    if (const auto* a = std::get_if<Array>(&value_))
    {
        return *a;
    }

    return kEmptyArray;
}

Json::Array& Json::asArray()
{
    if (isNull())
    {
        value_ = Array{};
    }

    if (auto* a = std::get_if<Array>(&value_))
    {
        return *a;
    }

    throw JsonError("JSON value is not an array");
}

const Json::Object& Json::asObject() const
{
    if (const auto* o = std::get_if<Object>(&value_))
    {
        return *o;
    }

    return kEmptyObject;
}

Json::Object& Json::asObject()
{
    if (isNull())
    {
        value_ = Object{};
    }

    if (auto* o = std::get_if<Object>(&value_))
    {
        return *o;
    }

    throw JsonError("JSON value is not an object");
}

const Json* Json::find(std::string_view key) const
{
    if (const auto* o = std::get_if<Object>(&value_))
    {
        for (const auto& m : *o)
        {
            if (m.first == key)
            {
                return &m.second;
            }
        }
    }

    return nullptr;
}

const Json& Json::operator[](std::string_view key) const
{
    const Json* v = find(key);
    return v ? *v : kNull;
}

Json& Json::operator[](std::string_view key)
{
    auto& obj = asObject();

    for (auto& m : obj)
    {
        if (m.first == key)
        {
            return m.second;
        }
    }

    obj.emplace_back(std::string(key), Json());
    return obj.back().second;
}

const Json& Json::operator[](std::size_t index) const
{
    if (const auto* a = std::get_if<Array>(&value_); a && index < a->size())
    {
        return (*a)[index];
    }

    return kNull;
}

void Json::push_back(Json value)
{
    asArray().push_back(std::move(value));
}

std::size_t Json::size() const
{
    if (const auto* a = std::get_if<Array>(&value_))
    {
        return a->size();
    }

    if (const auto* o = std::get_if<Object>(&value_))
    {
        return o->size();
    }

    return 0;
}

std::string Json::dump(int indent) const
{
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const
{
    switch (type())
    {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += std::get<bool>(value_) ? "true" : "false";
        break;
    case Type::Int:
        out += std::to_string(std::get<std::int64_t>(value_));
        break;
    case Type::Double:
    {
        const double d = std::get<double>(value_);

        if (!std::isfinite(d))
        {
            out += "null";
            break;
        }

        char buf[64];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d);
        out.append(buf, ptr);
        break;
    }

    case Type::String:
        out += '"';
        out += jsonEscape(std::get<std::string>(value_));
        out += '"';
        break;
    case Type::Array:
    {
        const auto& a = std::get<Array>(value_);
        out += '[';
        // Arrays of scalars (coordinates) stay on one line even when indenting.
        bool scalarOnly = true;

        for (const auto& v : a)
        {
            if (v.isArray() || v.isObject())
            {
                scalarOnly = false;
            }
        }

        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (i > 0)
            {
                out += indent >= 0 && scalarOnly ? ", " : ",";
            }

            if (indent >= 0 && !scalarOnly)
            {
                appendIndent(out, indent, depth + 1);
            }

            a[i].dumpTo(out, indent, depth + 1);
        }

        if (indent >= 0 && !scalarOnly && !a.empty())
        {
            appendIndent(out, indent, depth);
        }

        out += ']';
        break;
    }

    case Type::Object:
    {
        const auto& o = std::get<Object>(value_);
        out += '{';

        for (std::size_t i = 0; i < o.size(); ++i)
        {
            if (i > 0)
            {
                out += ',';
            }

            if (indent >= 0)
            {
                appendIndent(out, indent, depth + 1);
            }

            out += '"';
            out += jsonEscape(o[i].first);
            out += indent >= 0 ? "\": " : "\":";
            o[i].second.dumpTo(out, indent, depth + 1);
        }

        if (indent >= 0 && !o.empty())
        {
            appendIndent(out, indent, depth);
        }

        out += '}';
        break;
    }
    }
}

Json Json::parse(std::string_view text)
{
    return Parser(text).parseDocument();
}

} // namespace og
