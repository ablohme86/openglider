/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-21
 *  File: FishBehavior.cpp
 *
 *  Desc:
 *  Implementation of FishBehavior (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/behaviors/Behaviors.h"

#include "og/actions/ActionBinding.h"
#include "og/state/IGameStateProvider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <map>

namespace og
{
namespace
{

// Fishing lures, 1.12 through 3.3.5a: Shiny Bauble, Nightcrawlers, Bright
// Baubles, Aquadynamic Fish Attractor, Aquadynamic Fish Lens, Flesh Eating
// Worm, Sharpened Fish Hook, Glow Worm.
constexpr std::array<std::uint32_t, 8> kLures{6529, 6530, 6532, 6533, 6811, 7307, 34861, 46006};

// The first lure in the bags, 0 for none.
std::uint32_t carriedLure(const PlayerState& p)
{
    for (const auto& item : p.bagItems)
    {
        if (item.count > 0 && std::find(kLures.begin(), kLures.end(), item.itemId) != kLures.end())
        {
            return item.itemId;
        }
    }

    return 0;
}

bool carriesLure(const PlayerState& p)
{
    return carriedLure(p) != 0;
}

// Uses the first lure in the bags on the main hand (slot 16), unless the pole
// already has one. The same calls from 1.12 to 3.3.5a.
constexpr std::string_view kLureScript = "if GetWeaponEnchantInfo() then return end "
                                         "local L={[6529]=1,[6530]=1,[6532]=1,[6533]=1,[6811]=1,[7307]=1,[34861]=1,[46006]=1} "
                                         "for b=0,4 do for s=1,GetContainerNumSlots(b) do local l=GetContainerItemLink(b,s) "
                                         "if l then local _,_,id=string.find(l,\"item:(%d+)\") if id and L[tonumber(id)] then "
                                         "UseContainerItem(b,s) PickupInventoryItem(16) if ReplaceEnchant then ReplaceEnchant() end return end end "
                                         "end end";

bool receivedItems(const std::vector<ItemStack>& before, const std::vector<ItemStack>& after)
{
    using Key = std::pair<std::uint32_t, std::string>;
    const auto counts = [](const std::vector<ItemStack>& items)
    {
        std::map<Key, int> out;

        for (const auto& item : items)
        {
            out[{item.itemId, item.itemId ? std::string{} : item.name}] += item.count;
        }

        return out;
    };
    const auto previous = counts(before);

    for (const auto& [key, count] : counts(after))
    {
        const auto it = previous.find(key);

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

std::string_view stageName(int stage)
{
    constexpr std::array<std::string_view, 9> names{"travel", "face", "lure", "cast", "wait for bobber", "wait for bite", "hook", "collect", "pause"};
    return names[static_cast<std::size_t>(stage)];
}

} // namespace

std::optional<FishBehavior::Spot> FishBehavior::chooseSpot(const GameSnapshot& s, const BotContext& ctx) const
{
    if (!ctx.profile || ctx.profile->fishingSpots.empty())
    {
        // Glider fished where the character stood, facing the way it faced.
        return Spot{s.player.position, std::nullopt, 15.0f};
    }

    const FishingSpot* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();

    for (const auto& spot : ctx.profile->fishingSpots)
    {
        if (!s.hostilesNear(spot.position, spot.safeRadius).empty())
        {
            continue;
        }

        const float d = distance(s.player.position, spot.position);

        if (d < bestDistance)
        {
            bestDistance = d;
            best = &spot;
        }
    }

    if (!best)
    {
        return std::nullopt;
    }

    return Spot{best->position, best->heading, best->safeRadius};
}

const GameObjectState* FishBehavior::findBobber(const GameSnapshot& s) const
{
    // The nearest bobber that appeared after this cast. A bobber stays in the
    // world a while after its catch, still ours: without the "after" it was
    // taken for the new one, never bit, and the bot stopped hooking.
    const GameObjectState* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();

    for (const auto& g : s.objects)
    {
        if (g.kind != GameObjectKind::FishingBobber || bobbersBefore_.contains(g.guid))
        {
            continue;
        }

        if (g.createdBy != 0 && g.createdBy != s.player.guid)
        {
            continue;
        }

        const float d = distance(s.player.position, g.position);

        if (d < bestDistance)
        {
            bestDistance = d;
            best = &g;
        }
    }

    return best;
}

bool FishBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes, and nowhere on its own.
    if (ctx.partyFollower())
    {
        return false;
    }

    const bool ok = fishable(s, ctx);

    if (!ok)
    {
        ctx.fishingBusy = false;
    }

    return ok;
}

bool FishBehavior::fishable(const GameSnapshot& s, BotContext& ctx)
{
    std::string& problem = ctx.fishingProblem;
    problem.clear();

    if (!ctx.fishingEnabled() || s.isDead() || s.inCombat() || !s.attackers().empty() || !ctx.pendingLoot.empty())
    {
        return false;
    }

    if (stopped_)
    {
        problem = "stopped after casts without a bobber - face fishable water with a pole equipped, then Start again";
        return false;
    }

    if (s.player.freeBagSlots == 0)
    {
        problem = "the bags are full";
        return false;
    }

    if (!s.fishing.biteKnown)
    {
        problem = "this client's offsets cannot tell a bite (no gameObjectBobbing in its layout)";

        if (!warnedNoBite_)
        {
            ctx.log.warn(logcat::Fish, "this client's layout cannot tell a bite - no fishing (it needs "
                                       "gameObjectBobbing)");
        }

        warnedNoBite_ = true;
        return false;
    }

    if (!ctx.actions.hasBinding(actions::FishCast) && !ctx.actions.hasBinding(actions::Fish))
    {
        problem = "Fishing has no key";

        if (!warnedNoSpell_)
        {
            ctx.log.warn(logcat::Fish, "Fishing is not on an action bar key - no fishing");
        }

        warnedNoSpell_ = true;
        return false;
    }

    if (!spot_ && !chooseSpot(s, ctx))
    {
        problem = "hostiles stand at every fishing spot";
        return false;
    }

    return true;
}

void FishBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    if (!spot_)
    {
        spot_ = chooseSpot(s, ctx);
    }

    // Back after something else ran: carry on with a cast that is still out,
    // or start over from getting to the spot.
    const bool castOut = stage_ == Stage::WaitBobber || stage_ == Stage::WaitBite || stage_ == Stage::Hook || stage_ == Stage::Collect;

    if (castOut && ctx.now() - stageStarted_ < ctx.config.fishing.biteTimeout && (stage_ == Stage::WaitBobber || s.findObject(bobber_)))
    {
        ctx.log.debug(logcat::Fish, "resuming at {}", stageName(static_cast<int>(stage_)));
        return;
    }

    bobber_ = 0;
    setStage(Stage::Travel, ctx);
}

void FishBehavior::onExit(BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.objectives.removeAll("Fish");
}

void FishBehavior::setStage(Stage next, BotContext& ctx)
{
    if (next != stage_)
    {
        ctx.log.debug(logcat::Fish, "{} -> {}", stageName(static_cast<int>(stage_)), stageName(static_cast<int>(next)));
    }

    stage_ = next;
    stageStarted_ = ctx.now();
}

Duration FishBehavior::between(Duration min, Duration max)
{
    random_ ^= random_ << 13;
    random_ ^= random_ >> 7;
    random_ ^= random_ << 17;
    const auto span = std::max<std::int64_t>(0, (max - min).count());
    return min + Duration(span ? static_cast<std::int64_t>(random_ % static_cast<std::uint64_t>(span + 1)) : 0);
}

void FishBehavior::pause(BotContext& ctx, Duration min, Duration max)
{
    pauseUntil_ = ctx.now() + between(min, max);
    setStage(Stage::Pause, ctx);
}

bool FishBehavior::lureDue(const GameSnapshot& s, BotContext& ctx) const
{
    const int minutes = ctx.profile ? ctx.profile->options.lureMinutes : 0;

    if ((minutes <= 0 && !ctx.fishingLure) || lureUnavailable_ || !carriesLure(s.player))
    {
        return false;
    }

    // Give the last one time to show on the pole before judging it.
    if (lureAppliedAt_ && ctx.now() - *lureAppliedAt_ < ctx.config.fishing.lureApplyTime + Duration(10000))
    {
        return false;
    }

    // Where the client shows the pole's enchant: when it is gone or about to go.
    if (const auto& enchant = s.player.mainHandTempEnchant)
    {
        return *enchant == 0 || s.player.mainHandTempEnchantSecondsLeft < 30;
    }

    if (minutes <= 0)
    {
        return !lureAppliedAt_;
    }

    return !lureAppliedAt_ || ctx.now() - *lureAppliedAt_ >= Duration(static_cast<std::int64_t>(minutes) * 60000);
}

bool FishBehavior::applyLure(const GameSnapshot& s, BotContext& ctx)
{
    // A key (the player's own macro) first, then Lua where the client runs it,
    // then the chat: the slash commands a player would type.
    if (ctx.actions.hasBinding(actions::FishingLure))
    {
        return ctx.actions.execute(actions::FishingLure) == ActionResult::Executed;
    }

    if (ctx.provider && ctx.provider->supportsLua())
    {
        return ctx.provider->executeLua(kLureScript);
    }

    if (const auto lure = carriedLure(s.player); lure && ctx.actions.chat(std::format("/use item:{}", lure)))
    {
        ctx.actions.chat("/use 16");

        // A lure not quite run out asks whether to replace it. Only then: a
        // hidden popup's button would answer whatever it last asked. Where the
        // UI can be read, the dialog that asks it is clicked (while pausing).
        if (s.player.mainHandTempEnchant.value_or(0) != 0)
        {
            if (ctx.provider && ctx.provider->supportsUi())
            {
                lureConfirmUntil_ = ctx.now() + ctx.config.fishing.lureApplyTime;
            }
            else
            {
                ctx.actions.chat("/click StaticPopup1Button1");
            }
        }

        return true;
    }

    ctx.log.warn(logcat::Fish, "no way to put a lure on the pole here (bind {}) - fishing without", actions::FishingLure);
    lureUnavailable_ = true;
    return false;
}

BehaviorStatus FishBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (lateCatchUntil_)
    {
        if (ctx.now() > *lateCatchUntil_)
        {
            lateCatchUntil_.reset();
        }
        else if (receivedItems(bagsBefore_, s.player.bagItems))
        {
            lateCatchUntil_.reset();
            --ctx.stats.fishMissed;
            ++ctx.stats.fishCaught;
            ctx.log.info(logcat::Fish, "the catch came after all ({} so far)", ctx.stats.fishCaught);
        }
    }

    const auto status = step(s, ctx);
    ctx.fishingBusy = status == BehaviorStatus::Running && (stage_ == Stage::WaitBobber || stage_ == Stage::WaitBite || stage_ == Stage::Hook || stage_ == Stage::Collect);
    return status;
}

BehaviorStatus FishBehavior::step(const GameSnapshot& s, BotContext& ctx)
{
    if (!spot_)
    {
        spot_ = chooseSpot(s, ctx);
    }

    if (!spot_)
    {
        return BehaviorStatus::Failure;
    }

    const auto& config = ctx.config.fishing;
    const TimePoint now = ctx.now();

    // Knocked off the spot (or never there): walk back first.
    if (stage_ != Stage::Travel && distance2D(s.player.position, spot_->position) > config.arriveRadius * 3.0f)
    {
        bobber_ = 0;
        setStage(Stage::Travel, ctx);
    }

    switch (stage_)
    {
    case Stage::Travel:
    {
        if (distance2D(s.player.position, spot_->position) > config.arriveRadius)
        {
            if (ctx.navigator.moveTo(s, spot_->position, config.arriveRadius * 0.7f) == NavStatus::Unreachable)
            {
                ctx.log.warn(logcat::Fish, "fishing spot {} is unreachable", spot_->position);
                stopped_ = true;
                return BehaviorStatus::Failure;
            }

            return BehaviorStatus::Running;
        }

        ctx.navigator.stop();
        ctx.movement.stop();
        ctx.objectives.removeAll("Fish");
        ctx.objectives.push(Objective{"Fish", 0, spot_->position});
        setStage(Stage::Face, ctx);
        return BehaviorStatus::Running;
    }

    case Stage::Face:
        if (spot_->heading && !ctx.movement.faceHeading(s.player, *spot_->heading, 0.1f))
        {
            return BehaviorStatus::Running;
        }

        ctx.movement.stopTurning();
        setStage(lureDue(s, ctx) ? Stage::Lure : Stage::Cast, ctx);
        return BehaviorStatus::Running;

    case Stage::Lure:
        if (applyLure(s, ctx))
        {
            lureAppliedAt_ = now;
            ctx.log.info(logcat::Fish, "putting a lure on the pole");
            pause(ctx, config.lureApplyTime, config.lureApplyTime + Duration(800));
        }
        else
        {
            setStage(Stage::Cast, ctx);
        }

        return BehaviorStatus::Running;

    case Stage::Cast:
    {
        if (s.player.isMoving || ctx.movement.isMoving())
        {
            ctx.movement.stop();
            return BehaviorStatus::Running;
        }

        bobbersBefore_.clear();

        for (const auto& g : s.objects)
        {
            if (g.kind == GameObjectKind::FishingBobber)
            {
                bobbersBefore_.insert(g.guid);
            }
        }

        const auto cast = ctx.actions.hasBinding(actions::FishCast) ? actions::FishCast : actions::Fish;

        if (ctx.actions.execute(cast) != ActionResult::Executed)
        {
            return BehaviorStatus::Running;
        }

        ++ctx.stats.fishCasts;
        bobber_ = 0;
        setStage(Stage::WaitBobber, ctx);
        return BehaviorStatus::Running;
    }

    case Stage::WaitBobber:
    {
        if (const auto* bobber = findBobber(s))
        {
            bobber_ = bobber->guid;
            bobberSpots_.clear();
            bobberNotFound_ = false;
            bobberPosition_ = bobber->position;
            failedCasts_ = 0;
            ctx.log.debug(logcat::Fish, "bobber {} is in the water", formatGuid(bobber_));
            setStage(Stage::WaitBite, ctx);
            return BehaviorStatus::Running;
        }

        if (now - stageStarted_ < config.bobberTimeout)
        {
            return BehaviorStatus::Running;
        }

        ++failedCasts_;
        ++ctx.stats.fishNoBobber;
        ctx.log.warn(logcat::Fish, "no bobber after casting ({} in a row)", failedCasts_);

        if (failedCasts_ >= config.maxFailedCasts)
        {
            ctx.log.error(logcat::Fish,
                          "{} casts without a bobber - is the character facing fishable water with a "
                          "pole equipped? Fishing stopped",
                          failedCasts_);
            stopped_ = true;
            return BehaviorStatus::Failure;
        }

        pause(ctx, config.recastDelayMin, config.recastDelayMax);
        return BehaviorStatus::Running;
    }

    case Stage::WaitBite:
    {
        const auto* bobber = s.findObject(bobber_);

        if (!bobber || now - stageStarted_ > config.biteTimeout)
        {
            ++ctx.stats.fishNoBite;
            ctx.log.info(logcat::Fish, "no bite before the bobber went");
            pause(ctx, config.missRecastDelayMin, config.missRecastDelayMax);
            return BehaviorStatus::Running;
        }

        bobberPosition_ = bobber->position;

        // Without memory interaction, have the mouse on the bobber before the bite.
        if (!bobberNotFound_ && !(ctx.provider && ctx.provider->supportsObjectInteraction()))
        {
            if (bobberSpots_.empty())
            {
                bobberSpots_ = ctx.buildTargetSeekSpots(s.player, bobber->position, 0.0f);
            }

            if (ctx.seekUnderMouse(bobber_, &bobberSpots_) == BotContext::MouseSeek::NotFound)
            {
                ctx.log.warn(logcat::Fish, "the bobber was never under the mouse - hooking with {}", actions::Interact);
                ctx.stopMouseSeek();
                bobberNotFound_ = true;
                // Its binding typed now, not at the bite.
                ctx.actions.prepare(actions::Interact);
            }
        }

        if (!bobber->isBobbing)
        {
            return BehaviorStatus::Running;
        }

        hookAt_ = now + between(config.hookDelayMin, config.hookDelayMax);
        setStage(Stage::Hook, ctx);
        return BehaviorStatus::Running;
    }

    case Stage::Hook:
    {
        if (now < hookAt_)
        {
            return BehaviorStatus::Running;
        }

        lateCatchUntil_.reset();
        bagsBefore_ = s.player.bagItems;
        // Memory interaction where the client has it; otherwise the interact
        // key, which the bindings addon points at the soft target in front -
        // the bobber, on the line the cast went out.
        const bool viaMemory = ctx.provider && ctx.provider->supportsObjectInteraction() && ctx.provider->interactObject(bobber_, bobberPosition_);
        const bool underMouse = !viaMemory && ctx.provider && ctx.provider->mouseoverGuid() == bobber_;

        if (viaMemory)
        {
            ctx.log.info(logcat::Fish, "bite - hooking the bobber");
        }
        else if (underMouse)
        {
            // Right-clicking the bobber hooks it, and Auto Loot takes the catch.
            ctx.actions.click(MouseButton::Right);
            ctx.log.info(logcat::Fish, "bite - right-clicking the bobber");
        }
        else
        {
            const auto result = ctx.actions.execute(actions::Interact);

            // The key waits for the chat - its binding typed first, or a line
            // just gone out: pressed again the next tick, while the bite
            // lasts. Given up on at once, the fish got away.
            if (result == ActionResult::InputUnavailable && now - hookAt_ < kHookRetry)
            {
                return BehaviorStatus::Running;
            }

            ctx.log.info(logcat::Fish,
                         "bite - hooking with {}: the game needs Interact With Target on its key "
                         "and SoftTargetInteract 3",
                         actions::Interact);

            if (result != ActionResult::Executed)
            {
                ctx.log.warn(logcat::Fish, "could not press {}: {}", actions::Interact, toString(result));
            }
        }

        ctx.stopMouseSeek();
        biteOverAt_.reset();
        setStage(Stage::Collect, ctx);
        return BehaviorStatus::Running;
    }

    case Stage::Collect:
        if (receivedItems(bagsBefore_, s.player.bagItems))
        {
            ++ctx.stats.fishCaught;
            ctx.log.info(logcat::Fish, "caught something ({} so far)", ctx.stats.fishCaught);
            pause(ctx, config.recastDelayMin, config.recastDelayMax);
            return BehaviorStatus::Running;
        }

        emptyLootWindow(ctx);
        {
            const auto* bobber = s.findObject(bobber_);

            if (!biteOverAt_ && (!bobber || !bobber->isBobbing))
            {
                biteOverAt_ = now;
            }

            const bool gotAway = biteOverAt_ && now - *biteOverAt_ >= config.escapeWait;

            if (!gotAway && now - stageStarted_ < config.lootWait)
            {
                return BehaviorStatus::Running;
            }
        }

        ++ctx.stats.fishMissed;
        lateCatchUntil_ = now + config.lootWait;
        ctx.log.info(logcat::Fish, "the fish got away ({} missed)", ctx.stats.fishMissed);
        pause(ctx, config.missRecastDelayMin, config.missRecastDelayMax);
        return BehaviorStatus::Running;

    case Stage::Pause:
        // "Do you want to replace <lure> with <lure>?"
        if (lureConfirmUntil_ && (now > *lureConfirmUntil_ || ctx.clickDialog("replace")))
        {
            lureConfirmUntil_.reset();
        }

        if (now < pauseUntil_)
        {
            return BehaviorStatus::Running;
        }

        setStage(lureDue(s, ctx) ? Stage::Lure : Stage::Cast, ctx);
        return BehaviorStatus::Running;
    }

    return BehaviorStatus::Running;
}

} // namespace og
