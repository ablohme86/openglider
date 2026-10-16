/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-05
 *  File: InventoryQuestBehavior.cpp
 *
 *  Desc:
 *  Right-clicks/uses quest starter items from bags and accepts them.
 *
 *
 */
#include "og/core/behaviors/Behaviors.h"
#include "og/core/BotContext.h"
#include "og/core/QuestTargets.h"
#include "og/state/IGameStateProvider.h"

#include <algorithm>
#include <format>

namespace og
{

namespace
{

constexpr std::size_t kLogFull = 20;
constexpr int kMaxAttempts = 3;
const Duration kCooldown{300000}; // 5 minutes

} // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> InventoryQuestBehavior::pickCandidate(const GameSnapshot& s, BotContext& ctx) const
{
    if (!ctx.questSource)
    {
        return std::nullopt;
    }

    for (const auto& stack : s.player.bagItems)
    {
        if (stack.itemId == 0)
        {
            continue;
        }

        const auto cooldown = failedUntil_.find(stack.itemId);
        if (cooldown != failedUntil_.end() && ctx.now() < cooldown->second)
        {
            continue;
        }

        if (const auto questId = ctx.questSource->questStartedByItem(s, stack.itemId))
        {
            if (ctx.ignoredQuests.contains(*questId))
            {
                continue;
            }
            return std::make_pair(stack.itemId, *questId);
        }
    }

    return std::nullopt;
}

bool InventoryQuestBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (ctx.partyFollower())
    {
        return false;
    }

    if (!ctx.config.quest.enabled)
    {
        if (stage_ != Stage::Idle)
        {
            stage_ = Stage::Idle;
            ctx.objectives.removeAll("InventoryQuest");
        }
        return false;
    }

    if (s.isDead() || s.inCombat() || !s.attackers().empty() || ctx.midRest(s))
    {
        return false;
    }

    if (stage_ != Stage::Idle)
    {
        return true;
    }

    if (s.player.questLog.size() >= kLogFull)
    {
        return false;
    }

    return pickCandidate(s, ctx).has_value();
}

void InventoryQuestBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    if (stage_ == Stage::Idle)
    {
        if (const auto candidate = pickCandidate(s, ctx))
        {
            currentItemId_ = candidate->first;
            currentQuestId_ = candidate->second;
            stage_ = Stage::OpeningItem;
            stageStarted_ = ctx.now();
            attempts_ = 0;

            const auto* name = ctx.questSource ? ctx.questSource->itemName(currentItemId_) : nullptr;
            itemName_ = (name && !name->empty()) ? *name : std::format("item {}", currentItemId_);

            ctx.movement.stop();
            ctx.navigator.stop();
            ctx.objectives.push(Objective{"InventoryQuest", 0, s.player.position});
        }
    }
}

void InventoryQuestBehavior::onExit(BotContext& ctx)
{
    ctx.objectives.removeAll("InventoryQuest");
    if (stage_ != Stage::Idle)
    {
        stage_ = Stage::Idle;
        currentItemId_ = 0;
        currentQuestId_ = 0;
        attempts_ = 0;
    }
}

BehaviorStatus InventoryQuestBehavior::finish(BotContext& ctx, const std::string& reason, bool failed)
{
    ctx.objectives.removeAll("InventoryQuest");
    if (failed && currentItemId_ != 0)
    {
        failedUntil_[currentItemId_] = ctx.now() + kCooldown;
        if (!reason.empty())
        {
            ctx.log.warn(logcat::Profile, "quest mode: giving up using {} - {} (retrying in 5 minutes)", itemName_, reason);
        }
    }
    else if (!failed && !reason.empty())
    {
        ctx.log.info(logcat::Profile, "quest mode: {}", reason);
    }

    stage_ = Stage::Idle;
    currentItemId_ = 0;
    currentQuestId_ = 0;
    attempts_ = 0;
    return failed ? BehaviorStatus::Failure : BehaviorStatus::Success;
}

void InventoryQuestBehavior::useItem(std::uint32_t itemId, BotContext& ctx)
{
    if (ctx.provider && ctx.provider->supportsLua())
    {
        const std::string lua = std::format(
            "local C=C_Container or {{}};local N,L,U=C.GetContainerNumSlots or GetContainerNumSlots,C.GetContainerItemLink or GetContainerItemLink,C.UseContainerItem or UseContainerItem;"
            "local target={};"
            "for b=0,4 do for s=1,(N(b) or 0) do local l=L(b,s) if l and string.find(l,'item:'..target..'%f[%D]') then U(b,s) return end end end",
            itemId);
        ctx.runScript(lua);
        return;
    }

    if (!itemName_.empty() && !itemName_.starts_with("item "))
    {
        ctx.actions.chat(std::format("/use {}", itemName_));
    }
    else
    {
        ctx.actions.chat(std::format("/use item:{}", itemId));
    }
}

BehaviorStatus InventoryQuestBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (ctx.ignoredQuests.contains(currentQuestId_))
    {
        return finish(ctx, "quest ignored by user", false);
    }
    const TimePoint now = ctx.now();

    if (s.player.isCasting || ctx.actions.isCasting())
    {
        stageStarted_ = now;
        return BehaviorStatus::Running;
    }

    if (currentQuestId_ != 0 && std::find(s.player.questLog.begin(), s.player.questLog.end(), currentQuestId_) != s.player.questLog.end())
    {
        for (const auto* close : {"QuestFrameCloseButton", "QuestFrameDeclineButton"})
        {
            if (ctx.uiFrameVisible(close))
            {
                ctx.clickUiFrame(close);
            }
        }
        return finish(ctx, std::format("accepted quest {} from {}", currentQuestId_, itemName_), false);
    }

    const bool detailVisible = ctx.uiFrameVisible("QuestFrameDetailPanel") || ctx.uiFrameVisible("QuestFrameAcceptButton");

    switch (stage_)
    {
    case Stage::Idle:
    {
        if (const auto candidate = pickCandidate(s, ctx))
        {
            currentItemId_ = candidate->first;
            currentQuestId_ = candidate->second;
            const auto* name = ctx.questSource ? ctx.questSource->itemName(currentItemId_) : nullptr;
            itemName_ = (name && !name->empty()) ? *name : std::format("item {}", currentItemId_);
            stage_ = Stage::OpeningItem;
            stageStarted_ = now;
            attempts_ = 0;
            ctx.movement.stop();
            ctx.navigator.stop();
            ctx.objectives.push(Objective{"InventoryQuest", 0, s.player.position});
            return BehaviorStatus::Running;
        }
        return BehaviorStatus::Success;
    }

    case Stage::OpeningItem:
    {
        if (detailVisible)
        {
            stage_ = Stage::Accepting;
            stageStarted_ = now;
            nextStep_ = now;
            return BehaviorStatus::Running;
        }

        if (attempts_ >= kMaxAttempts)
        {
            return finish(ctx, "detail window never opened", true);
        }

        ctx.movement.stop();
        ctx.navigator.stop();
        ctx.log.info(logcat::Profile, "quest mode: using {} to accept quest", itemName_);
        useItem(currentItemId_, ctx);
        ++attempts_;
        stage_ = Stage::WaitingDetail;
        stageStarted_ = now;
        nextStep_ = now + Duration(1000);
        return BehaviorStatus::Running;
    }

    case Stage::WaitingDetail:
    {
        // Check for any confirmation popup. The deadline runs on: a popup
        // that stays up however often it is clicked ends in the retry below.
        if ((ctx.clickDialog("quest", "Accept") || ctx.clickDialog("quest", "Okay") || ctx.clickDialog("quest", "Yes")) && now - stageStarted_ <= Duration(3000))
        {
            return BehaviorStatus::Running;
        }

        if (detailVisible)
        {
            stage_ = Stage::Accepting;
            stageStarted_ = now;
            nextStep_ = now + Duration(300);
            return BehaviorStatus::Running;
        }

        if (now - stageStarted_ > Duration(3000))
        {
            stage_ = Stage::OpeningItem;
            return BehaviorStatus::Running;
        }

        return BehaviorStatus::Running;
    }

    case Stage::Accepting:
    {
        if (now < nextStep_)
        {
            return BehaviorStatus::Running;
        }

        if (detailVisible)
        {
            const auto title = ctx.uiText("QuestDetailScrollChildFrame", "QuestInfoTitleHeader").value_or(itemName_);
            ctx.log.info(logcat::Profile, "quest mode: clicking Accept for {}", title);

            ctx.clickUiFrame("QuestFrameAcceptButton");
            if (ctx.provider && ctx.provider->supportsLua())
            {
                ctx.provider->executeLua("if QuestFrameAcceptButton and QuestFrameAcceptButton:IsVisible() then QuestFrameAcceptButton:Click() else AcceptQuest() end");
            }

            stage_ = Stage::Confirming;
            stageStarted_ = now;
            nextStep_ = now + Duration(1500);
            return BehaviorStatus::Running;
        }

        stage_ = Stage::Confirming;
        stageStarted_ = now;
        return BehaviorStatus::Running;
    }

    case Stage::Confirming:
    {
        if (currentQuestId_ != 0 && std::find(s.player.questLog.begin(), s.player.questLog.end(), currentQuestId_) != s.player.questLog.end())
        {
            return finish(ctx, std::format("accepted quest {} from {}", currentQuestId_, itemName_), false);
        }

        if (now - stageStarted_ > Duration(3000))
        {
            if (detailVisible)
            {
                stage_ = Stage::Accepting;
                nextStep_ = now;
                return BehaviorStatus::Running;
            }

            return finish(ctx, "quest was not added to quest log", true);
        }

        return BehaviorStatus::Running;
    }
    }

    return BehaviorStatus::Running;
}

} // namespace og
