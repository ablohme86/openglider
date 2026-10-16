/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-15
 *  File: PlayerProximityAlert.cpp
 *
 *  Desc:
 *  Implementation of PlayerProximityAlert (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/PlayerProximityAlert.h"

#include <format>

namespace og
{

void PlayerProximityAlert::update(const GameSnapshot& s, const PlayerAlertConfig& config, TimePoint now, Logger& log)
{
    if (!config.enabled)
    {
        players_.clear();
        return;
    }

    for (const auto& u : s.units)
    {
        if (!u.isPlayer || u.guid == s.player.guid)
        {
            continue;
        }

        const float dist = distance(s.player.position, u.position);
        Seen& seen = players_[u.guid];
        seen.lastSeen = now;

        if (dist > config.radius)
        {
            continue;
        }

        // Once as they come in, and again every renotifyAfter while they are
        // still here; stepping in and out on the edge in between is not news.
        if (!seen.warned || now - seen.lastWarned >= config.renotifyAfter)
        {
            const std::string who = u.playerClass == PlayerClass::Unknown ? std::format("level {}", u.level) : std::format("level {} {}", u.level, toString(u.playerClass));

            if (config.logWarnings)
            {
                log.warn(logcat::Target, "Player {} ({}) is near you, distance: {:.0f} yards", u.name.empty() ? std::string("(unknown)") : u.name, who, dist);
            }

            seen.warned = true;
            seen.lastWarned = now;
        }
    }

    // A player out of sight for longer than the renotify window would be
    // announced again anyway.
    std::erase_if(players_, [&](const auto& entry) { return now - entry.second.lastSeen > config.renotifyAfter; });
}

std::vector<Guid> PlayerProximityAlert::warnedWithin(TimePoint now, Duration window) const
{
    std::vector<Guid> out;

    for (const auto& [guid, seen] : players_)
    {
        if (seen.warned && now - seen.lastWarned < window)
        {
            out.push_back(guid);
        }
    }

    return out;
}

} // namespace og
