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
 *  File: StuckRecovery.cpp
 *
 *  Desc:
 *  Implementation of StuckRecovery (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/StuckRecovery.h"

#include "og/common/MathUtil.h"

#include <algorithm>

namespace og
{

std::string_view toString(RecoveryStage stage)
{
    switch (stage)
    {
    case RecoveryStage::Jump:
        return "Jump";
    case RecoveryStage::StrafeLeft:
        return "StrafeLeft";
    case RecoveryStage::StrafeRight:
        return "StrafeRight";
    case RecoveryStage::Turn45:
        return "Turn45";
    case RecoveryStage::Reverse:
        return "Reverse";
    case RecoveryStage::Recalculate:
        return "Recalculate";
    case RecoveryStage::BlacklistNode:
        return "BlacklistNode";
    case RecoveryStage::Fallback:
        return "Fallback";
    }

    return "?";
}

StuckRecovery::StuckRecovery(MovementController& movement, const IClock& clock, Logger& log) : movement_(movement), clock_(clock), log_(log)
{
}

void StuckRecovery::begin(const PlayerState& player)
{
    const TimePoint now = clock_.now();

    if (now - lastBegin_ > escalationReset)
    {
        nextStage_ = 0;
    }

    lastBegin_ = now;

    constexpr int kLast = static_cast<int>(RecoveryStage::Fallback);
    stage_ = static_cast<RecoveryStage>(std::min(nextStage_, kLast));
    nextStage_ = std::min(nextStage_ + 1, kLast);
    active_ = true;
    started_ = now;
    faced_ = false;
    log_.warn(logcat::Stuck, "recovery stage {} at {}", toString(stage_), player.position);

    movement_.stopTurning();

    if (stage_ == RecoveryStage::Turn45)
    {
        // Alternate direction so repeated attempts explore both sides.
        targetHeading_ = normalizeHeading(player.heading + degToRad(nextStage_ % 2 ? 45.0f : -45.0f));
    }

    holdStageKeys();

    if (stage_ == RecoveryStage::Jump)
    {
        movement_.jump();
    }
}

void StuckRecovery::holdStageKeys()
{
    // Only what the stage does not use is let go of: W up and straight down
    // again is a tap, and a key just let go of cannot come back at once
    // (MovementController), so each tick asks again until it is down.
    const bool forward = stage_ == RecoveryStage::Jump || stage_ == RecoveryStage::StrafeLeft || stage_ == RecoveryStage::StrafeRight || (stage_ == RecoveryStage::Turn45 && faced_);
    movement_.setForward(forward);
    movement_.setBackward(stage_ == RecoveryStage::Reverse);
    movement_.setStrafeLeft(stage_ == RecoveryStage::StrafeLeft);
    movement_.setStrafeRight(stage_ == RecoveryStage::StrafeRight);
}

RecoveryResult StuckRecovery::tick(const PlayerState& player)
{
    if (!active_)
    {
        return RecoveryResult::Done;
    }

    const Duration elapsed = clock_.now() - started_;

    auto finish = [&](RecoveryResult r)
    {
        movement_.stop();
        active_ = false;
        return r;
    };

    switch (stage_)
    {
    case RecoveryStage::Jump:
        holdStageKeys();

        if (elapsed >= Duration(700))
        {
            return finish(RecoveryResult::Done);
        }

        break;
    case RecoveryStage::StrafeLeft:
    case RecoveryStage::StrafeRight:
        holdStageKeys();

        if (elapsed >= Duration(900))
        {
            return finish(RecoveryResult::Done);
        }

        break;
    case RecoveryStage::Turn45:
        if (!faced_)
        {
            faced_ = movement_.faceHeading(player, targetHeading_) || elapsed >= Duration(1500);

            if (faced_)
            {
                started_ = clock_.now();
                movement_.stopTurning();
                holdStageKeys();
            }
        }
        else if (elapsed >= Duration(1000))
        {
            return finish(RecoveryResult::Done);
        }
        else
        {
            holdStageKeys();
        }

        break;
    case RecoveryStage::Reverse:
        holdStageKeys();

        if (elapsed >= Duration(1200))
        {
            return finish(RecoveryResult::Done);
        }

        break;
    case RecoveryStage::Recalculate:
        return finish(RecoveryResult::RequestRecalculate);
    case RecoveryStage::BlacklistNode:
        return finish(RecoveryResult::RequestBlacklist);
    case RecoveryStage::Fallback:
        return finish(RecoveryResult::RequestFallback);
    }

    return RecoveryResult::InProgress;
}

} // namespace og
