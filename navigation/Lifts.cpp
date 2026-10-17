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
 *  File: Lifts.cpp
 *
 *  Desc:
 *  Implementation of Lifts (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/Lifts.h"

#include <algorithm>
#include <cmath>

namespace og
{

namespace
{

constexpr std::uint32_t kKalimdor = 1;
// The same car from one sighting to the next, and the same shaft as a lift known.
constexpr float kSameShaft = 3.0f;
// Up and down at least this far between two stops: a lift, not a door.
constexpr float kRise = 10.0f;
constexpr float kStill = 0.02f;
// Not seen for this long: out of sight, its car not known.
constexpr Duration kGone{1500};

} // namespace

LiftWatch::LiftWatch(const IClock& clock) : clock_(clock)
{
    // Thunder Bluff's Mesa Elevators by the Mulgore road, two cars side by
    // side, measured going up and down (the WoW Forever client, October 2026).
    lifts_.push_back({kKalimdor, -1286.24f, 189.72f, 68.84f, 130.04f});
    lifts_.push_back({kKalimdor, -1308.38f, 185.29f, 68.59f, 130.08f});
}

bool LiftWatch::isLift(const GameObjectState& object)
{
    return object.name.find("Elevator") != std::string::npos;
}

void LiftWatch::update(const GameSnapshot& snapshot)
{
    if (!snapshot.mapId)
    {
        return;
    }

    const TimePoint now = clock_.now();

    for (const auto& o : snapshot.objects)
    {
        if (!isLift(o))
        {
            continue;
        }

        auto it = std::find_if(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.guid == o.guid; });

        if (it == seen_.end())
        {
            // How long it has stood still is not known until it has been seen
            // coming in: a car found standing may be about to leave.
            seen_.push_back({o.guid, *snapshot.mapId, o.position, o.position.z, o.position.z, false, TimePoint{}, now});
            continue;
        }

        Seen& s = *it;
        const bool moving = std::fabs(o.position.z - s.at.z) > kStill || distance2D(o.position, s.at) > kStill;

        if (!moving && s.moving)
        {
            s.stillSince = now;
        }

        s.moving = moving;
        s.at = o.position;
        s.seenAt = now;

        // Where it stands still is a stop: the lowest and the highest one.
        if (!moving)
        {
            s.low = std::min(s.low, o.position.z);
            s.high = std::max(s.high, o.position.z);
        }

        const bool known = std::any_of(lifts_.begin(), lifts_.end(), [&](const Lift& l) { return l.mapId == s.mapId && std::hypot(l.x - s.at.x, l.y - s.at.y) <= kSameShaft; });

        if (!known && s.high - s.low >= kRise)
        {
            lifts_.push_back({s.mapId, s.at.x, s.at.y, s.low, s.high});
        }
    }
}

std::optional<LiftCar> LiftWatch::car(const Lift& lift) const
{
    const TimePoint now = clock_.now();

    for (const Seen& s : seen_)
    {
        if (s.mapId == lift.mapId && now - s.seenAt <= kGone && std::hypot(lift.x - s.at.x, lift.y - s.at.y) <= kSameShaft)
        {
            return LiftCar{s.at.z, s.moving, s.moving ? Duration{} : now - s.stillSince};
        }
    }

    return std::nullopt;
}

} // namespace og
