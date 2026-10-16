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
 *  File: PlanStep.cpp
 *
 *  Desc:
 *  Implementation of PlanStep (task graph definitions, execution, and
 *  validation).
 *
 *
 */
#include "og/tasks/PlanStep.h"

#include <format>

namespace og
{

std::string_view planStepTypeToString(PlanStepType t)
{
    switch (t)
    {
    case PlanStepType::GoTo:
        return "GoTo";
    case PlanStepType::AcceptQuest:
        return "AcceptQuest";
    case PlanStepType::CompleteQuestObjective:
        return "CompleteQuestObjective";
    case PlanStepType::TurnInQuest:
        return "TurnInQuest";
    case PlanStepType::Grind:
        return "Grind";
    case PlanStepType::GatherMining:
        return "GatherMining";
    case PlanStepType::GatherHerbalism:
        return "GatherHerbalism";
    case PlanStepType::Fish:
        return "Fish";
    case PlanStepType::Vendor:
        return "Vendor";
    case PlanStepType::Repair:
        return "Repair";
    case PlanStepType::Train:
        return "Train";
    case PlanStepType::UseFlightPath:
        return "UseFlightPath";
    case PlanStepType::Wait:
        return "Wait";
    case PlanStepType::Interact:
        return "Interact";
    case PlanStepType::Stop:
        return "Stop";
    }

    return "Unknown";
}

PlanStepType planStepTypeFromString(std::string_view s)
{
    if (s == "GoTo")
    {
        return PlanStepType::GoTo;
    }

    if (s == "AcceptQuest")
    {
        return PlanStepType::AcceptQuest;
    }

    if (s == "CompleteQuestObjective")
    {
        return PlanStepType::CompleteQuestObjective;
    }

    if (s == "TurnInQuest")
    {
        return PlanStepType::TurnInQuest;
    }

    if (s == "Grind")
    {
        return PlanStepType::Grind;
    }

    if (s == "GatherMining")
    {
        return PlanStepType::GatherMining;
    }

    if (s == "GatherHerbalism")
    {
        return PlanStepType::GatherHerbalism;
    }

    if (s == "Fish")
    {
        return PlanStepType::Fish;
    }

    if (s == "Vendor")
    {
        return PlanStepType::Vendor;
    }

    if (s == "Repair")
    {
        return PlanStepType::Repair;
    }

    if (s == "Train")
    {
        return PlanStepType::Train;
    }

    if (s == "UseFlightPath")
    {
        return PlanStepType::UseFlightPath;
    }

    if (s == "Wait")
    {
        return PlanStepType::Wait;
    }

    if (s == "Interact")
    {
        return PlanStepType::Interact;
    }

    if (s == "Stop")
    {
        return PlanStepType::Stop;
    }

    return PlanStepType::Stop;
}

std::string PlanStep::describe() const
{
    std::string s(planStepTypeToString(type));

    switch (type)
    {
    case PlanStepType::GoTo:
        if (!locationName.empty())
        {
            return std::format("GoTo({})", locationName);
        }

        return std::format("GoTo({:.1f}, {:.1f}, {:.1f})", position.x, position.y, position.z);
    case PlanStepType::AcceptQuest:
        return std::format("AcceptQuest(id={}{})", questId, targetName.empty() ? "" : ", from=" + targetName);
    case PlanStepType::CompleteQuestObjective:
        return std::format("CompleteQuestObjective(id={}, obj={}{})", questId, objectiveIndex, targetName.empty() ? "" : ", " + targetName);
    case PlanStepType::TurnInQuest:
        return std::format("TurnInQuest(id={}{})", questId, targetName.empty() ? "" : ", to=" + targetName);
    case PlanStepType::Grind:
        if (targetLevel > 0)
        {
            return std::format("Grind(targetLevel={})", targetLevel);
        }

        return "Grind";
    case PlanStepType::Wait:
        return std::format("Wait({:.1f}s)", toSeconds(waitDuration));
    case PlanStepType::Interact:
        return std::format("Interact({})", targetName.empty() ? formatGuid(targetGuid) : targetName);
    default:
        return s;
    }
}

Json PlanStep::toJson() const
{
    Json j = Json::object();

    if (!id.empty())
    {
        j["id"] = id;
    }

    j["type"] = std::string(planStepTypeToString(type));

    if (!description.empty())
    {
        j["description"] = description;
    }

    if (position.x != 0.0f || position.y != 0.0f || position.z != 0.0f)
    {
        j["position"] = Json(Json::Array{position.x, position.y, position.z});
    }

    if (!locationName.empty())
    {
        j["locationName"] = locationName;
    }

    if (arrivalDistance != 3.0f)
    {
        j["arrivalDistance"] = arrivalDistance;
    }

    if (questId != 0)
    {
        j["questId"] = static_cast<int64_t>(questId);
    }

    if (objectiveIndex >= 0)
    {
        j["objectiveIndex"] = static_cast<int64_t>(objectiveIndex);
    }

    if (!targetName.empty())
    {
        j["targetName"] = targetName;
    }

    if (targetGuid != 0)
    {
        j["targetGuid"] = formatGuid(targetGuid);
    }

    if (requiredCount != 0)
    {
        j["requiredCount"] = static_cast<int64_t>(requiredCount);
    }

    if (currentCount != 0)
    {
        j["currentCount"] = static_cast<int64_t>(currentCount);
    }

    if (waitDuration.count() > 0)
    {
        j["waitMs"] = static_cast<int64_t>(waitDuration.count());
    }

    if (targetLevel != 0)
    {
        j["targetLevel"] = static_cast<int64_t>(targetLevel);
    }

    return j;
}

PlanStep PlanStep::fromJson(const Json& j)
{
    PlanStep step;

    if (j.contains("id"))
    {
        step.id = j["id"].asString();
    }

    if (j.contains("type"))
    {
        step.type = planStepTypeFromString(j["type"].asString());
    }

    if (j.contains("description"))
    {
        step.description = j["description"].asString();
    }

    if (j.contains("position") && j["position"].isArray() && j["position"].asArray().size() == 3)
    {
        const auto& arr = j["position"].asArray();
        step.position = Vec3{arr[0].asFloat(), arr[1].asFloat(), arr[2].asFloat()};
    }

    if (j.contains("locationName"))
    {
        step.locationName = j["locationName"].asString();
    }

    if (j.contains("arrivalDistance"))
    {
        step.arrivalDistance = j["arrivalDistance"].asFloat(3.0f);
    }

    if (j.contains("questId"))
    {
        step.questId = static_cast<int>(j["questId"].asInt());
    }

    if (j.contains("objectiveIndex"))
    {
        step.objectiveIndex = static_cast<int>(j["objectiveIndex"].asInt());
    }

    if (j.contains("targetName"))
    {
        step.targetName = j["targetName"].asString();
    }

    if (j.contains("targetGuid"))
    {
        const auto& val = j["targetGuid"];

        if (val.isInt())
        {
            step.targetGuid = static_cast<Guid>(val.asInt());
        }
        else if (val.isString())
        {
            try
            {
                step.targetGuid = std::stoull(val.asString(), nullptr, 0);
            }
            catch (...)
            {
                step.targetGuid = 0;
            }
        }
    }

    if (j.contains("requiredCount"))
    {
        step.requiredCount = static_cast<int>(j["requiredCount"].asInt());
    }

    if (j.contains("currentCount"))
    {
        step.currentCount = static_cast<int>(j["currentCount"].asInt());
    }

    if (j.contains("waitMs"))
    {
        step.waitDuration = Duration(j["waitMs"].asInt());
    }

    if (j.contains("targetLevel"))
    {
        step.targetLevel = static_cast<int>(j["targetLevel"].asInt());
    }

    return step;
}

} // namespace og
