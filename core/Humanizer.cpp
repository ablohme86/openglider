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
 *  File: Humanizer.cpp
 *
 *  Desc:
 *  Implementation of Humanizer (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/Humanizer.h"
#include "og/navigation/Avoidance.h"

#include "og/core/BotContext.h"
#include "og/state/GameSnapshot.h"

#include <algorithm>

namespace og
{

Humanizer::Humanizer(std::uint32_t seed) : rng_(seed)
{
}

Duration Humanizer::pick(Duration lo, Duration hi)
{
    if (hi < lo)
    {
        std::swap(lo, hi);
    }

    if (lo == hi)
    {
        return lo;
    }

    std::uniform_int_distribution<std::int64_t> dist(lo.count(), hi.count());
    return Duration(dist(rng_));
}

void Humanizer::schedule(const HumanizerConfig& config, TimePoint now)
{
    nextJump_ = now + pick(config.jumpMinInterval, config.jumpMaxInterval);
    nextStrafe_ = now + pick(config.strafeMinInterval, config.strafeMaxInterval);
    nextPause_ = now + pick(config.pauseMinInterval, config.pauseMaxInterval);

    // Kept from one stretch of travel to the next: every fight and every
    // corpse resets the humanizer, and drawn anew each time, a break 25
    // minutes off never came.
    if (nextBreak_ == TimePoint{})
    {
        nextBreak_ = now + pick(config.breakMinInterval, config.breakMaxInterval);
    }

    scheduled_ = true;
}

void Humanizer::reset(BotContext& ctx)
{
    if (active_ == Event::Strafe)
    {
        ctx.movement.setStrafeLeft(false);
        ctx.movement.setStrafeRight(false);
    }

    active_ = Event::None;
    scheduled_ = false;
}

void Humanizer::applyMovement(BotContext& ctx)
{
    if (ctx.navigator.recovering() || active_ != Event::Strafe)
    {
        return;
    }

    // moveTo() clears both strafe keys every tick, so the hold is re-applied here.
    ctx.movement.setStrafeLeft(strafeLeft_);
    ctx.movement.setStrafeRight(!strafeLeft_);
}

bool Humanizer::tick(const GameSnapshot& s, BotContext& ctx)
{
    const HumanizerConfig& config = ctx.config.humanizer;
    const bool nearObstacle = ctx.obstacles && (!clearOfZones(s.player.position, s.player.position, ctx.obstacles->noGoZones(), 8.0f) || !clearOfZones(s.player.position, s.player.position, ctx.obstacles->avoidZones(), 8.0f));

    if (ctx.navigator.recovering())
    {
        // Do not release recovery's own strafe keys when abandoning an event.
        active_ = Event::None;
        scheduled_ = false;
        return false;
    }

    if (!config.enabled || nearObstacle)
    {
        if (active_ != Event::None || scheduled_)
        {
            reset(ctx);
        }

        return false;
    }

    const TimePoint now = ctx.now();

    if (!scheduled_)
    {
        schedule(config, now);
    }

    // Let an in-flight event run out first.
    if (active_ != Event::None)
    {
        if (now < activeUntil_)
        {
            return active_ == Event::Pause || active_ == Event::Break;
        }

        if (active_ == Event::Strafe)
        {
            ctx.movement.setStrafeLeft(false);
            ctx.movement.setStrafeRight(false);
        }

        active_ = Event::None;
    }

    // Anything but quiet travel is a bad moment to act up.
    if (s.inCombat() || !s.attackers().empty() || s.isDead())
    {
        return false;
    }

    if (config.breaks && now >= nextBreak_)
    {
        active_ = Event::Break;
        activeUntil_ = now + pick(config.breakMinDuration, config.breakMaxDuration);
        nextBreak_ = activeUntil_ + pick(config.breakMinInterval, config.breakMaxInterval);
        ctx.movement.stop();
        ctx.log.info(logcat::Move, "humanizer: a break of {} s", (activeUntil_ - now).count() / 1000);
        return true;
    }

    if (config.jumps && now >= nextJump_)
    {
        // Not into a corner of the way: people jump on the open stretches.
        if (const auto& path = ctx.navigator.path().points; ctx.navigator.pathIndex() + 1 < path.size() && distance2D(s.player.position, path[ctx.navigator.pathIndex()]) < config.jumpClearAhead)
        {
            nextJump_ = now + pick(Duration(2000), Duration(6000));
            return false;
        }

        ctx.movement.jump();
        nextJump_ = now + pick(config.jumpMinInterval, config.jumpMaxInterval);
        ctx.log.debug(logcat::Move, "humanizer: jump");
        return false;
    }

    if (config.strafes && now >= nextStrafe_)
    {
        strafeLeft_ = std::uniform_int_distribution<int>(0, 1)(rng_) == 0;
        active_ = Event::Strafe;
        activeUntil_ = now + pick(config.strafeMinDuration, config.strafeMaxDuration);
        nextStrafe_ = activeUntil_ + pick(config.strafeMinInterval, config.strafeMaxInterval);
        ctx.movement.setStrafeLeft(strafeLeft_);
        ctx.movement.setStrafeRight(!strafeLeft_);
        ctx.log.debug(logcat::Move, "humanizer: strafe {} for {} ms", strafeLeft_ ? "left" : "right", (activeUntil_ - now).count());
        return false;
    }

    if (config.pauses && now >= nextPause_)
    {
        active_ = Event::Pause;
        activeUntil_ = now + pick(config.pauseMinDuration, config.pauseMaxDuration);
        nextPause_ = activeUntil_ + pick(config.pauseMinInterval, config.pauseMaxInterval);
        ctx.movement.stop();
        ctx.log.debug(logcat::Move, "humanizer: pause for {} ms", (activeUntil_ - now).count());
        return true;
    }

    return false;
}

} // namespace og
