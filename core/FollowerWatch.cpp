/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-29
 *  File: FollowerWatch.cpp
 *
 *  Desc:
 *  Implementation of FollowerWatch (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/FollowerWatch.h"

#include <format>

namespace og
{

void FollowerWatch::release(Logger& log, const std::string& why)
{
    log.info(logcat::Target, "{} {} - carrying on", followerName_, why);
    follower_ = 0;
    followerName_.clear();
    tracks_.clear();
}

Guid FollowerWatch::update(const GameSnapshot& s, const FollowerConfig& config, TimePoint now, Logger& log)
{
    if (!config.enabled)
    {
        tracks_.clear();

        if (follower_)
        {
            release(log, "is no longer watched for");
        }

        return follower_;
    }

    if (follower_)
    {
        if (const UnitState* u = s.findUnit(follower_))
        {
            followerSeen_ = now;
            const float dist = distance2D(s.player.position, u->position);

            if (dist >= config.clearDistance)
            {
                release(log, std::format("is {:.0f} yd away", dist));
            }
        }
        else if (now - followerSeen_ >= config.lostTimeout)
        {
            release(log, "is out of sight");
        }

        return follower_;
    }

    std::erase_if(tracks_, [&](const auto& entry) { return !s.findUnit(entry.first); });

    for (const auto& u : s.units)
    {
        if (!u.isPlayer || u.guid == s.player.guid || !u.isAlive())
        {
            continue;
        }

        if (distance2D(s.player.position, u.position) > config.radius)
        {
            tracks_.erase(u.guid);
            continue;
        }

        const Track& track = tracks_.try_emplace(u.guid, Track{now, u.position, s.player.position}).first->second;

        // Long enough, and both on the move: standing by or passing is not following.
        if (now - track.since < config.followTime || distance2D(track.theirStart, u.position) < config.minTravel || distance2D(track.ourStart, s.player.position) < config.minTravel)
        {
            continue;
        }

        follower_ = u.guid;
        followerName_ = u.name.empty() ? std::string("A player") : "Player " + u.name;
        followerSeen_ = now;
        followerFound_ = now;
        foundGuid_ = u.guid;
        log.warn(logcat::Target,
                 "{} has followed you for {:.0f} s - stopping until they are {:.0f} yd away "
                 "(fighting back if attacked)",
                 followerName_, toSeconds(now - track.since), config.clearDistance);
        break;
    }

    return follower_;
}

std::vector<Guid> FollowerWatch::foundWithin(TimePoint now, Duration window) const
{
    if (foundGuid_ == 0 || now - followerFound_ >= window)
    {
        return {};
    }

    return {foundGuid_};
}

} // namespace og
