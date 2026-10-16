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
 *  File: Events.h
 *
 *  Desc:
 *  Declarations and interface for Events (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/state/Types.h"

#include <cstddef>
#include <string>

// Engine events published on the instance EventBus. Module-specific events
// live with their module: ActionExecutedEvent (actions), StuckDetectedEvent
// (navigation).
namespace og
{

struct BehaviorChangedEvent
{
    std::string from;
    std::string to;
};

struct PlayerDiedEvent
{
    Vec3 position;
};

struct PlayerResurrectedEvent
{
    Vec3 position;
};

struct CombatStartedEvent
{
};

struct CombatEndedEvent
{
    double durationSeconds = 0.0;
};

struct TargetChangedEvent
{
    Guid previous = 0;
    Guid current = 0;
};

struct TargetKilledEvent
{
    Guid guid = 0;
    std::string name;
    Vec3 position;
};

struct WaypointReachedEvent
{
    std::size_t index = 0;
    Vec3 position;
};

struct LootCompletedEvent
{
    Guid corpse = 0;
    bool looted = false;
    bool skinned = false;
};

struct GatherStartedEvent
{
    Guid guid = 0;
    GameObjectKind kind = GameObjectKind::Unknown;
    std::string name;
};

struct GatherCompletedEvent
{
    Guid guid = 0;
    GameObjectKind kind = GameObjectKind::Unknown;
    bool success = false; // Requires an observed increase in bag contents.
    std::string reason;
};

struct ProfileLoadedEvent
{
    std::string name;
};

struct ObjectiveChangedEvent
{
    std::string description; // e.g. "GrindRoute > LootCorpse(0x1A)"
};

} // namespace og
