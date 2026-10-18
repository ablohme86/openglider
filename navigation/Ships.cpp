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
#include <cmath>
#include <format>

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

std::string ShipLog::header()
{
    return "time_ms,map,guid,entry,name,x,y,z,state,player_x,player_y,player_z,distance";
}

std::vector<std::string> ShipLog::lines(const GameSnapshot& snapshot, const ShipWatch& watch, std::int64_t wallMs)
{
    // No map known: not a ship's route, nor its dock, to put down.
    if (!snapshot.mapId)
    {
        return {};
    }

    const std::uint32_t mapId = *snapshot.mapId;
    // The ships in sight, each once: the client lists a ship several times,
    // under other GUIDs at the same place.
    std::vector<std::vector<const GameObjectState*>> ships;

    for (const auto& o : snapshot.objects)
    {
        if (o.kind != GameObjectKind::Ship)
        {
            continue;
        }

        const auto same = std::find_if(ships.begin(), ships.end(), [&](const auto& listed) { return listed[0]->name == o.name && distance(listed[0]->position, o.position) <= kSamePlace; });

        if (same == ships.end())
        {
            ships.push_back({&o});
        }
        else
        {
            same->push_back(&o);
        }
    }

    // Each one the ship logged under one of its GUIDs, or else of its name
    // the one last seen nearest - the GUID is another each time a ship comes
    // in sight - or else a ship not logged before.
    std::vector<std::pair<std::size_t, const GameObjectState*>> seen;
    std::vector<bool> taken(logged_.size(), false);
    std::vector<std::size_t> unmatched;

    for (std::size_t i = 0; i < ships.size(); ++i)
    {
        bool found = false;

        for (std::size_t l = 0; l < logged_.size() && !found; ++l)
        {
            for (const GameObjectState* o : ships[i])
            {
                if (!taken[l] && logged_[l].mapId == mapId && o->guid == logged_[l].guid)
                {
                    taken[l] = true;
                    seen.emplace_back(l, o);
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            unmatched.push_back(i);
        }
    }

    for (const std::size_t i : unmatched)
    {
        const auto& listed = ships[i];
        const GameObjectState* lowest = *std::min_element(listed.begin(), listed.end(), [](const GameObjectState* a, const GameObjectState* b) { return a->guid < b->guid; });
        std::optional<std::size_t> nearest;

        for (std::size_t l = 0; l < logged_.size(); ++l)
        {
            if (logged_[l].mapId != mapId || logged_[l].name != lowest->name)
            {
                continue;
            }

            if (!taken[l] && (!nearest || distance(logged_[l].at, lowest->position) < distance(logged_[*nearest].at, lowest->position)))
            {
                nearest = l;
            }
        }

        if (!nearest)
        {
            nearest = logged_.size();
            logged_.push_back({mapId, lowest->name});
            taken.push_back(false);
        }

        taken[*nearest] = true;
        logged_[*nearest].guid = lowest->guid;
        seen.emplace_back(*nearest, lowest);
    }

    // On board the player's position is the ship's, not the world's: near
    // naught, and every ship farther from it than the client sees.
    const auto& p = snapshot.player.position;
    const bool aboard = !seen.empty() && std::abs(p.x) <= kAboardWithin && std::abs(p.y) <= kAboardWithin && std::abs(p.z) <= kAboardWithin &&
                        std::all_of(seen.begin(), seen.end(), [&](const auto& ship) { return distance(p, ship.second->position) > kSight; });
    std::vector<std::string> out;

    for (const auto& [l, o] : seen)
    {
        Logged& ship = logged_[l];
        ship.at = o->position;
        const auto state = watch.state(o->guid).value_or(ShipState::Moving);
        const bool due = !ship.writtenAt || snapshot.timestamp - *ship.writtenAt >= kEvery;

        if (!due && ship.state == state)
        {
            continue;
        }

        ship.writtenAt = snapshot.timestamp;
        ship.state = state;
        // A name with a comma or a quote in it, quoted as CSV quotes.
        std::string name = o->name;

        for (std::size_t q = name.find('"'); q != std::string::npos; q = name.find('"', q + 2))
        {
            name.insert(q, 1, '"');
        }

        const std::string player = aboard ? ",,," : std::format("{:.2f},{:.2f},{:.2f},{:.1f}", p.x, p.y, p.z, distance(p, o->position));
        out.push_back(std::format("{},{},0x{:016X},{},\"{}\",{:.2f},{:.2f},{:.2f},{},{}", wallMs, mapId, o->guid, o->entry, name, o->position.x, o->position.y, o->position.z, toString(state), player));
    }

    return out;
}

} // namespace og
