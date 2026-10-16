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
 *  File: FollowerWatch.h
 *
 *  Desc:
 *  Declarations and interface for FollowerWatch (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Log.h"
#include "og/core/BotConfig.h"
#include "og/state/GameSnapshot.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace og
{

// Finds a player following us (FollowerConfig), and says when they have gone.
class FollowerWatch
{
public:
    // The follower the bot stands for, or 0.
    Guid update(const GameSnapshot& s, const FollowerConfig& config, TimePoint now, Logger& log);
    Guid follower() const
    {
        return follower_;
    }

    // The follower when found within `window` before `now`: the radar flashes
    // them and the Player Near sound plays for it.
    std::vector<Guid> foundWithin(TimePoint now, Duration window) const;

private:
    void release(Logger& log, const std::string& why);

    // A player within the radius: since when, and where both of us were then.
    struct Track
    {
        TimePoint since{};
        Vec3 theirStart;
        Vec3 ourStart;
    };
    std::unordered_map<Guid, Track> tracks_;
    Guid follower_ = 0;
    std::string followerName_;
    TimePoint followerSeen_{};
    // The last follower found, and when: kept after they are gone, for foundWithin.
    Guid foundGuid_ = 0;
    TimePoint followerFound_{};
};

} // namespace og
