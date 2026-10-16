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
 *  File: Plan.cpp
 *
 *  Desc:
 *  Implementation of Plan (task graph definitions, execution, and
 *  validation).
 *
 *
 */
#include "og/tasks/Plan.h"

namespace og
{

Json PlanConstraints::toJson() const
{
    Json j = Json::object();
    j["maxDeaths"] = static_cast<int64_t>(maxDeaths);
    j["maxStuckEvents"] = static_cast<int64_t>(maxStuckEvents);
    j["maxDurationMs"] = static_cast<int64_t>(maxDuration.count());
    j["minFreeBagSlots"] = static_cast<int64_t>(minFreeBagSlots);
    j["minDurabilityPercent"] = minDurabilityPercent;
    return j;
}

PlanConstraints PlanConstraints::fromJson(const Json& j)
{
    PlanConstraints c;

    if (j.contains("maxDeaths"))
    {
        c.maxDeaths = static_cast<int>(j["maxDeaths"].asInt());
    }

    if (j.contains("maxStuckEvents"))
    {
        c.maxStuckEvents = static_cast<int>(j["maxStuckEvents"].asInt());
    }

    if (j.contains("maxDurationMs"))
    {
        c.maxDuration = Duration(j["maxDurationMs"].asInt());
    }

    if (j.contains("minFreeBagSlots"))
    {
        c.minFreeBagSlots = static_cast<int>(j["minFreeBagSlots"].asInt());
    }

    if (j.contains("minDurabilityPercent"))
    {
        c.minDurabilityPercent = j["minDurabilityPercent"].asFloat(25.0f);
    }

    return c;
}

Json Plan::toJson() const
{
    Json j = Json::object();

    if (!planId.empty())
    {
        j["planId"] = planId;
    }

    if (!goal.empty())
    {
        j["goal"] = goal;
    }

    if (!reason.empty())
    {
        j["reason"] = reason;
    }

    if (!candidateId.empty())
    {
        j["candidateId"] = candidateId;
    }

    j["confidence"] = confidence;
    j["currentStepIndex"] = static_cast<int64_t>(currentStepIndex);
    j["constraints"] = constraints.toJson();

    Json stepsArr = Json::array();

    for (const auto& step : steps)
    {
        stepsArr.push_back(step.toJson());
    }

    j["steps"] = std::move(stepsArr);
    return j;
}

Plan Plan::fromJson(const Json& j)
{
    Plan p;

    if (j.contains("planId"))
    {
        p.planId = j["planId"].asString();
    }

    if (j.contains("goal"))
    {
        p.goal = j["goal"].asString();
    }

    if (j.contains("reason"))
    {
        p.reason = j["reason"].asString();
    }

    if (j.contains("candidateId"))
    {
        p.candidateId = j["candidateId"].asString();
    }

    if (j.contains("confidence"))
    {
        p.confidence = j["confidence"].asDouble(1.0);
    }

    if (j.contains("currentStepIndex"))
    {
        p.currentStepIndex = static_cast<std::size_t>(j["currentStepIndex"].asInt(0));
    }

    if (j.contains("constraints"))
    {
        p.constraints = PlanConstraints::fromJson(j["constraints"]);
    }

    if (j.contains("steps") && j["steps"].isArray())
    {
        for (const auto& sj : j["steps"].asArray())
        {
            p.steps.push_back(PlanStep::fromJson(sj));
        }
    }

    return p;
}

} // namespace og
