/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-17
 *  File: Ships.h
 *
 *  Desc:
 *  The zeppelins and boats in sight: whether each is parked at its
 *  dock, leaving it or coming in to one.
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Vec3.h"
#include "og/state/GameSnapshot.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace og
{

enum class ShipState
{
    Moving,   // under way, neither from nor to a dock known
    Parked,   // standing at a dock
    Leaving,  // away from the dock it stood at
    Entering  // in to a dock it has stood at, or slowing to stop at one
};

const char* toString(ShipState state);

// The ships seen (GameObjectKind::Ship), judged by the snapshots' times: a
// ship that has stood still is parked, and where it stood is one of its docks.
class ShipWatch
{
public:
    void update(const GameSnapshot& snapshot);
    std::optional<ShipState> state(Guid guid) const;
    // Where the ship has been seen parked, on the map it is on now.
    std::vector<Vec3> docks(Guid guid) const;

    // Still: moved less than kStill over kStillFor.
    static constexpr float kStill = 0.3f;
    static constexpr Duration kStillFor{1500};
    // Coming in to a dock known from this far.
    static constexpr float kEnteringFrom = 400.0f;

private:
    struct Seen
    {
        std::uint32_t mapId = 0;
        Vec3 at;
        Vec3 sample;
        TimePoint firstAt{};
        TimePoint sampleAt{};
        TimePoint movedAt{};
        TimePoint seenAt{};
        float speed = 0.0f;       // yards a second, between samples
        float slowerFor = 0.0f;   // seconds it has been slowing down
        std::optional<Vec3> left; // the dock it stood at last
        std::vector<std::pair<std::uint32_t, Vec3>> docks;
        ShipState state = ShipState::Moving;
    };

    std::map<Guid, Seen> seen_;
};

// The ships' and zeppelins' positions as CSV lines, once a second each and at
// every change of state: their routes, docks and timetables measured, for
// travel between the continents. Each ship once, known by its map and name;
// the player's columns left empty on board, where its position is the ship's.
class ShipLog
{
public:
    static constexpr Duration kEvery{1000};
    // One ship listed twice: the same name this close.
    static constexpr float kSamePlace = 1.0f;
    // On board: the player's position this near naught, and every ship
    // farther from it than the client sees.
    static constexpr float kAboardWithin = 50.0f;
    static constexpr float kSight = 1500.0f;

    // The line naming the columns.
    static std::string header();
    // The lines for this snapshot; `wallMs` the wall clock's milliseconds.
    std::vector<std::string> lines(const GameSnapshot& snapshot, const ShipWatch& watch, std::int64_t wallMs);

private:
    // By map and name.
    std::map<std::string, TimePoint> writtenAt_;
    std::map<std::string, ShipState> stateWritten_;
};

} // namespace og
