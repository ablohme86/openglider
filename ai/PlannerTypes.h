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
 *  File: PlannerTypes.h
 *
 *  Desc:
 *  Declarations and interface for PlannerTypes (strategic and tactical
 *  AI planning systems).
 *
 *
 */
#pragma once

#include "og/common/Json.h"
#include "og/tasks/Plan.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

enum class BotGoalType
{
    LevelTo,
    CompleteQuest,
    CompleteZone,
    GrindToLevel,
    Custom
};

std::string_view botGoalTypeToString(BotGoalType t);
BotGoalType botGoalTypeFromString(std::string_view s);

struct BotGoal
{
    BotGoalType type = BotGoalType::LevelTo;
    int targetLevel = 60;
    std::string targetZone;
    int targetQuestId = 0;

    Json toJson() const;
    static BotGoal fromJson(const Json& j);
};

enum class PlanAdjustmentType
{
    None,
    Continue,
    AbortStep,
    AbortPlan,
    ReplacePlan
};

std::string_view planAdjustmentTypeToString(PlanAdjustmentType t);

struct PlanAdjustment
{
    PlanAdjustmentType type = PlanAdjustmentType::None;
    std::string reason;
    std::optional<Plan> newPlan;
};

struct CandidatePlan
{
    std::string id;
    std::string description;
    double score = 0.0;
    Plan plan;

    Json toJson() const;
    static CandidatePlan fromJson(const Json& j);
};

} // namespace og
