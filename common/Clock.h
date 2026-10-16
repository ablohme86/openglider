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
 *  File: Clock.h
 *
 *  Desc:
 *  Declarations and interface for Clock (common utility functions,
 *  math, logging, and data structures).
 *
 *
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace og
{

// All engine time is expressed in milliseconds on a monotonic timeline. The
// timeline is injected through IClock so that simulation, replay and unit
// tests can drive time deterministically.
using Duration = std::chrono::milliseconds;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock, Duration>;

using namespace std::chrono_literals;

constexpr Duration seconds(double s)
{
    return Duration(static_cast<std::int64_t>(s * 1000.0));
}

constexpr double toSeconds(Duration d)
{
    return static_cast<double>(d.count()) / 1000.0;
}

constexpr std::int64_t toMillis(TimePoint t)
{
    return t.time_since_epoch().count();
}

constexpr TimePoint fromMillis(std::int64_t ms)
{
    return TimePoint(Duration(ms));
}

class IClock
{
public:
    virtual ~IClock() = default;
    virtual TimePoint now() const = 0;
};

class SteadyClock final : public IClock
{
public:
    TimePoint now() const override
    {
        return std::chrono::time_point_cast<Duration>(std::chrono::steady_clock::now());
    }
};

// Clock driven explicitly by its owner (simulation, replay, tests).
class ManualClock final : public IClock
{
public:
    explicit ManualClock(TimePoint start = TimePoint(Duration(0))) : now_(start)
    {
    }

    TimePoint now() const override
    {
        return now_;
    }

    void set(TimePoint t)
    {
        now_ = t;
    }

    void advance(Duration d)
    {
        now_ += d;
    }

private:
    TimePoint now_;
};

} // namespace og
