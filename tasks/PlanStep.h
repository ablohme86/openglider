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
 *  File: PlanStep.h
 *
 *  Desc:
 *  Declarations and interface for PlanStep (task graph definitions,
 *  execution, and validation).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Json.h"
#include "og/common/Vec3.h"
#include "og/state/Types.h"
#include "og/tasks/TaskTypes.h"

#include <string>
#include <string_view>

namespace og
{

enum class PlanStepType
{
    GoTo,
    AcceptQuest,
    CompleteQuestObjective,
    TurnInQuest,
    Grind,
    GatherMining,
    GatherHerbalism,
    Fish,
    Vendor,
    Repair,
    Train,
    UseFlightPath,
    Wait,
    Interact,
    Stop
};

std::string_view planStepTypeToString(PlanStepType t);
PlanStepType planStepTypeFromString(std::string_view s);

struct PlanStep
{
    std::string id;
    PlanStepType type = PlanStepType::Stop;
    std::string description;

    // Navigation / Target position
    Vec3 position{};
    std::string locationName;
    float arrivalDistance = 3.0f;

    // Quest attributes
    int questId = 0;
    int objectiveIndex = -1;

    // Entity targets
    std::string targetName;
    Guid targetGuid = 0;

    // Counts & progress
    int requiredCount = 0;
    int currentCount = 0;

    // Timeouts and limits
    Duration timeout = Duration(300000); // 5 minutes default
    Duration waitDuration = Duration(0);
    int targetLevel = 0;

    std::string describe() const;
    Json toJson() const;
    static PlanStep fromJson(const Json& j);
};

} // namespace og
