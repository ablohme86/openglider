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
 *  File: CombatTracker.cpp
 *
 *  Desc:
 *  Implementation of CombatTracker (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/CombatTracker.h"

#include "og/core/Events.h"

#include <algorithm>

namespace og
{

void CombatTracker::update(const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();
    const auto attackers = s.attackers();

    // Fight start/end.
    const bool fighting = s.inCombat() || !attackers.empty();

    if (fighting && !ctx.combat.inFight)
    {
        ctx.combat.inFight = true;
        ctx.combat.fightStarted = now;
    }
    else if (!fighting && ctx.combat.inFight)
    {
        ctx.combat.inFight = false;
    }

    // Auto-attack on (weapons out) at a target that is not fighting us - one
    // the character had when attached, or one the client took after a kill:
    // the client says in combat for as long as it stays on, and the bot waited
    // for the fight to end, before looting or anything else, for ever. Its
    // target cleared (Escape - only with a target, or it opens the game menu)
    // stops it.
    constexpr Duration kStrayAttackWait{2000};
    constexpr Duration kStrayClearEvery{3000};
    const UnitState* target = s.target();
    const bool stray = s.inCombat() && attackers.empty() && target && target->guid != ctx.combat.engaged && (!target->isAlive() || target->targetGuid != s.player.guid);

    if (!stray)
    {
        strayAttackSince_.reset();
    }
    else if (!strayAttackSince_)
    {
        strayAttackSince_ = now;
    }
    else if (now - *strayAttackSince_ >= kStrayAttackWait && now - strayClearedAt_ >= kStrayClearEvery)
    {
        strayClearedAt_ = now;
        ctx.log.info(logcat::Combat, "in combat with nothing fighting us, auto-attacking {} - clearing the target to stop it", target->name);
        ctx.actions.execute(actions::TargetClear);
    }

    // Remember everything we are fighting.
    for (const UnitState* u : attackers)
    {
        fought_.insert(u->guid);
    }

    if (ctx.combat.engaged)
    {
        fought_.insert(ctx.combat.engaged);
    }

    // Detect kills.
    for (auto it = fought_.begin(); it != fought_.end();)
    {
        const UnitState* u = s.findUnit(*it);

        if (!u)
        {
            if (ctx.combat.engaged == *it)
            {
                ctx.combat.engaged = 0;
            }

            it = fought_.erase(it);
            continue;
        }

        if (!u->isAlive())
        {
            if (ctx.config.quest.enabled && std::any_of(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const QuestTarget& q) { return !q.done && std::find(q.mobs.begin(), q.mobs.end(), u->entry) != q.mobs.end(); }))
            {
                ctx.questPullAfter = now + Duration(1000);
            }

            ++ctx.stats.kills;
            ctx.log.info(logcat::Combat, "{} {} killed", u->name, formatGuid(u->guid));
            ctx.recentKills.push_back({u->guid, u->entry, u->name, u->position, now});
            ctx.events.publish(TargetKilledEvent{u->guid, u->name, u->position});

            if (ctx.lootEnabled() || ctx.skinEnabled())
            {
                const bool known = std::any_of(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return p.guid == u->guid; });

                if (!known)
                {
                    ctx.pendingLoot.push_back({u->guid, u->position, now, false, now});
                }
            }

            if (ctx.combat.engaged == u->guid)
            {
                ctx.combat.engaged = 0;
            }

            it = fought_.erase(it);
            continue;
        }

        // Evaded / reset units are no longer ours.
        if (!u->inCombat && u->guid != ctx.combat.engaged)
        {
            it = fought_.erase(it);
            continue;
        }

        ++it;
    }

    // Engaged target sanity. A blacklisted mob that attacks us is still
    // fought: dropping it here only had combat pick it again on the next tick.
    if (ctx.combat.engaged)
    {
        const UnitState* u = s.findUnit(ctx.combat.engaged);

        if (!u || (ctx.targetBlacklist.contains(ctx.combat.engaged) && !s.isAttacker(*u)))
        {
            ctx.combat.engaged = 0;
        }
    }

    // A corpse may be flagged lootable only when the fight is over.
    if (fighting)
    {
        for (auto& p : ctx.pendingLoot)
        {
            p.graceFrom = now;
        }
    }

    // Prune pending loot.
    std::erase_if(ctx.pendingLoot,
                  [&](const PendingLoot& p)
                  {
                      if (ctx.objectBlacklist.contains(p.guid))
                      {
                          return true;
                      }

                      const UnitState* u = s.findUnit(p.guid);

                      if (!u || (u->isTappedByOther && !p.skinOnly))
                      {
                          return true;
                      }

                      if (now - p.addedAt > Duration(180000))
                      {
                          return true;
                      }

                      if (p.inProgress)
                      {
                          return false;
                      }

                      if (p.skinOnly)
                      {
                          return !(u->isSkinnable && ctx.canSkin(*u, s.player));
                      }

                      const bool interesting = u->isLootable || (u->isSkinnable && ctx.canSkin(*u, s.player)) || ctx.config.lootWithoutFlag;
                      return !interesting && now - p.graceFrom > ctx.config.lootGrace;
                  });
}

} // namespace og
