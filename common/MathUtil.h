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
 *  File: MathUtil.h
 *
 *  Desc:
 *  Declarations and interface for MathUtil (common utility functions,
 *  math, logging, and data structures).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"

#include <cmath>

namespace og
{

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;

constexpr float degToRad(float deg)
{
    return deg * kPi / 180.0f;
}

constexpr float radToDeg(float rad)
{
    return rad * 180.0f / kPi;
}

// Wraps an angle difference into [-pi, pi).
inline float normalizeAngle(float a)
{
    a = std::fmod(a + kPi, kTwoPi);

    if (a < 0.0f)
    {
        a += kTwoPi;
    }

    return a - kPi;
}

// Wraps a heading into [0, 2pi).
inline float normalizeHeading(float a)
{
    a = std::fmod(a, kTwoPi);

    if (a < 0.0f)
    {
        a += kTwoPi;
    }

    return a;
}

// Heading from `from` towards `to` using the WoW convention: facing f moves
// the player along (cos f, sin f), and turning left increases the heading.
inline float headingTo(const Vec3& from, const Vec3& to)
{
    return normalizeHeading(std::atan2(to.y - from.y, to.x - from.x));
}

// Signed angle to turn from `heading` to face `to`. Positive = turn left.
inline float headingDelta(float heading, const Vec3& from, const Vec3& to)
{
    return normalizeAngle(headingTo(from, to) - heading);
}

inline Vec3 directionFromHeading(float heading)
{
    return {std::cos(heading), std::sin(heading), 0.0f};
}

} // namespace og
