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
 *  File: BotEngine.h
 *
 *  Desc:
 *  Declarations and interface for BotEngine (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/core/BotContext.h"
#include "og/core/CombatTracker.h"
#include "og/core/IBotBehavior.h"
#include "og/state/IGameStateProvider.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace og
{

enum class EngineState
{
    Running,
    Paused
};

// The main loop:
//
//   while running:
//       snapshot = provider.capture()
//       update shared trackers, publish transition events
//       run the highest-priority behavior that can run
//       sleep tickInterval
class BotEngine
{
public:
    using SnapshotObserver = std::function<void(const GameSnapshot&)>;

    BotEngine(IGameStateProvider& provider, BotContext& ctx);

    void addBehavior(std::unique_ptr<IBotBehavior> behavior);
    const std::vector<std::unique_ptr<IBotBehavior>>& behaviors() const
    {
        return behaviors_;
    }

    IBotBehavior* findBehavior(std::string_view name) const;

    // One iteration of the loop (used directly by the simulator and tests).
    // beforeBehaviors updates derived state from this tick's snapshot before selection.
    void tick(const SnapshotObserver& beforeBehaviors = {});
    // Blocking loop on the real clock until `stop` is requested.
    void run(std::stop_token stop);
    // Leaves the active behavior and releases every held key.
    void shutdown();

    void pause();
    void resume();
    EngineState state() const
    {
        return state_;
    }

    // A manually requested journey owns navigation, even while paused. Only
    // self-defense and emergency combat actions may interrupt it.
    void setManualTravel(bool active);
    bool manualTravel() const
    {
        return manualTravel_;
    }

    // The last tick had no game state: not in the world, or a loading screen.
    bool waitingForGame() const
    {
        return waitingForGame_;
    }

    void setProvider(IGameStateProvider& provider)
    {
        provider_ = &provider;
    }

    // Called with every captured snapshot (recorder, UI).
    void setSnapshotObserver(SnapshotObserver observer)
    {
        observer_ = std::move(observer);
    }

    const IBotBehavior* current() const
    {
        return current_;
    }

    std::string currentName() const;
    const std::optional<GameSnapshot>& lastSnapshot() const
    {
        return last_;
    }

    std::uint64_t tickCount() const
    {
        return ticks_;
    }

private:
    void detectTransitions(const GameSnapshot& previous, const GameSnapshot& snapshot);
    void switchTo(IBotBehavior* next, const GameSnapshot& snapshot);

    IGameStateProvider* provider_;
    BotContext& ctx_;
    std::vector<std::unique_ptr<IBotBehavior>> behaviors_;
    IBotBehavior* current_ = nullptr;
    CombatTracker combatTracker_;
    std::optional<GameSnapshot> last_;
    SnapshotObserver observer_;
    std::atomic<EngineState> state_{EngineState::Running};
    bool waitingForGame_ = false;
    bool manualTravel_ = false;
    bool travelDefense_ = false;
    std::uint64_t ticks_ = 0;
};

} // namespace og
