/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: BotStats.h
 *
 *  Desc:
 *  Declarations and interface for BotStats (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include <cstdint>

namespace og
{

struct BotStats
{
    TimePoint started{};
    int kills = 0;
    int deaths = 0;
    int pulls = 0;
    int loots = 0;
    // Observed increases in carried money, in copper. Spending is not deducted.
    std::int64_t moneyGathered = 0;
    int skins = 0;
    int herbsGathered = 0;
    int mineralsGathered = 0;
    int gatheringFailures = 0;
    int fishCasts = 0;
    int fishCaught = 0;   // the catch reached the bags
    int fishMissed = 0;   // hooked, but nothing arrived
    int fishNoBite = 0;   // the bobber went (or the fish got away) before a bite
    int fishNoBobber = 0; // the cast never put a bobber in the water
    int rests = 0;
    int waypointsReached = 0;
    int stuckEvents = 0;
    int blacklisted = 0;
    int actionsExecuted = 0;

    double perHour(int count, TimePoint now) const
    {
        const double hours = toSeconds(now - started) / 3600.0;
        return hours > 0.0 ? count / hours : 0.0;
    }
};

} // namespace og
