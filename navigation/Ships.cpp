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
 *  File: Ships.cpp
 *
 *  Desc:
 *  Implementation of ShipWatch.
 *
 */
#include "og/navigation/Ships.h"

#include <algorithm>

namespace og
{

namespace
{
// Samples this far apart, for a speed that does not jump with each tick.
constexpr Duration kSampleEvery{500};
// The same dock, seen again.
constexpr float kSameDock = 20.0f;
// Slowing down this long, from no dock known: coming in to one.
constexpr float kSlowingFor = 2.0f;
} // namespace

const char* toString(ShipState state)
{
    switch (state)
    {
    case ShipState::Parked:
        return "Parked";
    case ShipState::Leaving:
        return "Leaving";
    case ShipState::Entering:
        return "Entering";
    case ShipState::Moving:
        break;
    }

    return "Moving";
}

void ShipWatch::update(const GameSnapshot& snapshot)
{
    if (!snapshot.mapId)
    {
        return;
    }

    const TimePoint now = snapshot.timestamp;

    for (const auto& o : snapshot.objects)
    {
        if (o.kind != GameObjectKind::Ship)
        {
            continue;
        }

        auto [it, fresh] = seen_.try_emplace(o.guid);
        Seen& s = it->second;

        if (fresh || s.mapId != *snapshot.mapId)
        {
            s = Seen{};
            s.mapId = *snapshot.mapId;
            s.at = s.sample = o.position;
            s.firstAt = s.sampleAt = s.seenAt = now;
            continue;
        }

        s.at = o.position;
        s.seenAt = now;

        if (now - s.sampleAt >= kSampleEvery)
        {
            const auto seconds = static_cast<float>(toSeconds(now - s.sampleAt));
            const float moved = distance(o.position, s.sample);
            const float speed = moved / seconds;
            s.slowerFor = speed < s.speed - 0.1f ? s.slowerFor + seconds : 0.0f;
            s.speed = speed;

            if (moved > kStill * seconds / static_cast<float>(toSeconds(kStillFor)))
            {
                s.movedAt = now;
            }

            s.sample = o.position;
            s.sampleAt = now;
        }

        // Not moved for a while - since it was first seen, if it never has.
        const bool still = now - (s.movedAt != TimePoint{} ? s.movedAt : s.firstAt) >= kStillFor;

        if (still)
        {
            if (std::none_of(s.docks.begin(), s.docks.end(), [&](const auto& d) { return d.first == s.mapId && distance(d.second, o.position) <= kSameDock; }))
            {
                s.docks.emplace_back(s.mapId, o.position);
            }

            s.left = o.position;
            s.state = ShipState::Parked;
            continue;
        }

        // Under way: in to a dock it has stood at, away from the one it left.
        std::optional<Vec3> nearest;

        for (const auto& [map, dock] : s.docks)
        {
            if (map == s.mapId && (!s.left || distance(dock, *s.left) > kSameDock) && (!nearest || distance(dock, o.position) < distance(*nearest, o.position)))
            {
                nearest = dock;
            }
        }

        if (nearest && distance(*nearest, o.position) <= kEnteringFrom)
        {
            s.state = ShipState::Entering;
        }
        else if (s.left && distance(*s.left, o.position) <= kEnteringFrom * 2)
        {
            s.state = ShipState::Leaving;
        }
        else if (s.slowerFor >= kSlowingFor)
        {
            s.state = ShipState::Entering;
        }
        else
        {
            s.state = ShipState::Moving;
        }
    }
}

std::optional<ShipState> ShipWatch::state(Guid guid) const
{
    const auto it = seen_.find(guid);
    return it == seen_.end() ? std::nullopt : std::optional<ShipState>{it->second.state};
}

std::vector<Vec3> ShipWatch::docks(Guid guid) const
{
    std::vector<Vec3> out;

    if (const auto it = seen_.find(guid); it != seen_.end())
    {
        for (const auto& [map, dock] : it->second.docks)
        {
            if (map == it->second.mapId)
            {
                out.push_back(dock);
            }
        }
    }

    return out;
}

} // namespace og
