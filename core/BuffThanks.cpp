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
 *  File: BuffThanks.cpp
 *
 *  Desc:
 *  Implementation of BuffThanks (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/BuffThanks.h"

namespace og
{

void BuffThanks::update(const GameSnapshot& s, const BuffThanksConfig& config, TimePoint now, const std::function<Duration()>& delay, const std::function<bool(const std::string&)>& chat, Logger& log)
{
    if (!config.enabled || config.command.empty())
    {
        seen_.clear();
        primed_ = false;
        pending_.reset();
        return;
    }

    std::set<std::pair<std::uint32_t, Guid>> current;

    for (const Aura& a : s.player.auras)
    {
        if (!a.harmful && a.caster != 0 && a.caster != s.player.guid)
        {
            current.emplace(a.spellId, a.caster);
        }
    }

    const bool primed = std::exchange(primed_, true);

    for (const auto& [spell, caster] : current)
    {
        if (!primed || seen_.contains({spell, caster}) || pending_)
        {
            continue;
        }

        const UnitState* who = s.findUnit(caster);

        if (!who || !who->isPlayer || who->reaction == Reaction::Hostile)
        {
            continue;
        }

        if (const auto it = answered_.find(caster); it != answered_.end() && now - it->second < config.perPlayer)
        {
            continue;
        }

        pending_ = {caster, now + delay()};
        pendingName_ = who->name;
    }

    seen_ = std::move(current);

    // Typed a moment later, and not while fighting or casting.
    if (!pending_ || now < pending_->second || s.inCombat() || s.player.isCasting)
    {
        return;
    }

    std::string line = config.command;

    if (const auto at = line.find("{name}"); at != std::string::npos)
    {
        if (pendingName_.empty())
        {
            pending_.reset();
            return;
        }

        line.replace(at, 6, pendingName_);
    }

    const Guid caster = pending_->first;
    pending_.reset();
    answered_[caster] = now;

    if (chat(line))
    {
        log.info(logcat::Action, "{} buffed you - {}", pendingName_.empty() ? std::string("A player") : pendingName_, line);
    }
}

} // namespace og
