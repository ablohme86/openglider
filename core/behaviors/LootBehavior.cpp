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
 *  File: LootBehavior.cpp
 *
 *  Desc:
 *  Implementation of LootBehavior (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/actions/ActionBinding.h"
#include "og/core/Events.h"
#include "og/core/behaviors/Behaviors.h"
#include "og/state/IGameStateProvider.h"

#include <algorithm>
#include <limits>

namespace og
{

namespace
{

int carriedItems(const PlayerState& p)
{
    int total = 0;

    for (const auto& item : p.bagItems)
    {
        total += item.count;
    }

    return total;
}

bool receivedItems(const PlayerState& player, const std::vector<ItemStack>& before)
{
    // Compare each item separately: consuming something or a missing item in
    // a memory snapshot can hide newly looted items in a single grand total.
    using ItemKey = std::pair<std::uint32_t, std::string>;
    const auto counts = [](const std::vector<ItemStack>& items)
    {
        std::map<ItemKey, int> result;

        for (const auto& item : items)
        {
            if (item.itemId == 0 && item.name.empty())
            {
                continue;
            }

            result[{item.itemId, item.itemId ? std::string{} : item.name}] += item.count;
        }

        return result;
    };
    const auto previous = counts(before);

    for (const auto& [item, count] : counts(player.bagItems))
    {
        const auto it = previous.find(item);

        if (count > (it == previous.end() ? 0 : it->second))
        {
            return true;
        }
    }

    return false;
}

void emptyLootWindow(BotContext& ctx)
{
    if (ctx.provider && ctx.provider->supportsLua() && ctx.provider->luaIdle())
    {
        ctx.provider->executeLua("if GetNumLootItems()>0 then for i=GetNumLootItems(),1,-1 do LootSlot(i) end end");
    }
}

bool worthLooting(const PendingLoot& p, const GameSnapshot& s, const BotContext& ctx)
{
    const UnitState* u = s.findUnit(p.guid);

    // A quest's mob back at once by its corpse guards it for ever: what the
    // quest needs is taken all the same (as in tick).
    if (!u || (ctx.corpseGuarded(s, u->position) && !ctx.questNeeds(u->entry)))
    {
        return false;
    }

    if (!s.attackers().empty() && !ctx.questNeeds(u->entry))
    {
        return false;
    }

    // Another player's kill: only its skin, once the flag says it may be taken.
    if (p.skinOnly)
    {
        return u->isSkinnable && ctx.canSkin(*u, s.player);
    }

    if (u->isTappedByOther)
    {
        return false;
    }

    if (u->isLootable || (u->isSkinnable && ctx.canSkin(*u, s.player)))
    {
        return true;
    }

    if (ctx.config.lootWithoutFlag)
    {
        return true; // bounded retries when the flag is unavailable
    }

    return ctx.now() - p.graceFrom < ctx.config.lootGrace;
}

} // namespace

bool LootBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (!(ctx.lootEnabled() || ctx.skinEnabled()) || s.isDead())
    {
        return false;
    }

    if (!s.attackers().empty() && !ctx.hasQuestLootNearby(s))
    {
        return false;
    }

    // A herb or vein already gone for is finished first. A corpse left for the
    // hostile next to it came up for looting each time that one wandered off:
    // the bot turned back and forth between the two until it pulled the guard.
    if (current_ == 0 && ctx.objectives.contains("GatherNode"))
    {
        return false;
    }

    if (current_ != 0 || !ctx.pendingLoot.empty())
    {
        discoverNearby(s, ctx);
    }
    else if ((ctx.navigating || ctx.questTravel) && !s.player.isCasting && !ctx.actions.isCasting() && s.player.freeBagSlots != 0 && !ctx.needsRest(s) && (!lastNavigationScan_ || ctx.now() - *lastNavigationScan_ >= Duration(1000)))
    {
        // Passing a corpse can start a loot detour too. Poll once a second
        // while following the profile or walking to a quest NPC, without
        // interrupting another harvest.
        lastNavigationScan_ = ctx.now();
        discoverNearby(s, ctx);
    }

    // Finish the active corpse even if this snapshot pruned it from the queue.
    // Its loot can arrive in exactly the same update that despawns it.
    return current_ != 0 || std::any_of(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return p.inProgress || worthLooting(p, s, ctx); });
}

void LootBehavior::discoverNearby(const GameSnapshot& s, BotContext& ctx)
{
    // No detours on a vendor trip; on the way to a quest NPC there are.
    if (s.inCombat() || (ctx.combat.defendOnly && !ctx.questTravel))
    {
        return;
    }

    std::erase_if(completed_, [&](const auto& entry) { return !s.findUnit(entry.first); });

    for (const auto& unit : s.units)
    {
        if (unit.guid == 0 || unit.isAlive() || unit.isPlayer || unit.isCritter || ctx.objectBlacklist.contains(unit.guid) || distance(s.player.position, unit.position) > ctx.config.lootSearchRadius)
        {
            continue;
        }

        const auto it = completed_.find(unit.guid);
        const CorpseProgress done = it == completed_.end() ? CorpseProgress{} : it->second;
        const bool loot = ctx.lootEnabled() && unit.isLootable && !unit.isTappedByOther && !done.lootHandled;
        const bool skin = unit.isSkinnable && !done.skinHandled && ctx.canSkin(unit, s.player);

        if (!loot && !skin)
        {
            continue; // never probe someone else's unflagged corpse
        }

        if (std::any_of(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return p.guid == unit.guid; }))
        {
            continue;
        }

        ctx.pendingLoot.push_back({unit.guid, unit.position, ctx.now(), false, ctx.now(), unit.isTappedByOther});
        ctx.log.debug(logcat::Loot, "nearby corpse {} {} added to loot queue{}", unit.name, formatGuid(unit.guid), unit.isTappedByOther ? " (another player's kill: skin only)" : "");
    }
}

void LootBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    current_ = 0;

    if (const UnitState* t = s.target(); t && t->isAlive() && !s.isAttacker(*t))
    {
        ctx.actions.execute(actions::TargetClear);
    }
}

void LootBehavior::onExit(BotContext& ctx)
{
    for (auto& p : ctx.pendingLoot)
    {
        p.inProgress = false;
    }

    ctx.objectives.removeAll("LootCorpse");
    ctx.navigator.stop();
    ctx.cancelInteraction();
    ctx.restoreCamera();
    current_ = 0;
}

void LootBehavior::selectNext(const GameSnapshot& s, BotContext& ctx)
{
    float best = std::numeric_limits<float>::max();
    PendingLoot* chosen = nullptr;

    for (auto& p : ctx.pendingLoot)
    {
        if (!worthLooting(p, s, ctx))
        {
            continue;
        }

        const UnitState* u = s.findUnit(p.guid);
        const bool isQuestCorpse = u && ctx.questNeeds(u->entry);
        const float d = distance(s.player.position, p.position) - (isQuestCorpse ? 1000.0f : 0.0f);

        if (d < best)
        {
            best = d;
            chosen = &p;
        }
    }

    if (!chosen)
    {
        current_ = 0;
        return;
    }

    current_ = chosen->guid;
    skinOnly_ = chosen->skinOnly;
    chosen->inProgress = true;
    moveStarted_ = ctx.now();
    lastInteract_ = TimePoint{};
    lootReadySince_.reset();
    targetTypedAt_.reset();
    lootAttempts_ = 0;
    skinAttempts_ = 0;
    skinItemsBefore_ = 0;
    skinLootReceived_ = false;
    skinCasting_ = false;
    skinCastSeen_ = false;
    lastCarried_ = -1;
    bagsChangedAt_.reset();
    skinRetryMoving_ = false;
    skinFinishedAt_.reset();
    lootDoneAt_.reset();
    moneyBefore_ = -1;
    itemsBefore_.clear();
    freeSlotsBefore_ = -1;
    const auto previous = completed_.find(current_);
    lootReceived_ = previous != completed_.end() && previous->second.lootHandled;
    flagSeen_ = false;
    ctx.objectives.push(Objective{"LootCorpse", current_, chosen->position});
}

void LootBehavior::finish(const UnitState* unit, BotContext& ctx)
{
    // An unflagged corpse is looted only if loot arrived.
    const bool looted = lootAttempts_ > 0 && (lootReceived_ || (flagSeen_ && (!unit || !unit->isLootable)));
    const bool skinned = skinAttempts_ > 0 && skinLootReceived_;

    if (looted)
    {
        ++ctx.stats.loots;
    }

    if (skinned)
    {
        ++ctx.stats.skins;
    }

    ctx.log.info(logcat::Loot, "corpse {} done (looted={} skinned={})", formatGuid(current_), looted, skinned);
    ctx.events.publish(LootCompletedEvent{current_, looted, skinned});
    auto& done = completed_[current_];
    done.lootHandled = done.lootHandled || lootAttempts_ > 0 || skinAttempts_ > 0;
    done.skinHandled = done.skinHandled || skinAttempts_ > 0;
    std::erase_if(ctx.pendingLoot, [&](const PendingLoot& p) { return p.guid == current_; });
    ctx.objectives.pop("LootCorpse");
    ctx.cancelInteraction();
    ctx.restoreCamera();

    if (looted || skinned)
    {
        ctx.setTarget(0);
    }

    current_ = 0;
}

std::optional<bool> LootBehavior::targetCorpse(const GameSnapshot& s, const UnitState& corpse, BotContext& ctx)
{
    if (!ctx.provider || !ctx.provider->targetsByName() || s.targetGuid == corpse.guid)
    {
        return true;
    }

    const TimePoint now = ctx.now();

    if (!targetTypedAt_)
    {
        // A local command: nobody else sees it.
        if (!ctx.actions.chat("/target " + corpse.name))
        {
            return true; // no chat: the soft target may still take it
        }

        targetTypedAt_ = now;
        return false;
    }

    // The client shows the new target a moment later.
    if (now - *targetTypedAt_ < Duration(1500))
    {
        return false;
    }

    // /target took another of the name - perhaps a living one. Never
    // interact with that: it would be attacked.
    if (const UnitState* other = s.target(); other && other->guid != corpse.guid)
    {
        ctx.actions.execute(actions::TargetClear);
    }

    return std::nullopt;
}

void LootBehavior::giveUp(const std::string& reason, BotContext& ctx)
{
    ctx.log.warn(logcat::Loot, "giving up on corpse {}: {}", formatGuid(current_), reason);
    ctx.objectBlacklist.add(current_, Duration(300000), reason);
    std::erase_if(ctx.pendingLoot, [&](const PendingLoot& p) { return p.guid == current_; });
    ctx.objectives.pop("LootCorpse");
    ctx.navigator.stop();
    ctx.cancelInteraction();
    ctx.restoreCamera();
    current_ = 0;
}

BehaviorStatus LootBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (ctx.standUp(s))
    {
        return BehaviorStatus::Running;
    }

    // Check receipt before handling a despawn: bags can update in the same
    // snapshot that removes the corpse from the world and pending queue.
    if (current_ != 0 && lootAttempts_ > 0 && skinAttempts_ == 0 && !lootReceived_)
    {
        const bool money = moneyBefore_ >= 0 && s.player.money > moneyBefore_;
        const bool items = receivedItems(s.player, itemsBefore_);
        const bool slots = freeSlotsBefore_ >= 0 && s.player.freeBagSlots >= 0 && s.player.freeBagSlots < freeSlotsBefore_;

        if (money || items || slots)
        {
            lootReceived_ = true;
            ctx.seekTook(current_);
            ctx.log.info(logcat::Loot, "loot received from corpse {} (money={} items={} bagSlotUsed={})", formatGuid(current_), money, items, slots);
        }
    }

    const bool tracked = std::any_of(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return p.guid == current_; });

    // Pruned by the CombatTracker (corpse despawned/blacklisted).
    if (current_ != 0 && !tracked && skinAttempts_ == 0)
    {
        finish(s.findUnit(current_), ctx);
    }

    if (current_ == 0)
    {
        selectNext(s, ctx);
    }

    if (current_ == 0)
    {
        return BehaviorStatus::Success;
    }

    const TimePoint now = ctx.now();
    const UnitState* unit = s.findUnit(current_);
    const auto pending = std::find_if(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return p.guid == current_; });

    // When under attack, only quest corpses are looted immediately before fighting
    if (!s.attackers().empty() && (!unit || !ctx.questNeeds(unit->entry)))
    {
        if (pending != ctx.pendingLoot.end())
        {
            pending->inProgress = false;
        }

        ctx.objectives.pop("LootCorpse");
        ctx.navigator.stop();
        current_ = 0;
        return BehaviorStatus::Failure;
    }

    if (const int carried = carriedItems(s.player); carried != lastCarried_)
    {
        if (lastCarried_ >= 0)
        {
            bagsChangedAt_ = now;
        }

        lastCarried_ = carried;
    }

    if (skinAttempts_ > 0)
    {
        if (s.spellMovingErrors > 0)
        {
            ctx.log.info(logcat::Loot, "skinning interrupted by movement ('Can't do that while moving') - retrying immediately");
            ctx.navigator.stop();
            ctx.movement.stop();
            ctx.actions.clearCooldown(actions::Interact);
            skinCasting_ = false;
            skinRetryMoving_ = true;
            lastInteract_ = TimePoint{};
        }
        else if (s.player.isCasting)
        {
            // What reached the bags before the cast was the corpse's loot.
            if (!skinCasting_)
            {
                skinItemsBefore_ = carriedItems(s.player);
            }

            skinCasting_ = true;
            skinCastSeen_ = true;
            skinRetryMoving_ = false;
            return BehaviorStatus::Running;
        }
        else if (skinCasting_)
        {
            skinCasting_ = false;
            lastInteract_ = now; // allow the skin loot to arrive after the cast
            skinWait_ = ctx.randomBetween(ctx.config.skinIntervalMin, ctx.config.skinIntervalMax);
        }

        skinLootReceived_ = skinLootReceived_ || (skinCastSeen_ && carriedItems(s.player) > skinItemsBefore_);

        if (skinLootReceived_)
        {
            // The bag update confirms completion; no arrival grace is needed.
            finish(unit, ctx);
            return BehaviorStatus::Running;
        }

        if (!unit || !unit->isSkinnable)
        {
            if (!skinFinishedAt_)
            {
                skinFinishedAt_ = now;
            }

            // Despawn/flag removal can precede the loot window and bag update.
            emptyLootWindow(ctx);

            if (now - *skinFinishedAt_ >= ctx.config.lootArrivalWait)
            {
                finish(unit, ctx);
            }

            return BehaviorStatus::Running;
        }

        if (!skinRetryMoving_ && now - lastInteract_ < skinWait_)
        {
            emptyLootWindow(ctx);
            return BehaviorStatus::Running;
        }
    }

    if (!unit)
    {
        finish(nullptr, ctx);
        return BehaviorStatus::Running;
    }

    if (s.player.isCasting)
    {
        ctx.seekTook(current_);
        return BehaviorStatus::Running;
    }

    if (s.player.freeBagSlots == 0)
    {
        giveUp("bags are full", ctx);
        return BehaviorStatus::Failure;
    }

    // A corpse on the bed of a lake is reached by diving: there the height
    // counts too.
    const auto surfaceThere = ctx.navigator.provider().waterSurface(unit->position);
    const bool underWater = surfaceThere && unit->position.z < *surfaceThere - 1.5f;
    const float dist = underWater ? distance(s.player.position, unit->position) : distance2D(s.player.position, unit->position);

    // Up for air on the way to it: too deep for one breath.
    if (underWater && ctx.breath.surfacedAt && *ctx.breath.surfacedAt >= moveStarted_)
    {
        giveUp("too deep: had to come up for air on the way", ctx);
        return BehaviorStatus::Failure;
    }

    // Not the same step short of every corpse (the humanizer).
    const float reach = ctx.humanReach(ctx.config.interactDistance, unit->guid);

    if (dist > reach)
    {
        // A quest's mob back at once by its corpse (Garrick Padfoot): guarded
        // for ever, and pulled for ever. What the quest needs is taken all the same.
        const bool needed = ctx.questNeeds(unit->entry);

        if (lootAttempts_ + skinAttempts_ == 0 && !needed && ctx.corpseGuarded(s, unit->position))
        {
            // Walking into it would pull whatever stands there. Leave the corpse
            // queued: the pull clears the way, or the hostile wanders off.
            ctx.log.info(logcat::Loot, "hostile within {:.0f} yd of {} - leaving it for now", ctx.config.lootDangerRadius, unit->name);

            if (pending != ctx.pendingLoot.end())
            {
                pending->inProgress = false;
            }

            ctx.objectives.pop("LootCorpse");
            ctx.navigator.stop();
            current_ = 0;
            return BehaviorStatus::Running;
        }

        if (now - moveStarted_ > ctx.config.lootMoveTimeout)
        {
            giveUp("could not reach corpse in time", ctx);
            return BehaviorStatus::Failure;
        }

        if (ctx.navigator.moveTo(s, unit->position, reach * 0.7f) == NavStatus::Unreachable)
        {
            giveUp("unreachable", ctx);
            return BehaviorStatus::Failure;
        }

        return BehaviorStatus::Running;
    }

    ctx.navigator.halt();

    if (!ctx.turningRound(current_) && !ctx.movement.face(s.player, unit->position, 0.4f))
    {
        lootReadySince_.reset();
        return BehaviorStatus::Running;
    }

    if (unit->isLootable)
    {
        flagSeen_ = true;
    }

    // Where the flag cannot be trusted, retry up to lootAttempts, allowing
    // lootArrivalWait after each interact. Not while the fight goes on:
    // 2.5.6 flags the corpse only after it, and an interact before finds
    // nothing - the bot then left corpses with their loot on.
    const bool fightOver = pending != ctx.pendingLoot.end() && now - pending->graceFrom >= ctx.config.lootAfterFightDelay;
    const bool tryUnflagged = !skinOnly_ && ctx.config.lootWithoutFlag && !unit->isLootable && !flagSeen_ && !lootReceived_ && fightOver && (lootAttempts_ < ctx.config.lootAttempts || (lootAttempts_ > 0 && now - lastInteract_ < ctx.config.lootArrivalWait));
    const bool lootable = !skinOnly_ && skinAttempts_ == 0 && ((unit->isLootable && !lootReceived_) || tryUnflagged);
    const bool canSkin = ctx.canSkin(*unit, s.player);
    const bool skinnable = canSkin && unit->isSkinnable;

    if (!lootable && !skinnable)
    {
        // The skinnable flag can follow the loot window closing by a moment.
        if (canSkin && lootAttempts_ > 0 && skinAttempts_ == 0)
        {
            if (!lootDoneAt_)
            {
                lootDoneAt_ = now;
            }

            ctx.seekTook(current_);

            if (now - *lootDoneAt_ < ctx.config.skinFlagWait)
            {
                emptyLootWindow(ctx);
                return BehaviorStatus::Running;
            }
        }

        const auto it = std::find_if(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return p.guid == current_; });
        const bool waitingForFlag = !skinOnly_ && lootAttempts_ == 0 && it != ctx.pendingLoot.end() && now - it->graceFrom < ctx.config.lootGrace;

        if (!waitingForFlag)
        {
            finish(unit, ctx);
        }

        return BehaviorStatus::Running;
    }

    // Arrived and facing the corpse: a moment's wait before the first loot,
    // or before the first skin of a corpse there is nothing to loot from.
    if (lootAttempts_ + skinAttempts_ == 0)
    {
        if (!lootReadySince_)
        {
            lootReadySince_ = now;
            lootWait_ = ctx.randomBetween(ctx.config.lootDelayMin, ctx.config.lootDelayMax);
        }

        if (now - *lootReadySince_ < lootWait_)
        {
            return BehaviorStatus::Running;
        }
    }

    if (lootable)
    {
        const Duration retryInterval = tryUnflagged ? std::max(ctx.config.lootInteractInterval, ctx.config.lootArrivalWait) : ctx.config.lootInteractInterval;

        if (lootAttempts_ > 0 && now - lastInteract_ < retryInterval)
        {
            // Empty the window while waiting on it. Only here: the client runs one
            // queued request at a time, and a script queued right before the
            // interact would leave no room for a right-click made through it.
            emptyLootWindow(ctx);
            return BehaviorStatus::Running;
        }

        if (lootAttempts_ >= ctx.config.lootAttempts)
        {
            giveUp("loot window never finished", ctx);
            return BehaviorStatus::Failure;
        }

        // A client whose target cannot be set: the corpse under the mouse,
        // right-clicked as a player would (Auto Loot takes the loot) - or
        // where it does not say what is under the mouse, targeted by name.
        // A corpse tried without its flag has most often nothing to give: no
        // reason to look closer.
        if (!tryUnflagged && ctx.zoomInForClick(lootAttempts_))
        {
            return BehaviorStatus::Running;
        }

        const auto seek = ctx.seekTurningRound(s.player, current_, unit->position);

        if (seek == BotContext::MouseSeek::Searching)
        {
            return BehaviorStatus::Running;
        }

        if (seek == BotContext::MouseSeek::NotFound)
        {
            ctx.stopMouseSeek();
            giveUp("it was never under the mouse", ctx);
            return BehaviorStatus::Failure;
        }

        const bool underMouse = seek == BotContext::MouseSeek::Found;

        if (!underMouse)
        {
            const auto targeted = targetCorpse(s, *unit, ctx);

            if (!targeted)
            {
                giveUp("it could not be targeted by name", ctx);
                return BehaviorStatus::Failure;
            }

            if (!*targeted)
            {
                return BehaviorStatus::Running;
            }
        }

        if (lootAttempts_ > 0)
        {
            ctx.log.info(logcat::Loot, "loot not confirmed for {}: money {} -> {}, free bag slots {} -> {}, lootable={}", unit->name, moneyBefore_, s.player.money, freeSlotsBefore_, s.player.freeBagSlots, unit->isLootable);
        }

        ctx.log.info(logcat::Loot, "looting {} (attempt {})", unit->name, lootAttempts_ + 1);

        if (lootAttempts_ == 0)
        {
            moneyBefore_ = s.player.money;
            itemsBefore_ = s.player.bagItems;
            freeSlotsBefore_ = s.player.freeBagSlots;
        }

        if (underMouse)
        {
            ctx.actions.click(MouseButton::Right);
            ctx.stopMouseSeek();
        }
        else
        {
            // 1. CTM Loot direct in memory (no action key required)
            const bool ctmOk = ctx.interactUnit(current_, unit->position);

            // 2. Set target in memory and trigger action key if configured
            ctx.setTarget(current_);

            if (!ctmOk || ctx.actions.hasBinding(actions::Interact))
            {
                ctx.actions.execute(actions::Interact);
            }
        }

        ++lootAttempts_;
        lastInteract_ = now;
        skinWait_ = ctx.randomBetween(ctx.config.skinIntervalMin, ctx.config.skinIntervalMax);
        return BehaviorStatus::Running;
    }

    // Skinning (give the loot window time to close first).
    if (ctx.movement.isMoving() || s.player.isMoving)
    {
        ctx.navigator.stop();
        ctx.movement.stop();
        return BehaviorStatus::Running;
    }

    if (lootAttempts_ + skinAttempts_ > 0 && !skinRetryMoving_ && now - lastInteract_ < skinWait_)
    {
        emptyLootWindow(ctx);
        return BehaviorStatus::Running;
    }

    // Another player's kill, where the flag is up from the kill: its owner may
    // not have looted it yet, and then no try would take. One is enough.
    const int maxSkinAttempts = skinOnly_ && !ctx.config.skinFlagTrusted ? 1 : ctx.config.skinAttempts;

    // Loot still coming in: the loot window is not done with the corpse yet,
    // and an interact now would only open it again.
    if (skinAttempts_ == 0 && bagsChangedAt_ && now - *bagsChangedAt_ < Duration(600))
    {
        emptyLootWindow(ctx);
        return BehaviorStatus::Running;
    }

    if (!skinRetryMoving_ && skinAttempts_ >= maxSkinAttempts)
    {
        giveUp(skinOnly_ ? "skinning failed - its owner may not have looted it yet" : "skinning failed", ctx);
        return BehaviorStatus::Failure;
    }

    // A right-click on the corpse under the mouse skins it, as it loots it.
    if (ctx.zoomInForClick(skinAttempts_))
    {
        return BehaviorStatus::Running;
    }

    const auto seek = ctx.seekTurningRound(s.player, current_, unit->position);

    if (seek == BotContext::MouseSeek::Searching)
    {
        return BehaviorStatus::Running;
    }

    if (seek == BotContext::MouseSeek::NotFound)
    {
        ctx.stopMouseSeek();
        giveUp("it was never under the mouse", ctx);
        return BehaviorStatus::Failure;
    }

    if (skinRetryMoving_)
    {
        ctx.log.info(logcat::Loot, "skinning {} (retrying immediately after movement)", unit->name);
    }
    else
    {
        ctx.log.info(logcat::Loot, "skinning {} (attempt {})", unit->name, skinAttempts_ + 1);
    }

    if (skinAttempts_ == 0)
    {
        skinItemsBefore_ = carriedItems(s.player);
    }

    ctx.cancelInteraction();
    ctx.setTarget(current_);

    if (skinRetryMoving_)
    {
        ctx.actions.clearCooldown(actions::Interact);
    }

    if (seek == BotContext::MouseSeek::Found)
    {
        ctx.actions.click(MouseButton::Right);
        ctx.stopMouseSeek();
    }
    else
    {
        const bool clicked = ctx.provider && ctx.provider->skinUnit(current_);

        if (!clicked)
        {
            ctx.actions.execute(actions::Interact);
        }
    }

    if (!skinRetryMoving_)
    {
        ++skinAttempts_;
    }
    else
    {
        skinRetryMoving_ = false;
    }

    lastInteract_ = now;
    skinWait_ = ctx.randomBetween(ctx.config.skinIntervalMin, ctx.config.skinIntervalMax);
    return BehaviorStatus::Running;
}

} // namespace og
