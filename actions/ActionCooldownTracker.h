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
 *  File: ActionCooldownTracker.h
 *
 *  Desc:
 *  Declarations and interface for ActionCooldownTracker (semantic game
 *  actions, action configuration, and execution).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace og
{

// Tracks per-action cooldowns and the global cooldown as seen by the bot
// (what we pressed and when), independent of what the game reports.
class ActionCooldownTracker
{
public:
    void record(std::string_view action, TimePoint now, Duration cooldown, Duration gcd);

    bool isReady(std::string_view action, TimePoint now, bool ignoresGcd = false) const;
    Duration remaining(std::string_view action, TimePoint now) const;
    Duration gcdRemaining(TimePoint now) const;
    std::optional<TimePoint> lastUsed(std::string_view action) const;

    // Forgets one action's cooldown; the next isReady() for it is true again.
    void clear(std::string_view action);
    void clearGcd()
    {
        gcdReadyAt_ = TimePoint{};
    }

    void startGcd(TimePoint now, Duration gcd)
    {
        gcdReadyAt_ = std::max(gcdReadyAt_, now + gcd);
    }

    void reset();

private:
    struct Entry
    {
        TimePoint lastUsed{};
        TimePoint readyAt{};
    };
    std::map<std::string, Entry, std::less<>> entries_;
    TimePoint gcdReadyAt_{};
};

} // namespace og
