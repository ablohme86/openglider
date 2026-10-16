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
 *  File: Vec3.h
 *
 *  Desc:
 *  Declarations and interface for Vec3 (common utility functions, math,
 *  logging, and data structures).
 *
 *
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <format>
#include <span>

namespace og
{

// World position in game coordinates. For WoW: +X north, +Y west, +Z up.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_)
    {
    }

    constexpr Vec3 operator+(const Vec3& o) const
    {
        return {x + o.x, y + o.y, z + o.z};
    }

    constexpr Vec3 operator-(const Vec3& o) const
    {
        return {x - o.x, y - o.y, z - o.z};
    }

    constexpr Vec3 operator*(float s) const
    {
        return {x * s, y * s, z * s};
    }

    constexpr Vec3 operator/(float s) const
    {
        return {x / s, y / s, z / s};
    }

    constexpr Vec3& operator+=(const Vec3& o)
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }

    constexpr Vec3& operator-=(const Vec3& o)
    {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }

    constexpr bool operator==(const Vec3&) const = default;

    float length() const
    {
        return std::sqrt(x * x + y * y + z * z);
    }

    float length2D() const
    {
        return std::sqrt(x * x + y * y);
    }
};

inline float distance(const Vec3& a, const Vec3& b)
{
    return (a - b).length();
}

inline float distance2D(const Vec3& a, const Vec3& b)
{
    return (a - b).length2D();
}

inline float pathDistance(std::span<const Vec3> path)
{
    if (path.size() < 2)
    {
        return 0.0f;
    }

    float total = 0.0f;
    for (std::size_t i = 0; i + 1 < path.size(); ++i)
    {
        total += distance(path[i], path[i + 1]);
    }
    return total;
}

inline float remainingPathDistance(const Vec3& currentPos, std::span<const Vec3> path, std::size_t currentIndex = 0)
{
    if (path.empty() || currentIndex >= path.size())
    {
        return 0.0f;
    }

    float total = distance(currentPos, path[currentIndex]);
    for (std::size_t i = currentIndex; i + 1 < path.size(); ++i)
    {
        total += distance(path[i], path[i + 1]);
    }
    return total;
}

} // namespace og

template <> struct std::formatter<og::Vec3>
{
    constexpr auto parse(std::format_parse_context& ctx)
    {
        return ctx.begin();
    }

    auto format(const og::Vec3& v, std::format_context& ctx) const
    {
        return std::format_to(ctx.out(), "({:.2f}, {:.2f}, {:.2f})", v.x, v.y, v.z);
    }
};
