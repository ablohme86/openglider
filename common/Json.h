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
 *  File: Json.h
 *
 *  Desc:
 *  Declarations and interface for Json (common utility functions, math,
 *  logging, and data structures).
 *
 *
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace og
{

class JsonError : public std::runtime_error
{
public:
    JsonError(const std::string& what, std::size_t line = 0, std::size_t column = 0) : std::runtime_error(what), line_(line), column_(column)
    {
    }

    std::size_t line() const
    {
        return line_;
    }

    std::size_t column() const
    {
        return column_;
    }

private:
    std::size_t line_;
    std::size_t column_;
};

// Small self-contained JSON value used for profiles, replays and the daemon
// API. Objects keep insertion order so written files stay readable.
class Json
{
public:
    enum class Type
    {
        Null,
        Bool,
        Int,
        Double,
        String,
        Array,
        Object
    };
    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t)
    {
    }

    Json(bool b) : value_(b)
    {
    }

    template <class T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    Json(T v) : value_(static_cast<std::int64_t>(v))
    {
    }

    template <class T>
        requires std::is_floating_point_v<T>
    Json(T v) : value_(static_cast<double>(v))
    {
    }

    Json(const char* s) : value_(std::string(s))
    {
    }

    Json(std::string_view s) : value_(std::string(s))
    {
    }

    Json(std::string s) : value_(std::move(s))
    {
    }

    Json(Array a) : value_(std::move(a))
    {
    }

    Json(Object o) : value_(std::move(o))
    {
    }

    static Json array()
    {
        return Json(Array{});
    }

    static Json object()
    {
        return Json(Object{});
    }

    Type type() const
    {
        return static_cast<Type>(value_.index());
    }

    bool isNull() const
    {
        return type() == Type::Null;
    }

    bool isBool() const
    {
        return type() == Type::Bool;
    }

    bool isNumber() const
    {
        return type() == Type::Int || type() == Type::Double;
    }

    bool isInt() const
    {
        return type() == Type::Int;
    }

    bool isString() const
    {
        return type() == Type::String;
    }

    bool isArray() const
    {
        return type() == Type::Array;
    }

    bool isObject() const
    {
        return type() == Type::Object;
    }

    bool asBool(bool fallback = false) const;
    double asDouble(double fallback = 0.0) const;
    float asFloat(float fallback = 0.0f) const
    {
        return static_cast<float>(asDouble(fallback));
    }

    std::int64_t asInt(std::int64_t fallback = 0) const;
    const std::string& asString() const;
    std::string asString(std::string_view fallback) const;

    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();

    // Object access. The const version returns a null value for missing keys.
    const Json& operator[](std::string_view key) const;
    Json& operator[](std::string_view key);
    const Json* find(std::string_view key) const;
    bool contains(std::string_view key) const
    {
        return find(key) != nullptr;
    }

    // Array access.
    const Json& operator[](std::size_t index) const;
    void push_back(Json value);

    std::size_t size() const;
    bool empty() const
    {
        return size() == 0;
    }

    // indent < 0: compact single line.
    std::string dump(int indent = -1) const;
    static Json parse(std::string_view text);

    bool operator==(const Json& other) const = default;

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> value_{nullptr};
};

std::string jsonEscape(std::string_view s);

} // namespace og
