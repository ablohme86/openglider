/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-30
 *  File: QuestGivers.cpp
 *
 *  Desc:
 *  Implementation of QuestGivers (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/QuestGivers.h"

#include "og/common/Json.h"
#include "og/common/Sqlite.h"

#include <functional>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace og
{

namespace
{

std::uint32_t idOf(const std::string& key)
{
    std::uint32_t id = 0;
    std::from_chars(key.data(), key.data() + key.size(), id);
    return id;
}

std::vector<std::uint32_t> idsOf(const Json& j, std::string_view key)
{
    std::vector<std::uint32_t> out;

    if (const Json* list = j.find(key); list && list->isArray())
    {
        for (const auto& v : list->asArray())
        {
            out.push_back(static_cast<std::uint32_t>(v.asInt()));
        }
    }

    return out;
}

bool contains(const std::vector<std::uint32_t>& list, std::uint32_t id)
{
    return std::find(list.begin(), list.end(), id) != list.end();
}

} // namespace

bool QuestGivers::loadFromJson(std::string_view questsText, std::string_view npcsText, std::string* error)
{
    try
    {
        const Json questsJson = Json::parse(questsText);
        const Json npcsJson = Json::parse(npcsText);
        const Json* quests = questsJson.find("quests");
        const Json* npcs = npcsJson.find("npcs");

        if (!quests || !quests->isObject() || !npcs || !npcs->isObject())
        {
            if (error)
            {
                *error = "expected a \"quests\" and an \"npcs\" object";
            }

            return false;
        }

        quests_.clear();
        npcs_.clear();
        itemStarts_.clear();
        const auto number = [](const Json& j, std::string_view key)
        {
            const Json* v = j.find(key);
            return v ? v->asInt() : 0;
        };

        for (const auto& [key, q] : quests->asObject())
        {
            Quest quest;
            quest.id = idOf(key);
            quest.name = q.find("name") ? q.find("name")->asString("") : "";
            quest.minLevel = static_cast<int>(number(q, "minLevel"));
            quest.level = static_cast<int>(number(q, "level"));
            quest.races = static_cast<std::uint32_t>(number(q, "races"));
            quest.classes = static_cast<std::uint32_t>(number(q, "classes"));
            quest.preGroup = idsOf(q, "preGroup");
            quest.preSingle = idsOf(q, "preSingle");
            quest.exclusiveTo = idsOf(q, "exclusiveTo");
            quest.next = static_cast<std::uint32_t>(number(q, "next"));
            quest.starterItems = idsOf(q, "starterItems");

            if (quest.id)
            {
                for (const auto itemId : quest.starterItems)
                {
                    itemStarts_[itemId].push_back(quest.id);
                }

                quests_.emplace(quest.id, std::move(quest));
            }
        }

        for (const auto& [key, n] : npcs->asObject())
        {
            if (const auto id = idOf(key))
            {
                npcs_[id] = Npc{idsOf(n, "questStarts"), idsOf(n, "questEnds")};
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

bool QuestGivers::loadFromDb(const SqliteDb& db, std::string* error)
{
    quests_.clear();
    npcs_.clear();
    itemStarts_.clear();

    try
    {
        auto& mutableDb = const_cast<SqliteDb&>(db);

        // 1. Quests
        auto qStmt = mutableDb.prepare("SELECT id, name, min_level, level, races, classes, next_quest FROM quests");
        while (qStmt.step())
        {
            Quest q;
            q.id = qStmt.columnUint32(0);
            q.name = qStmt.columnString(1);
            q.minLevel = qStmt.columnInt(2);
            q.level = qStmt.columnInt(3);
            q.races = qStmt.columnUint32(4);
            q.classes = qStmt.columnUint32(5);
            q.next = qStmt.columnUint32(6);
            if (q.id)
            {
                quests_.emplace(q.id, std::move(q));
            }
        }

        // 2. Pre-groups
        auto pgStmt = mutableDb.prepare("SELECT quest_id, required_quest_id FROM quest_pre_group");
        while (pgStmt.step())
        {
            if (auto it = quests_.find(pgStmt.columnUint32(0)); it != quests_.end())
            {
                it->second.preGroup.push_back(pgStmt.columnUint32(1));
            }
        }

        // 3. Pre-singles
        auto psStmt = mutableDb.prepare("SELECT quest_id, required_quest_id FROM quest_pre_single");
        while (psStmt.step())
        {
            if (auto it = quests_.find(psStmt.columnUint32(0)); it != quests_.end())
            {
                it->second.preSingle.push_back(psStmt.columnUint32(1));
            }
        }

        // 4. Exclusive
        auto exStmt = mutableDb.prepare("SELECT quest_id, exclusive_quest_id FROM quest_exclusive");
        while (exStmt.step())
        {
            if (auto it = quests_.find(exStmt.columnUint32(0)); it != quests_.end())
            {
                it->second.exclusiveTo.push_back(exStmt.columnUint32(1));
            }
        }

        // 5. Starter items
        auto siStmt = mutableDb.prepare("SELECT quest_id, item_id FROM quest_starter_items");
        while (siStmt.step())
        {
            const auto qid = siStmt.columnUint32(0);
            const auto iid = siStmt.columnUint32(1);
            if (auto it = quests_.find(qid); it != quests_.end())
            {
                it->second.starterItems.push_back(iid);
            }
            itemStarts_[iid].push_back(qid);
        }

        // 6. NPC starters & enders
        auto qsStmt = mutableDb.prepare("SELECT npc_id, quest_id FROM quest_starters");
        while (qsStmt.step())
        {
            npcs_[qsStmt.columnUint32(0)].starts.push_back(qsStmt.columnUint32(1));
        }

        auto qeStmt = mutableDb.prepare("SELECT npc_id, quest_id FROM quest_enders");
        while (qeStmt.step())
        {
            npcs_[qeStmt.columnUint32(0)].ends.push_back(qeStmt.columnUint32(1));
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

bool QuestGivers::loadFromFiles(const std::string& quests, const std::string& npcs, std::string* error)
{
    if (quests.ends_with(".db") || quests.ends_with(".sqlite"))
    {
        SqliteDb db;
        if (db.openReadOnly(quests, error))
        {
            return loadFromDb(db, error);
        }
        return false;
    }

    const auto read = [error](const std::string& path, std::string& text)
    {
        std::ifstream file(path);

        if (!file.is_open())
        {
            if (error)
            {
                *error = "could not open " + path;
            }

            return false;
        }

        std::stringstream ss;
        ss << file.rdbuf();
        text = ss.str();
        return true;
    };
    std::string questsText, npcsText;
    return read(quests, questsText) && read(npcs, npcsText) && loadFromJson(questsText, npcsText, error);
}

const QuestGivers::Quest* QuestGivers::find(std::uint32_t id) const
{
    const auto it = quests_.find(id);
    return it == quests_.end() ? nullptr : &it->second;
}

std::unordered_set<std::uint32_t> QuestGivers::doneBefore(const std::vector<std::uint32_t>& questLog) const
{
    std::unordered_set<std::uint32_t> done;
    std::vector<std::uint32_t> todo = questLog;

    while (!todo.empty())
    {
        const Quest* q = find(todo.back());
        todo.pop_back();

        if (!q)
        {
            continue;
        }

        // One of preSingle is enough, so none of them is known to be done -
        // unless there is only the one.
        auto needed = q->preGroup;

        if (q->preSingle.size() == 1)
        {
            needed.push_back(q->preSingle.front());
        }

        for (const auto pre : needed)
        {
            if (done.insert(pre).second)
            {
                todo.push_back(pre);
            }
        }
    }

    return done;
}

std::vector<const QuestGivers::Quest*> QuestGivers::offers(std::uint32_t npcEntry, const Character& who) const
{
    std::vector<const Quest*> out;
    const auto npc = npcs_.find(npcEntry);

    if (npc == npcs_.end())
    {
        return out;
    }

    const auto done = doneBefore(who.questLog);
    const auto knownDone = [&](std::uint32_t id) { return done.contains(id) || (who.done && who.done->contains(id)); };
    const auto taken = [&](std::uint32_t id) { return knownDone(id) || contains(who.questLog, id); };
    // A quest needed first that the data does not have cannot be waited for.
    const auto preDone = [&](std::uint32_t pre) { return knownDone(pre) || !find(pre); };
    // Still to do, as far as the log tells: in the log, or needing one that
    // is. With part one of The People's Militia in the log, part two is not
    // done either - and part three, which needs it, was offered all the same,
    // by the name of the part being worked on.
    std::unordered_set<std::uint32_t> walking;
    std::function<bool(std::uint32_t)> pending = [&](std::uint32_t id) -> bool
    {
        if (contains(who.questLog, id))
        {
            return true;
        }

        const Quest* q = find(id);

        if (!q || knownDone(id) || !walking.insert(id).second)
        {
            return false;
        }

        const bool waits = std::any_of(q->preGroup.begin(), q->preGroup.end(), pending) || (!q->preSingle.empty() && std::all_of(q->preSingle.begin(), q->preSingle.end(), pending));
        walking.erase(id);
        return waits;
    };

    for (const auto id : npc->second.starts)
    {
        const Quest* q = find(id);

        if (!q || taken(id))
        {
            continue;
        }

        if (q->minLevel > who.level)
        {
            continue;
        }

        if (!who.anyRaceOrClass && q->races && who.race > 0 && !(q->races & (1u << (who.race - 1))))
        {
            continue;
        }

        if (!who.anyRaceOrClass && q->classes && who.playerClass > 0 && !(q->classes & (1u << (who.playerClass - 1))))
        {
            continue;
        }

        if (q->next && taken(q->next))
        {
            continue;
        }

        if (std::any_of(q->exclusiveTo.begin(), q->exclusiveTo.end(), taken))
        {
            continue;
        }

        if (who.strict)
        {
            // What it needs first is known to be done.
            if (!std::all_of(q->preGroup.begin(), q->preGroup.end(), preDone))
            {
                continue;
            }

            if (!q->preSingle.empty() && !std::any_of(q->preSingle.begin(), q->preSingle.end(), preDone))
            {
                continue;
            }
        }
        else
        {
            // What it needs first is done, or the log says nothing against it.
            if (std::any_of(q->preGroup.begin(), q->preGroup.end(), pending))
            {
                continue;
            }

            if (!q->preSingle.empty() && std::all_of(q->preSingle.begin(), q->preSingle.end(), pending))
            {
                continue;
            }
        }

        out.push_back(q);
    }

    return out;
}

const QuestGivers::Quest* QuestGivers::itemOffer(std::uint32_t itemId, const Character& who) const
{
    const auto it = itemStarts_.find(itemId);

    if (it == itemStarts_.end())
    {
        return nullptr;
    }

    const auto done = doneBefore(who.questLog);
    const auto knownDone = [&](std::uint32_t id) { return done.contains(id) || (who.done && who.done->contains(id)); };
    const auto taken = [&](std::uint32_t id) { return knownDone(id) || contains(who.questLog, id); };
    const auto preDone = [&](std::uint32_t pre) { return knownDone(pre) || !find(pre); };
    // Still to do, as far as the log tells - as in offers().
    std::unordered_set<std::uint32_t> walking;
    std::function<bool(std::uint32_t)> pending = [&](std::uint32_t id) -> bool
    {
        if (contains(who.questLog, id))
        {
            return true;
        }

        const Quest* q = find(id);

        if (!q || knownDone(id) || !walking.insert(id).second)
        {
            return false;
        }

        const bool waits = std::any_of(q->preGroup.begin(), q->preGroup.end(), pending) || (!q->preSingle.empty() && std::all_of(q->preSingle.begin(), q->preSingle.end(), pending));
        walking.erase(id);
        return waits;
    };

    for (const auto id : it->second)
    {
        const Quest* q = find(id);

        if (!q || taken(id))
        {
            continue;
        }

        if (q->minLevel > who.level)
        {
            continue;
        }

        if (!who.anyRaceOrClass && q->races && who.race > 0 && !(q->races & (1u << (who.race - 1))))
        {
            continue;
        }

        if (!who.anyRaceOrClass && q->classes && who.playerClass > 0 && !(q->classes & (1u << (who.playerClass - 1))))
        {
            continue;
        }

        if (q->next && taken(q->next))
        {
            continue;
        }

        if (std::any_of(q->exclusiveTo.begin(), q->exclusiveTo.end(), taken))
        {
            continue;
        }

        if (who.strict)
        {
            if (!std::all_of(q->preGroup.begin(), q->preGroup.end(), preDone))
            {
                continue;
            }

            if (!q->preSingle.empty() && !std::any_of(q->preSingle.begin(), q->preSingle.end(), preDone))
            {
                continue;
            }
        }
        else
        {
            if (std::any_of(q->preGroup.begin(), q->preGroup.end(), pending))
            {
                continue;
            }

            if (!q->preSingle.empty() && std::all_of(q->preSingle.begin(), q->preSingle.end(), pending))
            {
                continue;
            }
        }

        return q;
    }

    return nullptr;
}

std::vector<const QuestGivers::Quest*> QuestGivers::takesBack(std::uint32_t npcEntry, const Character& who) const
{
    std::vector<const Quest*> out;
    const auto npc = npcs_.find(npcEntry);

    if (npc == npcs_.end())
    {
        return out;
    }

    for (const auto id : npc->second.ends)
    {
        if (contains(who.questLog, id))
        {
            if (const Quest* q = find(id))
            {
                out.push_back(q);
            }
        }
    }

    return out;
}

} // namespace og
