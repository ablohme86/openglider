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
 *  File: BotEngine.cpp
 *
 *  Desc:
 *  Implementation of BotEngine (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/BotEngine.h"

#include "og/core/Events.h"

#include <algorithm>
#include <thread>

namespace og
{

BotEngine::BotEngine(IGameStateProvider& provider, BotContext& ctx) : provider_(&provider), ctx_(ctx)
{
}

void BotEngine::addBehavior(std::unique_ptr<IBotBehavior> behavior)
{
    behaviors_.push_back(std::move(behavior));
    std::stable_sort(behaviors_.begin(), behaviors_.end(), [](const auto& a, const auto& b) { return a->priority() > b->priority(); });
}

IBotBehavior* BotEngine::findBehavior(std::string_view name) const
{
    for (const auto& b : behaviors_)
    {
        if (b->name() == name)
        {
            return b.get();
        }
    }

    return nullptr;
}

std::string BotEngine::currentName() const
{
    if (state_ == EngineState::Paused && !manualTravel_)
    {
        return "Paused";
    }

    return current_ ? std::string(current_->name()) : std::string("Idle");
}

void BotEngine::pause()
{
    state_ = EngineState::Paused;
}

void BotEngine::resume()
{
    state_ = EngineState::Running;
}

void BotEngine::setManualTravel(bool active)
{
    if (manualTravel_ == active)
    {
        return;
    }

    manualTravel_ = active;
    travelDefense_ = false;
}

void BotEngine::switchTo(IBotBehavior* next, const GameSnapshot& snapshot)
{
    if (next == current_)
    {
        return;
    }

    const std::string from = current_ ? std::string(current_->name()) : std::string("Idle");
    const std::string to = next ? std::string(next->name()) : std::string("Idle");

    if (current_)
    {
        current_->onYield(ctx_, to);
    }

    current_ = next;
    ctx_.log.info(logcat::State, "{} -> {}", from, to);

    if (current_)
    {
        current_->onEnter(snapshot, ctx_);
    }

    ctx_.events.publish(BehaviorChangedEvent{from, to});
}

void BotEngine::detectTransitions(const GameSnapshot& prev, const GameSnapshot& cur)
{
    const PlayerState& a = prev.player;
    const PlayerState& b = cur.player;

    // An unreadable balance (-1), or a different character, starts a fresh
    // baseline rather than counting their existing money as session earnings.
    if (a.guid == b.guid && a.money >= 0 && b.money > a.money)
    {
        ctx_.stats.moneyGathered += b.money - a.money;
    }

    if (!a.isDeadOrGhost() && b.isDeadOrGhost())
    {
        ++ctx_.stats.deaths;
        ctx_.lastDeathPosition = b.position;
        ctx_.log.warn(logcat::Death, "player died at {}", b.position);
        ctx_.events.publish(PlayerDiedEvent{b.position});
    }
    else if (a.isDeadOrGhost() && !b.isDeadOrGhost())
    {
        ctx_.log.info(logcat::Death, "player resurrected at {}", b.position);
        ctx_.events.publish(PlayerResurrectedEvent{b.position});
    }

    if (!a.inCombat && b.inCombat)
    {
        ctx_.log.info(logcat::Combat, "combat started");
        ctx_.events.publish(CombatStartedEvent{});
    }
    else if (a.inCombat && !b.inCombat)
    {
        const double duration = toSeconds(cur.timestamp - ctx_.combat.fightStarted);
        ctx_.log.info(logcat::Combat, "combat ended");
        ctx_.events.publish(CombatEndedEvent{duration});
    }

    // The configured castTime is only an estimate, and a guess for the window
    // between pressing the key and the client reporting the cast. Once the
    // client says a cast has ended, that is the truth - talents and haste make
    // the real cast shorter than the number in actions.yaml (Improved Frostbolt
    // casts in 2.3s where the file says 3.0), and holding the character still
    // for the difference wastes it on every single cast.
    if (a.isCasting && !b.isCasting && ctx_.actions.isCasting())
    {
        ctx_.log.debug(logcat::Action, "cast finished in the client; releasing the estimated cast timer");
        ctx_.actions.cancelCasting();
    }

    if (prev.targetGuid != cur.targetGuid)
    {
        if (const UnitState* t = cur.target())
        {
            ctx_.log.debug(logcat::Target, "target -> {} {} hp={:.0f}%", t->name, formatGuid(t->guid), t->healthPercent());
        }
        else if (cur.targetGuid == 0)
        {
            ctx_.log.debug(logcat::Target, "target cleared");
        }

        ctx_.events.publish(TargetChangedEvent{prev.targetGuid, cur.targetGuid});
    }
}

void BotEngine::tick(const SnapshotObserver& beforeBehaviors)
{
    ++ticks_;
    std::optional<GameSnapshot> snapshot = provider_->capture();

    if (!snapshot)
    {
        if (!waitingForGame_)
        {
            ctx_.log.warn(logcat::Engine, "no game state from '{}' - waiting", provider_->name());
            waitingForGame_ = true;

            if (current_)
            {
                current_->onExit(ctx_);
            }

            current_ = nullptr;
            ctx_.actions.releaseAll();
        }

        return;
    }

    if (waitingForGame_)
    {
        ctx_.log.info(logcat::Engine, "game state available");
        waitingForGame_ = false;
    }

    if (ctx_.navigator.provider().usesClientGeometry())
    {
        if (!last_ || last_->mapId != snapshot->mapId)
        {
            ctx_.navigator.stop();
        }

        ctx_.navigator.provider().setMap(snapshot->mapId);
    }

    if (observer_)
    {
        observer_(*snapshot);
    }

    combatTracker_.update(*snapshot, ctx_);
    ctx_.buffTracker.update(*snapshot, ctx_);

    if (last_)
    {
        detectTransitions(*last_, *snapshot);
    }

    if (snapshot->spellNotReadyErrors > 0)
    {
        ctx_.actions.spellNotReady();
    }

    if (snapshot->spellOutOfReachErrors > 0)
    {
        ctx_.actions.spellOutOfReach();
    }

    if (snapshot->spellMovingErrors > 0)
    {
        ctx_.movement.stop();
        ctx_.actions.releaseAll();
        ctx_.log.warn(logcat::Action, "spell failed while moving ('Can't do that while moving') - stopped movement");
        if (const auto last = ctx_.actions.lastAction())
        {
            ctx_.actions.clearCooldown(*last);
        }
    }

    if (beforeBehaviors)
    {
        beforeBehaviors(*snapshot);
    }

    if (state_ == EngineState::Paused && !manualTravel_)
    {
        if (current_)
        {
            current_->onExit(ctx_);
            current_ = nullptr;
            ctx_.movement.stop();
            ctx_.actions.releaseAll();
            ctx_.log.info(logcat::State, "paused");
        }

        last_ = std::move(snapshot);
        return;
    }

    if (manualTravel_)
    {
        if (!snapshot->isDead() && !snapshot->attackers().empty())
        {
            if (!travelDefense_)
            {
                ctx_.navigator.stop();
            }

            travelDefense_ = true;
        }
        else if (snapshot->isDead() || ctx_.combat.engaged == 0)
        {
            travelDefense_ = false;
        }
    }

    IBotBehavior* next = nullptr;

    for (const auto& b : behaviors_)
    {
        if (manualTravel_ && (!travelDefense_ || (b->name() != "Combat" && b->name() != "Emergency")))
        {
            continue;
        }

        if (b->canRun(*snapshot, ctx_))
        {
            next = b.get();
            break;
        }
    }

    bool preparedHandoff = false;

    if (current_ && next && current_ != next && current_->name() == "Navigation")
    {
        if (const auto destination = next->approachDestination(*snapshot, ctx_))
        {
            preparedHandoff = ctx_.navigator.preparePath(*snapshot, *destination);
        }
    }

    switchTo(next, *snapshot);

    if (current_)
    {
        const auto result = current_->tick(*snapshot, ctx_);

        // Target selection itself does not steer. Start the approach now,
        // using the route prepared while Navigation still owned movement.
        if (preparedHandoff && current_->name() == "Pull")
        {
            auto* combat = findBehavior("Combat");

            if (result == BehaviorStatus::Success && combat && combat->canRun(*snapshot, ctx_))
            {
                switchTo(combat, *snapshot);
                current_->tick(*snapshot, ctx_);
            }
            else
            {
                ctx_.navigator.stop();
            }
        }
    }

    if (!manualTravel_)
    {
        ctx_.buffTracker.tryApplyInstantBuff(*snapshot, ctx_, current_);
    }

    last_ = std::move(snapshot);
}

void BotEngine::run(std::stop_token stop)
{
    if (ctx_.obstacles)
    {
        ctx_.obstacles->setStopToken(stop);
    }

    SteadyClock wall;

    while (!stop.stop_requested())
    {
        const TimePoint start = wall.now();
        tick();
        const Duration elapsed = wall.now() - start;

        if (elapsed < ctx_.config.tickInterval)
        {
            std::this_thread::sleep_for(ctx_.config.tickInterval - elapsed);
        }
    }

    shutdown();
}

void BotEngine::shutdown()
{
    if (current_)
    {
        current_->onExit(ctx_);
    }

    current_ = nullptr;
    ctx_.buffTracker.reset();
    ctx_.movement.stop();
    ctx_.actions.releaseAll();
}

} // namespace og
