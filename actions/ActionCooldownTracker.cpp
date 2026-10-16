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
 *  File: ActionCooldownTracker.cpp
 *
 *  Desc:
 *  Implementation of ActionCooldownTracker (semantic game actions,
 *  action configuration, and execution).
 *
 *
 */
#include "og/actions/ActionCooldownTracker.h"

#include <algorithm>

namespace og
{

void ActionCooldownTracker::record(std::string_view action, TimePoint now, Duration cooldown, Duration gcd)
{
    auto it = entries_.find(action);

    if (it == entries_.end())
    {
        it = entries_.emplace(std::string(action), Entry{}).first;
    }

    it->second.lastUsed = now;
    it->second.readyAt = now + cooldown;

    if (gcd.count() > 0)
    {
        gcdReadyAt_ = std::max(gcdReadyAt_, now + gcd);
    }
}

bool ActionCooldownTracker::isReady(std::string_view action, TimePoint now, bool ignoresGcd) const
{
    if (!ignoresGcd && now < gcdReadyAt_)
    {
        return false;
    }

    auto it = entries_.find(action);
    return it == entries_.end() || now >= it->second.readyAt;
}

Duration ActionCooldownTracker::remaining(std::string_view action, TimePoint now) const
{
    auto it = entries_.find(action);

    if (it == entries_.end() || now >= it->second.readyAt)
    {
        return Duration(0);
    }

    return it->second.readyAt - now;
}

Duration ActionCooldownTracker::gcdRemaining(TimePoint now) const
{
    return now >= gcdReadyAt_ ? Duration(0) : gcdReadyAt_ - now;
}

std::optional<TimePoint> ActionCooldownTracker::lastUsed(std::string_view action) const
{
    auto it = entries_.find(action);

    if (it == entries_.end())
    {
        return std::nullopt;
    }

    return it->second.lastUsed;
}

void ActionCooldownTracker::clear(std::string_view action)
{
    if (auto it = entries_.find(action); it != entries_.end())
    {
        entries_.erase(it);
    }
}

void ActionCooldownTracker::reset()
{
    entries_.clear();
    gcdReadyAt_ = TimePoint{};
}

} // namespace og
