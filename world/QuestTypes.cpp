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
 *  File: QuestTypes.cpp
 *
 *  Desc:
 *  Implementation of QuestTypes (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/QuestTypes.h"

namespace og
{

std::string_view questObjectiveTypeToString(QuestObjectiveType t)
{
    switch (t)
    {
    case QuestObjectiveType::Kill:
        return "Kill";
    case QuestObjectiveType::Collect:
        return "Collect";
    case QuestObjectiveType::Interact:
        return "Interact";
    case QuestObjectiveType::Escort:
        return "Escort";
    case QuestObjectiveType::Explore:
        return "Explore";
    case QuestObjectiveType::Deliver:
        return "Deliver";
    case QuestObjectiveType::TalkToNpc:
        return "TalkToNpc";
    case QuestObjectiveType::UseItem:
        return "UseItem";
    case QuestObjectiveType::Custom:
        return "Custom";
    }

    return "Unknown";
}

QuestObjectiveType questObjectiveTypeFromString(std::string_view s)
{
    if (s == "Kill")
    {
        return QuestObjectiveType::Kill;
    }

    if (s == "Collect")
    {
        return QuestObjectiveType::Collect;
    }

    if (s == "Interact")
    {
        return QuestObjectiveType::Interact;
    }

    if (s == "Escort")
    {
        return QuestObjectiveType::Escort;
    }

    if (s == "Explore")
    {
        return QuestObjectiveType::Explore;
    }

    if (s == "Deliver")
    {
        return QuestObjectiveType::Deliver;
    }

    if (s == "TalkToNpc")
    {
        return QuestObjectiveType::TalkToNpc;
    }

    if (s == "UseItem")
    {
        return QuestObjectiveType::UseItem;
    }

    return QuestObjectiveType::Custom;
}

std::string_view questRuntimeStateToString(QuestRuntimeState s)
{
    switch (s)
    {
    case QuestRuntimeState::Inactive:
        return "Inactive";
    case QuestRuntimeState::Active:
        return "Active";
    case QuestRuntimeState::Complete:
        return "Complete";
    case QuestRuntimeState::Failed:
        return "Failed";
    }

    return "Unknown";
}

QuestRuntimeState questRuntimeStateFromString(std::string_view s)
{
    if (s == "Inactive")
    {
        return QuestRuntimeState::Inactive;
    }

    if (s == "Active")
    {
        return QuestRuntimeState::Active;
    }

    if (s == "Complete")
    {
        return QuestRuntimeState::Complete;
    }

    if (s == "Failed")
    {
        return QuestRuntimeState::Failed;
    }

    return QuestRuntimeState::Inactive;
}

// QuestObjective JSON
Json QuestObjective::toJson() const
{
    Json j = Json::object();
    j["type"] = std::string(questObjectiveTypeToString(type));

    if (!targetName.empty())
    {
        j["targetName"] = targetName;
    }

    if (targetId != 0)
    {
        j["targetId"] = static_cast<int64_t>(targetId);
    }

    j["requiredCount"] = static_cast<int64_t>(requiredCount);

    if (!zone.empty())
    {
        j["zone"] = zone;
    }

    if (!knownLocations.empty())
    {
        Json locs = Json::array();

        for (const auto& loc : knownLocations)
        {
            locs.push_back(Json(Json::Array{loc.x, loc.y, loc.z}));
        }

        j["knownLocations"] = std::move(locs);
    }

    return j;
}

QuestObjective QuestObjective::fromJson(const Json& j)
{
    QuestObjective obj;

    if (j.contains("type"))
    {
        obj.type = questObjectiveTypeFromString(j["type"].asString());
    }

    if (j.contains("targetName"))
    {
        obj.targetName = j["targetName"].asString();
    }

    if (j.contains("targetId"))
    {
        obj.targetId = static_cast<int>(j["targetId"].asInt());
    }

    if (j.contains("requiredCount"))
    {
        obj.requiredCount = static_cast<int>(j["requiredCount"].asInt());
    }

    if (j.contains("zone"))
    {
        obj.zone = j["zone"].asString();
    }

    if (j.contains("knownLocations") && j["knownLocations"].isArray())
    {
        for (const auto& lj : j["knownLocations"].asArray())
        {
            if (lj.isArray() && lj.asArray().size() == 3)
            {
                const auto& arr = lj.asArray();
                obj.knownLocations.push_back(Vec3{arr[0].asFloat(), arr[1].asFloat(), arr[2].asFloat()});
            }
        }
    }

    return obj;
}

// QuestDefinition JSON
Json QuestDefinition::toJson() const
{
    Json j = Json::object();
    j["id"] = static_cast<int64_t>(id);
    j["name"] = name;
    j["minLevel"] = static_cast<int64_t>(minLevel);
    j["recommendedLevel"] = static_cast<int64_t>(recommendedLevel);

    if (!prerequisites.empty())
    {
        Json pre = Json::array();

        for (int p : prerequisites)
        {
            pre.push_back(static_cast<int64_t>(p));
        }

        j["prerequisites"] = std::move(pre);
    }

    if (!followUps.empty())
    {
        Json fol = Json::array();

        for (int f : followUps)
        {
            fol.push_back(static_cast<int64_t>(f));
        }

        j["followUps"] = std::move(fol);
    }

    if (questGiverId != 0)
    {
        j["questGiverId"] = static_cast<int64_t>(questGiverId);
    }

    if (!questGiverName.empty())
    {
        j["questGiverName"] = questGiverName;
    }

    if (startLocation.x != 0.0f || startLocation.y != 0.0f || startLocation.z != 0.0f)
    {
        j["startLocation"] = Json(Json::Array{startLocation.x, startLocation.y, startLocation.z});
    }

    if (turnInNpcId != 0)
    {
        j["turnInNpcId"] = static_cast<int64_t>(turnInNpcId);
    }

    if (!turnInNpcName.empty())
    {
        j["turnInNpcName"] = turnInNpcName;
    }

    if (endLocation.x != 0.0f || endLocation.y != 0.0f || endLocation.z != 0.0f)
    {
        j["endLocation"] = Json(Json::Array{endLocation.x, endLocation.y, endLocation.z});
    }

    if (!objectives.empty())
    {
        Json objs = Json::array();

        for (const auto& obj : objectives)
        {
            objs.push_back(obj.toJson());
        }

        j["objectives"] = std::move(objs);
    }

    if (rewardXp != 0)
    {
        j["rewardXp"] = static_cast<int64_t>(rewardXp);
    }

    if (rewardMoney != 0)
    {
        j["rewardMoney"] = static_cast<int64_t>(rewardMoney);
    }

    if (!tags.empty())
    {
        Json tagArr = Json::array();

        for (const auto& t : tags)
        {
            tagArr.push_back(t);
        }

        j["tags"] = std::move(tagArr);
    }

    return j;
}

QuestDefinition QuestDefinition::fromJson(const Json& j)
{
    QuestDefinition q;

    if (j.contains("id"))
    {
        q.id = static_cast<int>(j["id"].asInt());
    }

    if (j.contains("name"))
    {
        q.name = j["name"].asString();
    }

    if (j.contains("minLevel"))
    {
        q.minLevel = static_cast<int>(j["minLevel"].asInt());
    }

    if (j.contains("recommendedLevel"))
    {
        q.recommendedLevel = static_cast<int>(j["recommendedLevel"].asInt());
    }

    if (j.contains("prerequisites") && j["prerequisites"].isArray())
    {
        for (const auto& p : j["prerequisites"].asArray())
        {
            q.prerequisites.push_back(static_cast<int>(p.asInt()));
        }
    }

    if (j.contains("followUps") && j["followUps"].isArray())
    {
        for (const auto& f : j["followUps"].asArray())
        {
            q.followUps.push_back(static_cast<int>(f.asInt()));
        }
    }

    if (j.contains("questGiverId"))
    {
        q.questGiverId = static_cast<int>(j["questGiverId"].asInt());
    }

    if (j.contains("questGiverName"))
    {
        q.questGiverName = j["questGiverName"].asString();
    }

    if (j.contains("startLocation") && j["startLocation"].isArray() && j["startLocation"].asArray().size() == 3)
    {
        const auto& arr = j["startLocation"].asArray();
        q.startLocation = Vec3{arr[0].asFloat(), arr[1].asFloat(), arr[2].asFloat()};
    }

    if (j.contains("turnInNpcId"))
    {
        q.turnInNpcId = static_cast<int>(j["turnInNpcId"].asInt());
    }

    if (j.contains("turnInNpcName"))
    {
        q.turnInNpcName = j["turnInNpcName"].asString();
    }

    if (j.contains("endLocation") && j["endLocation"].isArray() && j["endLocation"].asArray().size() == 3)
    {
        const auto& arr = j["endLocation"].asArray();
        q.endLocation = Vec3{arr[0].asFloat(), arr[1].asFloat(), arr[2].asFloat()};
    }

    if (j.contains("objectives") && j["objectives"].isArray())
    {
        for (const auto& obj : j["objectives"].asArray())
        {
            q.objectives.push_back(QuestObjective::fromJson(obj));
        }
    }

    if (j.contains("rewardXp"))
    {
        q.rewardXp = static_cast<int>(j["rewardXp"].asInt());
    }

    if (j.contains("rewardMoney"))
    {
        q.rewardMoney = static_cast<int>(j["rewardMoney"].asInt());
    }

    if (j.contains("tags") && j["tags"].isArray())
    {
        for (const auto& t : j["tags"].asArray())
        {
            q.tags.push_back(t.asString());
        }
    }

    return q;
}

// ObjectiveProgress JSON
Json ObjectiveProgress::toJson() const
{
    Json j = Json::object();
    j["target"] = target;
    j["current"] = static_cast<int64_t>(current);
    j["required"] = static_cast<int64_t>(required);
    return j;
}

ObjectiveProgress ObjectiveProgress::fromJson(const Json& j)
{
    ObjectiveProgress p;

    if (j.contains("target"))
    {
        p.target = j["target"].asString();
    }

    if (j.contains("current"))
    {
        p.current = static_cast<int>(j["current"].asInt());
    }

    if (j.contains("required"))
    {
        p.required = static_cast<int>(j["required"].asInt());
    }

    return p;
}

// ActiveQuestState JSON
Json ActiveQuestState::toJson() const
{
    Json j = Json::object();
    j["questId"] = static_cast<int64_t>(questId);
    j["state"] = std::string(questRuntimeStateToString(state));

    Json objs = Json::array();

    for (const auto& o : objectives)
    {
        objs.push_back(o.toJson());
    }

    j["objectives"] = std::move(objs);
    return j;
}

ActiveQuestState ActiveQuestState::fromJson(const Json& j)
{
    ActiveQuestState s;

    if (j.contains("questId"))
    {
        s.questId = static_cast<int>(j["questId"].asInt());
    }

    if (j.contains("state"))
    {
        s.state = questRuntimeStateFromString(j["state"].asString());
    }

    if (j.contains("objectives") && j["objectives"].isArray())
    {
        for (const auto& o : j["objectives"].asArray())
        {
            s.objectives.push_back(ObjectiveProgress::fromJson(o));
        }
    }

    return s;
}

} // namespace og
