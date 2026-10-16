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
 *  File: StuckRecovery.h
 *
 *  Desc:
 *  Declarations and interface for StuckRecovery (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Log.h"
#include "og/navigation/MovementController.h"

#include <string_view>

namespace og
{

// Escalating recovery stages. Consecutive stuck events move to the next stage;
// being free for `escalationReset` starts over at Jump.
enum class RecoveryStage
{
    Jump,
    StrafeLeft,
    StrafeRight,
    Turn45,
    Reverse,
    Recalculate,   // ask the navigator to recompute the path
    BlacklistNode, // ask the caller to skip/blacklist the current node
    Fallback,      // ask the caller to fall back to the previous safe waypoint
};

std::string_view toString(RecoveryStage stage);

enum class RecoveryResult
{
    InProgress,
    Done,
    RequestRecalculate,
    RequestBlacklist,
    RequestFallback
};

class StuckRecovery
{
public:
    StuckRecovery(MovementController& movement, const IClock& clock, Logger& log);

    // Starts the next recovery maneuver.
    void begin(const PlayerState& player);
    RecoveryResult tick(const PlayerState& player);

    bool active() const
    {
        return active_;
    }

    RecoveryStage stage() const
    {
        return stage_;
    }

    void resetEscalation()
    {
        nextStage_ = 0;
    }

    void cancel()
    {
        active_ = false;
        movement_.stop();
    }

    Duration escalationReset{15000};

private:
    // Holds the keys the current stage moves with and lets go of the rest.
    void holdStageKeys();

    MovementController& movement_;
    const IClock& clock_;
    Logger& log_;

    bool active_ = false;
    int nextStage_ = 0;
    RecoveryStage stage_ = RecoveryStage::Jump;
    TimePoint started_{};
    TimePoint lastBegin_{};
    float targetHeading_ = 0.0f;
    bool faced_ = false;
};

} // namespace og
