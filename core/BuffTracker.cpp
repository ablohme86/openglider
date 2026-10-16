/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-30
 *  File: BuffTracker.cpp
 *
 *  Desc:
 *  Implementation of BuffTracker (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/BuffTracker.h"

#include "og/actions/ActionBinding.h"
#include "og/actions/IActionService.h"
#include "og/core/BotContext.h"
#include "og/core/IBotBehavior.h"

#include <algorithm>

namespace og
{

namespace
{
constexpr Duration kBuffLandWait{2500};
constexpr Duration kBlockedBuffHold{30 * 60 * 1000};

std::set<std::uint32_t> buffIds(const GameSnapshot& s)
{
    std::set<std::uint32_t> ids;
    const auto add = [&](const UnitState& u)
    {
        for (const auto& aura : u.auras)
        {
            if (!aura.harmful && aura.spellId != 0)
            {
                ids.insert(aura.spellId);
            }
        }
    };
    add(s.player);

    if (const UnitState* pet = s.pet())
    {
        add(*pet);
    }

    return ids;
}

} // namespace

void BuffTracker::reset()
{
    pressedBuff_.reset();
    blockedBuffs_.clear();
}

void BuffTracker::update(const GameSnapshot& snapshot, BotContext& ctx)
{
    if (snapshot.isDead())
    {
        reset();
        return;
    }

    if (blockedBuffs_.empty() && !pressedBuff_)
    {
        return;
    }

    const auto ids = buffIds(snapshot);

    // One of the buffs up at the refusal faded: the stronger one may be gone.
    std::erase_if(blockedBuffs_,
                  [&](const BlockedBuff& b)
                  {
                      const bool faded = std::any_of(b.blockers.begin(), b.blockers.end(), [&](std::uint32_t id) { return !ids.contains(id); });

                      if (faded)
                      {
                          ctx.actions.clearCooldown(b.action);
                          ctx.log.info(logcat::Rest, "a buff faded - trying {} again", b.action);
                      }

                      return faded;
                  });

    const TimePoint now = ctx.now();

    if (!pressedBuff_ || now < pressedBuff_->settledAt || snapshot.player.isCasting || ctx.actions.isCasting())
    {
        return;
    }

    const PressedBuff pressed = std::move(*pressedBuff_);
    pressedBuff_.reset();
    const bool landed = std::any_of(ids.begin(), ids.end(), [&](std::uint32_t id) { return !pressed.before.contains(id); }) || (!pressed.hadPet && snapshot.pet() != nullptr);

    if (landed || pressed.before.empty())
    {
        return;
    }

    ctx.actions.holdBack(pressed.action, kBlockedBuffHold);
    blockedBuffs_.push_back(BlockedBuff{pressed.action, pressed.before});
    ctx.log.info(logcat::Rest, "{} gave no buff - a stronger one is up, not casting it until a buff fades", pressed.action);
}

bool BuffTracker::tryApplyInstantBuff(const GameSnapshot& snapshot, BotContext& ctx, const IBotBehavior* current)
{
    if (!ctx.combatClass || snapshot.isDead())
    {
        return false;
    }

    if (snapshot.inCombat() || !snapshot.attackers().empty())
    {
        return false;
    }

    if (snapshot.player.isCasting || ctx.actions.isCasting())
    {
        return false;
    }

    if (snapshot.player.isSitting)
    {
        return false;
    }

    if (ctx.actions.onGlobalCooldown())
    {
        return false;
    }

    if (ctx.fishingBusy)
    {
        return false;
    }

    if (ctx.followerHold != 0)
    {
        return false;
    }

    if (ctx.needsRest(snapshot))
    {
        return false;
    }

    // Do not buff while resting, looting, or holding
    if (current)
    {
        const auto name = current->name();

        if (name == "Rest" || name == "Loot" || name == "Hold")
        {
            return false;
        }
    }

    // Do not buff while NPC dialogs or merchant windows are open
    if (ctx.uiFrameVisible("QuestFrame") || ctx.uiFrameVisible("GossipFrame") || ctx.uiFrameVisible("MerchantFrame"))
    {
        return false;
    }

    auto cc = ctx.combatContext(snapshot);

    if (!ctx.combatClass->wantsInstantBuff(cc))
    {
        return false;
    }

    return applyInstantBuff(snapshot, ctx);
}

bool BuffTracker::applyInstantBuff(const GameSnapshot& snapshot, BotContext& ctx)
{
    if (!ctx.combatClass || pressedBuff_)
    {
        return false;
    }

    auto cc = ctx.combatContext(snapshot);
    const TimePoint now = ctx.now();
    auto before = buffIds(snapshot);

    if (ctx.combatClass->onInstantBuff(cc))
    {
        if (const auto used = ctx.actions.lastAction())
        {
            const Duration wait = ctx.actions.castTime(*used) + kBuffLandWait;

            if (ctx.actions.isReadyIgnoringGlobalCooldown(*used))
            {
                ctx.actions.holdBack(*used, wait);
            }

            pressedBuff_ = PressedBuff{*used, now + wait, std::move(before), snapshot.pet() != nullptr};
            return true;
        }
    }

    return false;
}

bool BuffTracker::applyBuff(const GameSnapshot& snapshot, BotContext& ctx)
{
    if (!ctx.combatClass || pressedBuff_)
    {
        return false;
    }

    auto cc = ctx.combatContext(snapshot);
    const TimePoint now = ctx.now();
    auto before = buffIds(snapshot);

    if (ctx.combatClass->onBuff(cc))
    {
        if (const auto used = ctx.actions.lastAction())
        {
            const Duration wait = ctx.actions.castTime(*used) + kBuffLandWait;

            if (ctx.actions.isReadyIgnoringGlobalCooldown(*used))
            {
                ctx.actions.holdBack(*used, wait);
            }

            pressedBuff_ = PressedBuff{*used, now + wait, std::move(before), snapshot.pet() != nullptr};
            return true;
        }
    }

    return false;
}

} // namespace og
