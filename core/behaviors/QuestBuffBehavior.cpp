/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-18
 *  File: QuestBuffBehavior.cpp
 *
 *  Desc:
 *  Implementation of QuestBuffBehavior: the item a quest gives used for
 *  the buff its objectives need, where the character has it not.
 *
 *
 */
#include "og/core/behaviors/Behaviors.h"
#include "og/core/BotContext.h"
#include "og/core/QuestTargets.h"

#include <algorithm>
#include <array>
#include <format>

namespace og
{

// A quest whose objectives need a buff, and the item that gives it - what
// the quest data does not say. Shhh! We're Hunting Kobolds: the Kobold
// Tracking Kit gives Hunting Kobolds, and the Kobold Tracks are followed
// with it on. The kit is not used up.
struct QuestBuff
{
    std::uint32_t questId;
    std::uint32_t itemId;
    std::uint32_t auraSpellId;
};

namespace
{

constexpr std::array kQuestBuffs{
    QuestBuff{91772, 247970, 1246031},
};

// How long the buff is waited for after the item, and how long an item that
// gave none is left before it is tried again.
constexpr Duration kAuraWait{4000};
constexpr Duration kRetryAfter{60000};

bool hasAura(const PlayerState& p, std::uint32_t spellId)
{
    return std::any_of(p.auras.begin(), p.auras.end(), [&](const Aura& a) { return a.spellId == spellId; });
}

} // namespace

const QuestBuff* QuestBuffBehavior::wanted(const GameSnapshot& s, BotContext& ctx) const
{
    for (const QuestBuff& buff : kQuestBuffs)
    {
        if (std::find(s.player.questLog.begin(), s.player.questLog.end(), buff.questId) == s.player.questLog.end() || hasAura(s.player, buff.auraSpellId))
        {
            continue;
        }

        if (const auto until = retryAt_.find(buff.itemId); until != retryAt_.end() && ctx.now() < until->second)
        {
            continue;
        }

        const auto quest = std::find_if(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const QuestTarget& t) { return t.questId == buff.questId; });

        if (quest == ctx.questTargets.end() || quest->done)
        {
            continue;
        }

        if (std::any_of(s.player.bagItems.begin(), s.player.bagItems.end(), [&](const auto& stack) { return stack.itemId == buff.itemId; }))
        {
            return &buff;
        }
    }

    return nullptr;
}

bool QuestBuffBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.config.quest.enabled || ctx.partyFollower() || s.isDead() || s.inCombat() || !s.attackers().empty() || s.player.isCasting)
    {
        return false;
    }

    return current_ != nullptr || wanted(s, ctx) != nullptr;
}

void QuestBuffBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    current_ = wanted(s, ctx);
    tries_ = 0;
    usedAt_.reset();
}

void QuestBuffBehavior::onExit(BotContext&)
{
    current_ = nullptr;
    usedAt_.reset();
}

BehaviorStatus QuestBuffBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (!current_)
    {
        return BehaviorStatus::Success;
    }

    const TimePoint now = ctx.now();

    if (hasAura(s.player, current_->auraSpellId))
    {
        ctx.log.info(logcat::Profile, "quest mode: the buff is on for {}", questName(ctx, current_->questId));
        current_ = nullptr;
        return BehaviorStatus::Success;
    }

    if (usedAt_ && now - *usedAt_ < kAuraWait)
    {
        return BehaviorStatus::Running;
    }

    // By its id first - the same in every language - then by its name.
    if (tries_ >= 2)
    {
        ctx.log.warn(logcat::Profile, "quest mode: using item {} gave no buff for {} - trying again in a minute", current_->itemId, questName(ctx, current_->questId));
        retryAt_[current_->itemId] = now + kRetryAfter;
        current_ = nullptr;
        return BehaviorStatus::Failure;
    }

    ctx.movement.stop();
    ctx.navigator.stop();
    const auto* name = ctx.questSource ? ctx.questSource->itemName(current_->itemId) : nullptr;
    const std::string line = tries_ == 0 || !name || name->empty() ? std::format("/use item:{}", current_->itemId) : std::format("/use {}", *name);

    if (!ctx.actions.chat(line))
    {
        return BehaviorStatus::Running;
    }

    ++tries_;
    usedAt_ = now;
    ctx.log.info(logcat::Profile, "quest mode: using {} for the buff {} needs", name && !name->empty() ? *name : std::format("item {}", current_->itemId), questName(ctx, current_->questId));
    return BehaviorStatus::Running;
}

std::string QuestBuffBehavior::questName(BotContext& ctx, std::uint32_t questId)
{
    const auto quest = std::find_if(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const QuestTarget& t) { return t.questId == questId; });
    return quest != ctx.questTargets.end() ? quest->questName : std::format("quest {}", questId);
}

} // namespace og
