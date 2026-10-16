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

#include <cmath>
#include <optional>

namespace og
{

namespace
{

// Where to run on to from `from`: walkable ground 25-45 yd off, away from the
// hostiles near, the one furthest from any of them.
std::optional<Vec3> fleeSpot(const Vec3& from, const GameSnapshot& s, BotContext& ctx)
{
    Vec3 center{};
    int count = 0;

    for (const auto* u : s.hostilesNear(from, 40.0f))
    {
        center = center + u->position;
        ++count;
    }

    const Vec3 away = count > 0 ? from - center * (1.0f / static_cast<float>(count)) : Vec3{1, 0, 0};
    const float baseAngle = std::atan2(away.y, away.x);
    std::optional<Vec3> best;
    float bestScore = -1.0f;

    for (const float dist : {25.0f, 35.0f, 45.0f})
    {
        for (int i = -6; i <= 6; ++i)
        {
            const float a = baseAngle + static_cast<float>(i) * (kPi / 8.0f);
            Vec3 candidate = from + Vec3{std::cos(a) * dist, std::sin(a) * dist, 0.0f};

            if (ctx.areaBlacklist.contains(candidate) || !ctx.navigator.canStandAt(candidate))
            {
                continue;
            }

            if (ctx.navigator.provider().usesClientGeometry())
            {
                const auto walkable = ctx.navigator.provider().findWalkablePoint(candidate, 3.0f, 0.5f);

                if (!walkable)
                {
                    continue;
                }

                candidate = *walkable;
            }

            float nearest = 1000.0f;

            for (const auto& u : s.units)
            {
                if (u.isAlive() && u.reaction == Reaction::Hostile && !u.isCritter)
                {
                    nearest = std::min(nearest, distance2D(u.position, candidate));
                }
            }

            if (nearest > bestScore)
            {
                bestScore = nearest;
                best = candidate;
            }
        }
    }

    return best;
}

} // namespace

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

    constexpr int kMaxReplans = 4;
    const TimePoint now = ctx.now();
    auto& escape = *ctx.resurrectEscape;
    const auto stop = [&]
    {
        ctx.resurrectEscape.reset();
        ctx.navigator.stop();
        return BehaviorStatus::Success;
    };

    // Not running for ever: wherever it got to, rested at (a mob that comes
    // is fought, Combat being above this).
    if (now - escape.startedAt > ctx.config.resurrectEscapeTimeout)
    {
        ctx.log.warn(logcat::Death, "resurrect escape: no place clear of hostiles in {:.0f}s - resting here", toSeconds(now - escape.startedAt));
        return stop();
    }

    const bool noHostilesNear = s.hostilesNear(s.player.position, ctx.config.corpseSafeRadius).empty();

    // Far enough and clear of hostiles: done.
    if (noHostilesNear && distance2D(s.player.position, escape.origin) >= 20.0f)
    {
        ctx.log.info(logcat::Death, "resurrect escape complete ({:.0f} yd from corpse) - starting rest in safety", distance2D(s.player.position, escape.origin));
        return stop();
    }

    const bool there = distance2D(s.player.position, escape.destination) <= 2.5f;
    const NavStatus status = there ? NavStatus::Arrived : ctx.navigator.moveTo(s, escape.destination, 2.0f);

    if (status != NavStatus::Arrived && status != NavStatus::Unreachable)
    {
        return BehaviorStatus::Running;
    }

    if (noHostilesNear)
    {
        ctx.log.info(logcat::Death, "resurrect escape arrived at {} - starting rest in safety", escape.destination);
        return stop();
    }

    // There, or no way there, and hostiles still near: on, away from them.
    if (status == NavStatus::Unreachable)
    {
        ctx.areaBlacklist.add(escape.destination, 3.0f, Duration(60000), "no way there to escape");
    }

    const auto next = escape.replans < kMaxReplans ? fleeSpot(s.player.position, s, ctx) : std::nullopt;

    if (!next)
    {
        ctx.log.warn(logcat::Death, "resurrect escape: {} and hostiles still near, no other way - resting here", status == NavStatus::Unreachable ? "no way on" : "there");
        return stop();
    }

    ++escape.replans;
    ctx.log.info(logcat::Death, "resurrect escape: hostiles still near {} - running on to {} ({}/{})", status == NavStatus::Unreachable ? "and no way there" : "there", *next, escape.replans, kMaxReplans);
    escape.destination = *next;
    ctx.navigator.stop();
    return BehaviorStatus::Running;
}

void ResurrectEscapeBehavior::onExit(BotContext& ctx)
{
    ctx.navigator.stop();
}

} // namespace og
