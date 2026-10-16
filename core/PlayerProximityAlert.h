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
 *  File: PlayerProximityAlert.h
 *
 *  Desc:
 *  Declarations and interface for PlayerProximityAlert (core bot
 *  engine, behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Log.h"
#include "og/core/BotConfig.h"
#include "og/state/GameSnapshot.h"

#include <unordered_map>
#include <vector>

namespace og
{

// Warns when another player is within PlayerAlertConfig::radius: once as they
// come in, and again every renotifyAfter for as long as they are still within
// it. Stepping in and out on the edge in between is not news.
class PlayerProximityAlert
{
public:
    void update(const GameSnapshot& s, const PlayerAlertConfig& config, TimePoint now, Logger& log);
    // The players warned about within `window` before `now`.
    std::vector<Guid> warnedWithin(TimePoint now, Duration window) const;

private:
    struct Seen
    {
        bool warned = false;
        TimePoint lastWarned{};
        TimePoint lastSeen{};
    };
    std::unordered_map<Guid, Seen> players_;
};

} // namespace og
