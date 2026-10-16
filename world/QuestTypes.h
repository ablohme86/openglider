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
 *  File: QuestTypes.h
 *
 *  Desc:
 *  Declarations and interface for QuestTypes (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/common/Json.h"
#include "og/common/Vec3.h"

#include <string>
#include <string_view>
#include <vector>

namespace og
{

enum class QuestObjectiveType
{
    Kill,
    Collect,
    Interact,
    Escort,
    Explore,
    Deliver,
    TalkToNpc,
    UseItem,
    Custom
};

std::string_view questObjectiveTypeToString(QuestObjectiveType t);
QuestObjectiveType questObjectiveTypeFromString(std::string_view s);

struct QuestObjective
{
    QuestObjectiveType type = QuestObjectiveType::Kill;
    std::string targetName;
    int targetId = 0;
    int requiredCount = 1;
    std::vector<Vec3> knownLocations;
    std::string zone;

    Json toJson() const;
    static QuestObjective fromJson(const Json& j);
};

struct QuestDefinition
{
    int id = 0;
    std::string name;
    int minLevel = 1;
    int recommendedLevel = 1;
    std::vector<int> prerequisites;
    std::vector<int> followUps;
    int questGiverId = 0;
    std::string questGiverName;
    Vec3 startLocation{};
    int turnInNpcId = 0;
    std::string turnInNpcName;
    Vec3 endLocation{};
    std::vector<QuestObjective> objectives;
    int rewardXp = 0;
    int rewardMoney = 0;
    std::vector<std::string> tags;

    Json toJson() const;
    static QuestDefinition fromJson(const Json& j);
};

enum class QuestRuntimeState
{
    Inactive,
    Active,
    Complete,
    Failed
};

std::string_view questRuntimeStateToString(QuestRuntimeState s);
QuestRuntimeState questRuntimeStateFromString(std::string_view s);

struct ObjectiveProgress
{
    std::string target;
    int current = 0;
    int required = 0;

    bool isComplete() const
    {
        return current >= required;
    }

    Json toJson() const;
    static ObjectiveProgress fromJson(const Json& j);
};

struct ActiveQuestState
{
    int questId = 0;
    QuestRuntimeState state = QuestRuntimeState::Active;
    std::vector<ObjectiveProgress> objectives;

    bool isComplete() const
    {
        if (state == QuestRuntimeState::Complete)
        {
            return true;
        }

        if (objectives.empty())
        {
            return false;
        }

        for (const auto& obj : objectives)
        {
            if (!obj.isComplete())
            {
                return false;
            }
        }

        return true;
    }

    Json toJson() const;
    static ActiveQuestState fromJson(const Json& j);
};

} // namespace og
