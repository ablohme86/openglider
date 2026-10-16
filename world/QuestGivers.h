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
 *  File: QuestGivers.h
 *
 *  Desc:
 *  Declarations and interface for QuestGivers (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace og
{

class SqliteDb;

// Which NPC hands out and takes back which quest, from Questie's Classic
// database (data/quests/quests.json and npcs.json, made by
// tools/questie_export.py). The client only shows that an NPC has a quest;
// this names it.
class QuestGivers
{
public:
    struct Quest
    {
        std::uint32_t id = 0;
        std::string name;
        int minLevel = 0;
        int level = 0;
        std::uint32_t races = 0;              // bit (race - 1); 0 for every race
        std::uint32_t classes = 0;            // bit (class - 1); 0 for every class
        std::vector<std::uint32_t> preGroup;  // all of them done first
        std::vector<std::uint32_t> preSingle; // one of them done first
        std::vector<std::uint32_t> exclusiveTo;
        std::uint32_t next = 0; // the chain's next quest: once it is taken, this one is not offered
        std::vector<std::uint32_t> starterItems;
    };

    // The character asking.
    struct Character
    {
        int level = 0;
        int race = 0;
        int playerClass = 0;
        std::vector<std::uint32_t> questLog;
        // Quests known to be done besides what the log tells (handed in this
        // session); may be null.
        const std::unordered_set<std::uint32_t>* done = nullptr;
        // Strict: a quest that needs others first is offered only when they
        // are known to be done. Otherwise they count as done unless the log
        // says against it - right for an NPC showing a yellow !, wrong for one
        // out of sight: Milly Osworth was walked to for Milly's Harvest, whose
        // lead-in quest the character never had.
        bool strict = false;
        // Whatever the data says of the races and classes a quest is for: the
        // window open in front of the character tells (A Scribbled Letter is
        // a warrior's in the data, offered to a human hunter on WoW Forever).
        bool anyRaceOrClass = false;
    };

    bool loadFromJson(std::string_view quests, std::string_view npcs, std::string* error = nullptr);
    bool loadFromFiles(const std::string& quests, const std::string& npcs, std::string* error = nullptr);
    bool loadFromDb(const SqliteDb& db, std::string* error = nullptr);
    bool empty() const
    {
        return quests_.empty();
    }

    const Quest* find(std::uint32_t id) const;
    // The quests this NPC offers the character: for its race, class and level,
    // not in the log, and not done - as far as the log and Character::done
    // tell, since what was handed in before is not known: the quests the
    // log's quests need first count as done.
    std::vector<const Quest*> offers(std::uint32_t npcEntry, const Character& who) const;
    // The quest an item starts for the character, if eligible and not in log/done.
    const Quest* itemOffer(std::uint32_t itemId, const Character& who) const;
    // The quests in the character's log this NPC takes back.
    std::vector<const Quest*> takesBack(std::uint32_t npcEntry, const Character& who) const;

private:
    std::unordered_set<std::uint32_t> doneBefore(const std::vector<std::uint32_t>& questLog) const;

    std::unordered_map<std::uint32_t, Quest> quests_;
    struct Npc
    {
        std::vector<std::uint32_t> starts;
        std::vector<std::uint32_t> ends;
    };
    std::unordered_map<std::uint32_t, Npc> npcs_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> itemStarts_; // item -> quests it starts
};

} // namespace og
