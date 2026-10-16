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
 *  File: QuestDatabase.h
 *
 *  Desc:
 *  Declarations and interface for QuestDatabase (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/world/QuestTypes.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace og
{

class QuestDatabase
{
public:
    QuestDatabase() = default;

    void addQuest(QuestDefinition quest);
    const QuestDefinition* find(int questId) const;

    std::vector<const QuestDefinition*> findAvailable(int playerLevel, const std::unordered_set<int>& completedQuests, const std::unordered_set<int>& activeQuests = {}, std::string_view zone = {}) const;

    std::vector<const QuestDefinition*> allQuests() const;

    bool loadFromJson(std::string_view jsonText, std::string* error = nullptr);
    bool loadFromFile(const std::string& path, std::string* error = nullptr);

    std::size_t size() const
    {
        return quests_.size();
    }

    bool empty() const
    {
        return quests_.empty();
    }

    void clear()
    {
        quests_.clear();
    }

private:
    std::unordered_map<int, QuestDefinition> quests_;
};

} // namespace og
