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
// Moved this far within kSampleEvery: going. A car going covers some 1.6 yd.
constexpr float kMoved = 0.15f;
constexpr Duration kSampleEvery{250};
// Not moved for this long: standing at a stop.
constexpr Duration kStillFor{500};
// Not seen for this long: out of sight, its car not known.
constexpr Duration kGone{1500};

} // namespace

LiftWatch::LiftWatch(const IClock& clock) : clock_(clock)
{
    // Thunder Bluff's Mesa Elevators by the Mulgore road, two cars side by
    // side, measured going up and down (the WoW Forever client, October 2026).
    lifts_.push_back({kKalimdor, -1286.24f, 189.72f, 68.84f, 130.04f});
    lifts_.push_back({kKalimdor, -1308.38f, 185.29f, 68.59f, 130.08f});
    // And the two on the east side of the mesa.
    lifts_.push_back({kKalimdor, -1037.27f, -49.24f, 69.27f, 140.49f});
    lifts_.push_back({kKalimdor, -1028.04f, -28.36f, 69.02f, 140.50f});
    // The Great Lift, from the Barrens down to Thousand Needles: two cars
    // side by side.
    lifts_.push_back({kKalimdor, -4670.77f, -1849.61f, -44.14f, 85.41f});
    lifts_.push_back({kKalimdor, -4665.43f, -1827.68f, -43.89f, 85.41f});
    // Freewind Post's, from the Needles up to the mesa.
    lifts_.push_back({kKalimdor, -5382.49f, -2489.42f, -40.53f, 89.03f});
    lifts_.push_back({kKalimdor, -5399.19f, -2504.61f, -40.28f, 89.02f});
}

bool LiftWatch::isLift(const GameObjectState& object)
{
    // Thunder Bluff's "Mesa Elevator", "The Great Lift", or any transport -
    // a boat or a zeppelin too, which is never learnt for one: it goes about.
    return object.kind == GameObjectKind::Transport || object.name.find("Elevator") != std::string::npos || object.name.find("Lift") != std::string::npos;
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
            seen_.push_back({o.guid, *snapshot.mapId, o.position, o.position.z, o.position.z, o.position, o.position, now, TimePoint{}, now});
            continue;
        }

        Seen& s = *it;
        s.at = o.position;
        s.seenAt = now;

        if (distance(o.position, s.sample) > kMoved)
        {
            s.movedAt = now;
            s.sample = o.position;
            s.sampleAt = now;
        }
        else if (now - s.sampleAt >= kSampleEvery)
        {
            s.sample = o.position;
            s.sampleAt = now;
        }

        const bool moving = s.movedAt != TimePoint{} && now - s.movedAt < kStillFor;

        // Where it stands still is a stop: the lowest and the highest one.
        if (!moving)
        {
            s.low = std::min(s.low, o.position.z);
            s.high = std::max(s.high, o.position.z);
        }

        const bool known = std::any_of(lifts_.begin(), lifts_.end(), [&](const Lift& l) { return l.mapId == s.mapId && std::hypot(l.x - s.at.x, l.y - s.at.y) <= kSameShaft; });

        if (!known && s.high - s.low >= kRise && distance2D(s.at, s.first) <= kSameShaft)
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
            const bool moving = s.movedAt != TimePoint{} && now - s.movedAt < kStillFor;
            // Standing since it last moved; since ever, for one never seen going.
            return LiftCar{s.at.z, moving, moving ? Duration{} : s.movedAt == TimePoint{} ? Duration::max() : now - s.movedAt - kStillFor};
        }
    }

    return std::nullopt;
}

} // namespace og
