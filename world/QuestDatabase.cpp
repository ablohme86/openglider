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
 *  File: QuestDatabase.cpp
 *
 *  Desc:
 *  Implementation of QuestDatabase (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/QuestDatabase.h"

#include <fstream>
#include <sstream>

namespace og
{

void QuestDatabase::addQuest(QuestDefinition quest)
{
    quests_[quest.id] = std::move(quest);
}

const QuestDefinition* QuestDatabase::find(int questId) const
{
    auto it = quests_.find(questId);
    return it != quests_.end() ? &it->second : nullptr;
}

std::vector<const QuestDefinition*> QuestDatabase::findAvailable(int playerLevel, const std::unordered_set<int>& completedQuests, const std::unordered_set<int>& activeQuests, std::string_view zone) const
{
    std::vector<const QuestDefinition*> out;

    for (const auto& [id, q] : quests_)
    {
        // Skip already completed or active
        if (completedQuests.contains(id) || activeQuests.contains(id))
        {
            continue;
        }

        // Level check
        if (playerLevel < q.minLevel)
        {
            continue;
        }

        // Prerequisites check
        bool prereqsMet = true;

        for (int req : q.prerequisites)
        {
            if (!completedQuests.contains(req))
            {
                prereqsMet = false;
                break;
            }
        }

        if (!prereqsMet)
        {
            continue;
        }

        // Zone filter if specified
        if (!zone.empty())
        {
            bool matchesZone = false;

            for (const auto& obj : q.objectives)
            {
                if (obj.zone == zone)
                {
                    matchesZone = true;
                    break;
                }
            }

            if (!matchesZone && !q.objectives.empty())
            {
                continue;
            }
        }

        out.push_back(&q);
    }

    return out;
}

std::vector<const QuestDefinition*> QuestDatabase::allQuests() const
{
    std::vector<const QuestDefinition*> out;
    out.reserve(quests_.size());

    for (const auto& [_, q] : quests_)
    {
        out.push_back(&q);
    }

    return out;
}

bool QuestDatabase::loadFromJson(std::string_view jsonText, std::string* error)
{
    try
    {
        Json j = Json::parse(jsonText);

        if (!j.isArray())
        {
            if (error)
            {
                *error = "Expected JSON array of quests";
            }

            return false;
        }

        for (const auto& qj : j.asArray())
        {
            QuestDefinition q = QuestDefinition::fromJson(qj);

            if (q.id > 0)
            {
                quests_[q.id] = std::move(q);
            }
        }

        return true;
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = e.what();
        }

        return false;
    }
}

bool QuestDatabase::loadFromFile(const std::string& path, std::string* error)
{
    std::ifstream file(path);

    if (!file.is_open())
    {
        if (error)
        {
            *error = "Could not open file: " + path;
        }

        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    return loadFromJson(ss.str(), error);
}

} // namespace og
