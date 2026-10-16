/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-12
 *  File: Humanizer.h
 *
 *  Desc:
 *  Declarations and interface for Humanizer (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/core/BotConfig.h"

#include <cstdint>
#include <random>

namespace og
{

struct BotContext;
struct GameSnapshot;

// Sprinkles small off-pattern movements into route travel: a hop, a short
// sidestep, a brief stop. Driven by NavigationBehavior only, so it can never
// interfere with combat, looting, gathering or a corpse run.
//
// Each kind of event has its own randomised timer. Timers are drawn from the
// configured [min, max] range and redrawn after every event, so the intervals
// never settle into a pattern of their own.
class Humanizer
{
public:
    enum class Event : std::uint8_t
    {
        None,
        Jump,
        Strafe,
        Pause,
        Break // a long pause: away from the keys
    };

    explicit Humanizer(std::uint32_t seed = std::random_device{}());

    // Runs the timers and starts at most one event. Returns true while the bot
    // should hold still, which the caller honours by not steering this tick.
    bool tick(const GameSnapshot& s, BotContext& ctx);

    // Re-asserts the keys of an in-flight event. Call after the navigator has
    // steered, since MovementController::moveTo() releases the strafe keys.
    void applyMovement(BotContext& ctx);

    // Drops any in-flight event and releases its keys.
    void reset(BotContext& ctx);

    Event active() const
    {
        return active_;
    }

private:
    Duration pick(Duration lo, Duration hi);
    void schedule(const HumanizerConfig& config, TimePoint now);

    std::mt19937 rng_;
    bool scheduled_ = false;
    TimePoint nextJump_{};
    TimePoint nextStrafe_{};
    TimePoint nextPause_{};
    TimePoint nextBreak_{};

    Event active_ = Event::None;
    TimePoint activeUntil_{};
    bool strafeLeft_ = false;
};

} // namespace og
