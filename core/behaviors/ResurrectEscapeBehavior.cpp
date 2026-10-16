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
 *  File: ResurrectEscapeBehavior.cpp
 *
 *  Desc:
 *  Implementation of ResurrectEscapeBehavior (runs to safety along
 *  planned escape route after resurrecting near hostiles, before resting).
 *
 */
#include "og/common/MathUtil.h"
#include "og/core/behaviors/Behaviors.h"

namespace og
{

bool ResurrectEscapeBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    return ctx.resurrectEscape.has_value() && !s.player.isDeadOrGhost();
}

void ResurrectEscapeBehavior::onEnter(const GameSnapshot&, BotContext& ctx)
{
    // The plan was made as a ghost: the timeout runs from the escape itself,
    // not from however long retrieving the corpse took.
    if (ctx.resurrectEscape)
    {
        ctx.resurrectEscape->startedAt = ctx.now();
    }
}

BehaviorStatus ResurrectEscapeBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.resurrectEscape)
    {
        return BehaviorStatus::Success;
    }

    const TimePoint now = ctx.now();
    const auto& escape = *ctx.resurrectEscape;

    // Timeout after resurrectEscapeTimeout (default 15s) to avoid running forever.
    if (now - escape.startedAt > ctx.config.resurrectEscapeTimeout)
    {
        ctx.log.warn(logcat::Death, "resurrect escape timed out - stopping to rest");
        ctx.resurrectEscape.reset();
        ctx.navigator.stop();
        return BehaviorStatus::Success;
    }

    const float distToDest = distance2D(s.player.position, escape.destination);
    const bool noHostilesNear = s.hostilesNear(s.player.position, ctx.config.corpseSafeRadius).empty();

    // Arrived at destination or far enough away and clear of hostiles:
    if (distToDest <= 2.5f || (noHostilesNear && distance2D(s.player.position, escape.origin) >= 20.0f))
    {
        ctx.log.info(logcat::Death, "resurrect escape complete ({:.0f} yd from corpse) - starting rest in safety", distance2D(s.player.position, escape.origin));
        ctx.resurrectEscape.reset();
        ctx.navigator.stop();
        return BehaviorStatus::Success;
    }

    const NavStatus status = ctx.navigator.moveTo(s, escape.destination, 2.0f);
    if (status == NavStatus::Arrived)
    {
        ctx.log.info(logcat::Death, "resurrect escape arrived at {} - starting rest in safety", escape.destination);
        ctx.resurrectEscape.reset();
        ctx.navigator.stop();
        return BehaviorStatus::Success;
    }

    if (status == NavStatus::Unreachable)
    {
        ctx.log.warn(logcat::Death, "escape route unreachable - stopping here to rest");
        ctx.resurrectEscape.reset();
        ctx.navigator.stop();
        return BehaviorStatus::Success;
    }

    return BehaviorStatus::Running;
}

void ResurrectEscapeBehavior::onExit(BotContext& ctx)
{
    ctx.navigator.stop();
}

} // namespace og
