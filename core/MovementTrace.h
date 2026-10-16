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
 *  File: MovementTrace.h
 *
 *  Desc:
 *  Declarations and interface for MovementTrace (bot engine, behaviors and
 *  runtime orchestration).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Vec3.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace og
{

struct GameSnapshot;

// Where the character was and which way it faced, ten times a second.
struct MovementSample
{
    std::int64_t ms = 0; // since the trace began
    Vec3 position;
    float heading = 0.0f; // rad
    bool bot = false;     // the bot had the keys; else the player (attached, paused)
};

// What a trace looks like in numbers a bot is told from a person by: how it
// turns, how it stops, how straight it runs. Of the samples of one side only.
struct MovementStats
{
    std::size_t samples = 0;
    double seconds = 0.0;
    double yards = 0.0;
    double movingShare = 0.0; // of the time, 0 to 1
    // Turning, of the time spent moving: deg/s at the median and the 90th
    // percentile of the turns, the share run dead straight (under 2 deg/s) and
    // the share turned at the keyboard's rate (180 deg/s, 10% either way) -
    // a person turns with the mouse, at every rate.
    double turnMedian = 0.0;
    double turnP90 = 0.0;
    double straightShare = 0.0;
    double keyboardTurnShare = 0.0;
    // Stops (still for half a second and more): how many, and how long.
    int stops = 0;
    double stopMedian = 0.0;
    double stopP90 = 0.0;
    // Of the time standing: the share spent turning on the spot.
    double turnInPlaceShare = 0.0;
};

// Writes a session's movement to a CSV file ("ms,x,y,z,heading,bot"), to
// compare the bot's with the player's own: attached and paused, the player is
// at the keys, and is recorded as such.
class MovementTrace
{
public:
    // False where the file cannot be written; then add() does nothing.
    bool open(const std::string& path);
    bool isOpen() const
    {
        return out_.is_open();
    }

    // At most every 100 ms; `bot` is whether the bot has the keys.
    void add(const GameSnapshot& snapshot, TimePoint now, bool bot);

private:
    std::ofstream out_;
    std::optional<TimePoint> started_;
    TimePoint last_{};
};

std::optional<std::vector<MovementSample>> readMovementTrace(const std::string& path, std::string* error = nullptr);
// Of the bot's samples (bot = true) or the player's.
MovementStats movementStats(const std::vector<MovementSample>& samples, bool bot);
std::string describe(const MovementStats& stats);

} // namespace og
