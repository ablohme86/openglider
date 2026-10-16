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
 *  File: PlannerTypes.cpp
 *
 *  Desc:
 *  Implementation of PlannerTypes (strategic and tactical AI planning
 *  systems).
 *
 *
 */
#include "og/ai/PlannerTypes.h"

namespace og
{

std::string_view botGoalTypeToString(BotGoalType t)
{
    switch (t)
    {
    case BotGoalType::LevelTo:
        return "LevelTo";
    case BotGoalType::CompleteQuest:
        return "CompleteQuest";
    case BotGoalType::CompleteZone:
        return "CompleteZone";
    case BotGoalType::GrindToLevel:
        return "GrindToLevel";
    case BotGoalType::Custom:
        return "Custom";
    }

    return "Unknown";
}

BotGoalType botGoalTypeFromString(std::string_view s)
{
    if (s == "LevelTo")
    {
        return BotGoalType::LevelTo;
    }

    if (s == "CompleteQuest")
    {
        return BotGoalType::CompleteQuest;
    }

    if (s == "CompleteZone")
    {
        return BotGoalType::CompleteZone;
    }

    if (s == "GrindToLevel")
    {
        return BotGoalType::GrindToLevel;
    }

    return BotGoalType::Custom;
}

std::string_view planAdjustmentTypeToString(PlanAdjustmentType t)
{
    switch (t)
    {
    case PlanAdjustmentType::None:
        return "None";
    case PlanAdjustmentType::Continue:
        return "Continue";
    case PlanAdjustmentType::AbortStep:
        return "AbortStep";
    case PlanAdjustmentType::AbortPlan:
        return "AbortPlan";
    case PlanAdjustmentType::ReplacePlan:
        return "ReplacePlan";
    }

    return "Unknown";
}

Json BotGoal::toJson() const
{
    Json j = Json::object();
    j["type"] = std::string(botGoalTypeToString(type));

    if (targetLevel > 0)
    {
        j["targetLevel"] = static_cast<int64_t>(targetLevel);
    }

    if (!targetZone.empty())
    {
        j["targetZone"] = targetZone;
    }

    if (targetQuestId > 0)
    {
        j["targetQuestId"] = static_cast<int64_t>(targetQuestId);
    }

    return j;
}

BotGoal BotGoal::fromJson(const Json& j)
{
    BotGoal g;

    if (j.contains("type"))
    {
        g.type = botGoalTypeFromString(j["type"].asString());
    }

    if (j.contains("targetLevel"))
    {
        g.targetLevel = static_cast<int>(j["targetLevel"].asInt());
    }

    if (j.contains("targetZone"))
    {
        g.targetZone = j["targetZone"].asString();
    }

    if (j.contains("targetQuestId"))
    {
        g.targetQuestId = static_cast<int>(j["targetQuestId"].asInt());
    }

    return g;
}

Json CandidatePlan::toJson() const
{
    Json j = Json::object();
    j["id"] = id;
    j["description"] = description;
    j["score"] = score;
    j["plan"] = plan.toJson();
    return j;
}

CandidatePlan CandidatePlan::fromJson(const Json& j)
{
    CandidatePlan c;

    if (j.contains("id"))
    {
        c.id = j["id"].asString();
    }

    if (j.contains("description"))
    {
        c.description = j["description"].asString();
    }

    if (j.contains("score"))
    {
        c.score = j["score"].asDouble();
    }

    if (j.contains("plan"))
    {
        c.plan = Plan::fromJson(j["plan"]);
    }

    return c;
}

} // namespace og
