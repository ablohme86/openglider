/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-09
 *  File: MovementTrace.cpp
 *
 *  Desc:
 *  Implementation of MovementTrace (bot engine, behaviors and runtime
 *  orchestration).
 *
 *
 */
#include "og/core/MovementTrace.h"

#include "og/common/MathUtil.h"
#include "og/state/GameSnapshot.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <sstream>

namespace og
{

namespace
{

constexpr std::int64_t kEveryMs = 100;
// Two samples further apart than this are not one stretch of movement: the
// other side had the keys between them, or the trace was not written.
constexpr std::int64_t kGapMs = 500;
constexpr double kMovingSpeed = 0.5;  // yards/s: slower is standing
constexpr double kTurningRate = 2.0;  // deg/s: slower is straight
constexpr double kStopSeconds = 0.5;  // still this long is a stop
constexpr double kKeyboardRate = 180; // deg/s

double percentile(std::vector<double> values, double share)
{
    if (values.empty())
    {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(share * static_cast<double>(values.size())))];
}

} // namespace

bool MovementTrace::open(const std::string& path)
{
    out_.open(path, std::ios::trunc);

    if (!out_)
    {
        return false;
    }

    out_ << "ms,x,y,z,heading,bot\n";
    return true;
}

void MovementTrace::add(const GameSnapshot& snapshot, TimePoint now, bool bot)
{
    if (!out_.is_open() || (started_ && now - last_ < Duration(kEveryMs)))
    {
        return;
    }

    started_ = started_.value_or(now);
    last_ = now;
    const auto& p = snapshot.player;
    out_ << std::format("{},{:.2f},{:.2f},{:.2f},{:.4f},{}\n", (now - *started_).count(), p.position.x, p.position.y, p.position.z, p.heading, bot ? 1 : 0);
    out_.flush();
}

std::optional<std::vector<MovementSample>> readMovementTrace(const std::string& path, std::string* error)
{
    std::ifstream in(path);

    if (!in)
    {
        if (error)
        {
            *error = "cannot read " + path;
        }

        return std::nullopt;
    }

    std::vector<MovementSample> samples;
    std::string line;
    std::getline(in, line); // the header

    while (std::getline(in, line))
    {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream fields(line);
        MovementSample s;
        int bot = 0;

        if (fields >> s.ms >> s.position.x >> s.position.y >> s.position.z >> s.heading >> bot)
        {
            s.bot = bot != 0;
            samples.push_back(s);
        }
    }

    return samples;
}

MovementStats movementStats(const std::vector<MovementSample>& samples, bool bot)
{
    MovementStats stats;
    std::vector<double> turns, stops;
    double moving = 0.0, standing = 0.0, straight = 0.0, keyboard = 0.0, turningInPlace = 0.0, still = 0.0;
    const MovementSample* previous = nullptr;
    const auto endStop = [&]
    {
        if (still >= kStopSeconds)
        {
            stops.push_back(still);
        }

        still = 0.0;
    };

    for (const auto& s : samples)
    {
        if (s.bot != bot)
        {
            previous = nullptr;
            endStop();
            continue;
        }

        ++stats.samples;

        if (previous && s.ms > previous->ms && s.ms - previous->ms <= kGapMs)
        {
            const double dt = static_cast<double>(s.ms - previous->ms) / 1000.0;
            const double step = distance2D(previous->position, s.position);
            const double rate = std::abs(normalizeAngle(s.heading - previous->heading)) * 180.0 / kPi / dt;
            stats.seconds += dt;
            stats.yards += step;

            if (step / dt >= kMovingSpeed)
            {
                moving += dt;
                endStop();

                if (rate < kTurningRate)
                {
                    straight += dt;
                }
                else
                {
                    turns.push_back(rate);
                    keyboard += std::abs(rate - kKeyboardRate) <= kKeyboardRate * 0.1 ? dt : 0.0;
                }
            }
            else
            {
                standing += dt;
                still += dt;
                turningInPlace += rate >= kTurningRate ? dt : 0.0;
            }
        }
        else
        {
            endStop();
        }

        previous = &s;
    }

    endStop();
    stats.movingShare = stats.seconds > 0 ? moving / stats.seconds : 0.0;
    stats.turnMedian = percentile(turns, 0.5);
    stats.turnP90 = percentile(turns, 0.9);
    stats.straightShare = moving > 0 ? straight / moving : 0.0;
    stats.keyboardTurnShare = moving > 0 ? keyboard / moving : 0.0;
    stats.stops = static_cast<int>(stops.size());
    stats.stopMedian = percentile(stops, 0.5);
    stats.stopP90 = percentile(stops, 0.9);
    stats.turnInPlaceShare = standing > 0 ? turningInPlace / standing : 0.0;
    return stats;
}

std::string describe(const MovementStats& s)
{
    if (s.samples < 2)
    {
        return "  nothing recorded\n";
    }

    return std::format("  recorded            {:.0f} s, {:.0f} yd, moving {:.0f}% of the time\n"
                       "  turns while moving  median {:.0f} deg/s, 90th percentile {:.0f} deg/s\n"
                       "  dead straight       {:.0f}% of the moving time\n"
                       "  at keyboard rate    {:.0f}% of the moving time (180 deg/s)\n"
                       "  stops               {} - median {:.1f} s, 90th percentile {:.1f} s\n"
                       "  turning on the spot {:.0f}% of the standing time\n",
                       s.seconds, s.yards, s.movingShare * 100, s.turnMedian, s.turnP90, s.straightShare * 100, s.keyboardTurnShare * 100, s.stops, s.stopMedian, s.stopP90, s.turnInPlaceShare * 100);
}

} // namespace og
