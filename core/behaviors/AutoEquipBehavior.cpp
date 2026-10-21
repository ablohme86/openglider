/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-16
 *  File: AutoEquipBehavior.cpp
 *
 *  Desc:
 *  Auto Equip New Items: new gear in the bags put on when it is better
 *  for the class than what is worn in its slot.
 *
 */
#include "og/core/BotContext.h"
#include "og/core/QuestRewards.h"
#include "og/core/behaviors/Behaviors.h"
#include <algorithm>
#include <format>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace og
{

namespace
{
// The bags shown after /run ToggleAllBags(), at the latest.
const Duration kBagsOpen{3000};
// A tooltip, and the comparison beside it, a moment after the mouse arrives.
const Duration kTooltip{15};
// No tooltip for a slot by then: an empty one.
const Duration kNoTooltip{15};
// The right-click shown in the bags (or the bind question), at the latest.
const Duration kEquipAnswer{1500};

int countOf(const GameSnapshot& s, std::uint32_t item)
{
    int total = 0;

    for (const ItemStack& stack : s.player.bagItems)
    {
        total += stack.itemId == item ? stack.count : 0;
    }

    return total;
}

bool hasLine(const UiItemTooltip& item, std::string_view text)
{
    return std::any_of(item.lines.begin(), item.lines.end(), [&](const std::string& line) { return line == text; });
}

} // namespace

bool AutoEquipBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // What came into the bags since the last tick: more of an item than there was.
    std::map<std::uint32_t, int> now;

    for (const ItemStack& stack : s.player.bagItems)
    {
        now[stack.itemId] += stack.count;
    }

    if (seenOnce_ && ctx.config.autoEquip)
    {
        for (ItemStack stack : s.player.bagItems)
        {
            // The client's own name for it, or else the data's: 1.60.1 often
            // leaves it out, and Flimsy Chain Pants were never looked at.
            if (stack.name.empty() && ctx.questSource)
            {
                if (const std::string* fromData = ctx.questSource->itemName(stack.itemId))
                {
                    stack.name = *fromData;
                }
            }

            const auto before = seen_.find(stack.itemId);

            if (stack.itemId == 0 || stack.name.empty() || notGear_.contains(stack.name) || takenOff_.contains(stack.name) || (before != seen_.end() && before->second >= now[stack.itemId]))
            {
                continue;
            }

            if (std::none_of(pending_.begin(), pending_.end(), [&](const ItemStack& p) { return p.itemId == stack.itemId; }))
            {
                pending_.push_back(stack);
            }
        }
    }

    seen_ = std::move(now);
    seenOnce_ = true;

    if (!ctx.config.autoEquip)
    {
        pending_.clear();
        return false;
    }

    if (pending_.empty() || !ctx.provider || !ctx.provider->supportsUiBagSlots())
    {
        if (!ctx.provider || !ctx.provider->supportsUiBagSlots())
        {
            pending_.clear();
        }

        return false;
    }

    // Not with a window up the bot is working in, nor in a fight.
    if (s.isDead() || s.inCombat() || !s.attackers().empty() || ctx.vendorTrip)
    {
        return false;
    }

    for (const auto* window : {"MerchantFrame", "GossipFrame", "QuestFrame", "ClassTrainerFrame", "LootFrame"})
    {
        if (ctx.uiFrameVisible(window))
        {
            return false;
        }
    }

    return true;
}

void AutoEquipBehavior::onEnter(const GameSnapshot&, BotContext& ctx)
{
    step_ = Step::OpenBags;
    stepAt_ = ctx.now();
    bagSlots_.clear();
    scannedItems_.clear();
    looked_.clear();
    slot_.reset();
    ctx.navigator.stop();
    ctx.movement.stop();
}

void AutoEquipBehavior::onExit(BotContext& ctx)
{
    // Called away (a fight): the bags shut again, and the item looked for afresh.
    if (bagsOpened_ && ctx.actions.chat("/run CloseAllBags()"))
    {
        bagsOpened_ = false;
    }

    bagSlots_.clear();
    scannedItems_.clear();
    looked_.clear();
    slot_.reset();
}

BehaviorStatus AutoEquipBehavior::next(BotContext& ctx)
{
    if (!pending_.empty())
    {
        pending_.erase(pending_.begin());
    }

    slot_.reset();
    stepAt_ = ctx.now();
    step_ = pending_.empty() ? Step::Close : Step::Find;
    return BehaviorStatus::Running;
}

BehaviorStatus AutoEquipBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();
    const auto moveTo = [&](const UiFrame& frame)
    {
        const auto size = ctx.actions.windowSize();

        if (!frame.visible || !size || !ctx.actions.mouseFree())
        {
            return false;
        }

        ctx.actions.mouseMove(static_cast<int>(frame.centerX() * static_cast<float>(size->first) + 0.5f), static_cast<int>(frame.centerY() * static_cast<float>(size->second) + 0.5f));
        return true;
    };

    switch (step_)
    {
    case Step::OpenBags:
    {
        // The comparison tooltips show only with this; typed once a session.
        if (!compareSet_)
        {
            if (!ctx.actions.chat("/console alwaysCompareItems 1"))
            {
                return BehaviorStatus::Running;
            }

            compareSet_ = true;
        }

        const auto slots = ctx.provider->uiBagSlots();

        if (!slots.empty())
        {
            bagSlots_ = slots;
            step_ = Step::Find;
            stepAt_ = now;
            return BehaviorStatus::Running;
        }

        // A chat command rather than the bags' key: many bind B to something else.
        if (!bagsOpened_)
        {
            if (!ctx.actions.chat("/run ToggleAllBags()"))
            {
                return BehaviorStatus::Running;
            }

            bagsOpened_ = true;
            stepAt_ = now;
            return BehaviorStatus::Running;
        }

        if (now - stepAt_ > kBagsOpen)
        {
            ctx.log.warn(logcat::Loot, "auto equip: the bags did not open - leaving the new items in them");
            pending_.clear();
            bagsOpened_ = false;
            return BehaviorStatus::Failure;
        }

        return BehaviorStatus::Running;
    }

    case Step::Find:
    {
        if (pending_.empty())
        {
            step_ = Step::Close;
            return BehaviorStatus::Running;
        }

        if (countOf(s, pending_.front().itemId) == 0)
        {
            return next(ctx);
        }

        const auto slots = ctx.provider->uiBagSlots();
        const auto sameSlot = [](const UiBagSlot& a, const UiBagSlot& b)
        {
            return a.token == b.token && a.frame.visible == b.frame.visible && a.frame.left == b.frame.left && a.frame.top == b.frame.top && a.frame.right == b.frame.right && a.frame.bottom == b.frame.bottom;
        };
        const bool slotLayoutChanged = slots.size() != bagSlots_.size() || !std::equal(slots.begin(), slots.end(), bagSlots_.begin(), bagSlots_.end(), sameSlot);

        if (slotLayoutChanged)
        {
            bagSlots_ = slots;
            looked_.clear();
            scannedItems_.clear();
            slot_.reset();
        }

        if (bagSlots_.empty())
        {
            step_ = Step::OpenBags;
            stepAt_ = now;
            return BehaviorStatus::Running;
        }

        if (const auto found = scannedItems_.find(pending_.front().name); found != scannedItems_.end())
        {
            if (!moveTo(found->second.frame))
            {
                return BehaviorStatus::Running;
            }

            slot_ = found->second;
            step_ = Step::Read;
            stepAt_ = now;
            return BehaviorStatus::Running;
        }

        const auto slot = std::find_if(bagSlots_.begin(), bagSlots_.end(), [&](const UiBagSlot& candidate) { return !looked_.contains(candidate.token); });

        // Gone from the bags (sold, used, put on by the player): nothing to do.
        if (slot == bagSlots_.end() || countOf(s, pending_.front().itemId) == 0)
        {
            return next(ctx);
        }

        if (!moveTo(slot->frame))
        {
            return BehaviorStatus::Running;
        }

        looked_.insert(slot->token);
        slot_ = *slot;
        step_ = Step::Read;
        stepAt_ = now;
        return BehaviorStatus::Running;
    }

    case Step::Read:
    {
        if (now - stepAt_ < kTooltip)
        {
            return BehaviorStatus::Running;
        }

        const auto item = ctx.provider->uiItemTooltip();

        if (!item || !slot_ || item->owner != slot_->token)
        {
            if (now - stepAt_ > kNoTooltip)
            {
                const auto cached = scannedItems_.find(pending_.front().name);

                if (slot_ && cached != scannedItems_.end() && cached->second.token == slot_->token)
                {
                    scannedItems_.erase(cached);
                }

                step_ = Step::Find;
            }

            return BehaviorStatus::Running;
        }

        if (!item->name.empty())
        {
            scannedItems_[item->name] = *slot_;
        }

        if (item->name != pending_.front().name)
        {
            const auto cached = scannedItems_.find(pending_.front().name);

            if (cached != scannedItems_.end() && cached->second.token == slot_->token)
            {
                scannedItems_.erase(cached);
            }

            step_ = Step::Find;
            return BehaviorStatus::Running;
        }

        if (!isGear(*item))
        {
            notGear_.insert(item->name);
            return next(ctx);
        }

        const int qualityFlag = autoEquipQualityFlag(item->quality);

        if (qualityFlag == 0 || (ctx.config.autoEquipQualities & qualityFlag) == 0)
        {
            ctx.log.info(logcat::Loot, "auto equip: {} quality {} is not selected - left in the bags", item->name, item->quality);
            return next(ctx);
        }

        if (item->hasRedLine())
        {
            ctx.log.info(logcat::Loot, "auto equip: {} is not for this character", item->name);
            return next(ctx);
        }

        if (ctx.config.autoEquipSkipBoE && bindsWhenEquipped(*item))
        {
            ctx.log.info(logcat::Loot, "auto equip: {} binds when equipped - left in the bags", item->name);
            return next(ctx);
        }

        equippingBoE_ = bindsWhenEquipped(*item);

        // What is worn there: none shown, an empty slot. Two (rings, trinkets,
        // a second weapon): the worse of them is the one it would take over.
        const auto worn = ctx.provider->uiComparisonTooltips();
        const PlayerClass c = s.player.playerClass;
        const auto mine = gearRank(*item, c, s.player.level);

        // A shield (or anything for the off hand) beside a two-hander takes
        // the weapon off: a paladin's two-handed mace went for a shield.
        const bool offHand = hasLine(*item, "Off Hand") || hasLine(*item, "Held In Off-hand") || hasLine(*item, "Shield");

        if (offHand && std::any_of(worn.begin(), worn.end(), [](const UiItemTooltip& w) { return hasLine(w, "Two-Hand"); }))
        {
            ctx.log.info(logcat::Loot, "auto equip: {} would take the two-handed weapon off - left in the bags", item->name);
            return next(ctx);
        }

        // Two of them shown: the worse is the one it takes over - but a
        // two-hander takes both hands, so it has to beat the two together.
        std::vector<const UiItemTooltip*> off;

        if (hasLine(*item, "Two-Hand"))
        {
            for (const UiItemTooltip& w : worn)
            {
                off.push_back(&w);
            }
        }
        else if (const auto worst = std::min_element(worn.begin(), worn.end(), [&](const UiItemTooltip& a, const UiItemTooltip& b) { return gearRank(a, c, s.player.level) < gearRank(b, c, s.player.level); }); worst != worn.end())
        {
            off.push_back(&*worst);
        }

        // What comes off, together: fit only if all of it fits; stats,
        // armour or damage, and price added up.
        std::tuple<int, int, int, int> theirs{1, 0, 0, 0};
        std::string names;

        for (const UiItemTooltip* w : off)
        {
            const auto [fit, stats, protection, price] = gearRank(*w, c, s.player.level);
            theirs = {std::min(std::get<0>(theirs), fit), std::get<1>(theirs) + stats, std::get<2>(theirs) + protection, std::get<3>(theirs) + price};
            names += (names.empty() ? "" : " and ") + w->name;
        }

        if (!off.empty() && !(mine > theirs))
        {
            ctx.log.info(logcat::Loot, "auto equip: {} is no better than {} - left in the bags", item->name, names);
            return next(ctx);
        }

        ctx.log.info(logcat::Loot, "auto equip: putting on {}{}", item->name, off.empty() ? std::string(" (nothing worn there)") : " instead of " + names);

        // Kept for when it has gone on (Step::Confirm).
        takingOff_.clear();

        for (const UiItemTooltip* w : off)
        {
            takingOff_.push_back(w->name);
        }
        countBefore_ = countOf(s, pending_.front().itemId);
        step_ = Step::Equip;
        stepAt_ = now;
        return BehaviorStatus::Running;
    }

    case Step::Equip:
    {
        // The mouse still on the item: the right-click puts it on.
        const auto item = ctx.provider->uiItemTooltip();

        if (!item || !slot_ || item->owner != slot_->token || item->name != pending_.front().name)
        {
            if (slot_)
            {
                const auto cached = scannedItems_.find(pending_.front().name);

                if (cached != scannedItems_.end() && cached->second.token == slot_->token)
                {
                    scannedItems_.erase(cached);
                }
            }

            step_ = Step::Find;
            return BehaviorStatus::Running;
        }

        if (!ctx.actions.mouseFree())
        {
            return BehaviorStatus::Running;
        }

        ctx.actions.click(MouseButton::Right);
        step_ = Step::Confirm;
        stepAt_ = now;
        return BehaviorStatus::Running;
    }

    case Step::Confirm:
    {
        // Put on: one fewer of it in the bags (what was worn takes its place).
        if (countOf(s, pending_.front().itemId) < countBefore_)
        {
            // What came off is back in the bags in the same moment: no new item.
            for (const std::string& off : std::exchange(takingOff_, {}))
            {
                std::erase_if(pending_, [&](const ItemStack& p) { return p.name == off; });
                takenOff_.insert(off);
            }

            ctx.log.info(logcat::Loot, "auto equip: {} put on", pending_.front().name);
            return next(ctx);
        }

        // Bind on Equip, and allowed: the client asks first. It can use any
        // of the four StaticPopup frames; accept only a bind question here.
        if (equippingBoE_ && ctx.clickDialog("bind"))
        {
            return BehaviorStatus::Running;
        }

        if (now - stepAt_ > kEquipAnswer)
        {
            ctx.log.warn(logcat::Loot, "auto equip: {} did not go on", pending_.front().name);
            return next(ctx);
        }

        return BehaviorStatus::Running;
    }

    case Step::Close:
        if (bagsOpened_ && !ctx.actions.chat("/run CloseAllBags()"))
        {
            return BehaviorStatus::Running;
        }

        bagsOpened_ = false;
        return BehaviorStatus::Success;
    }

    return BehaviorStatus::Running;
}

} // namespace og
