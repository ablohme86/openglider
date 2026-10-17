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
 *  File: QuestSource.h
 *
 *  Desc:
 *  Declarations and interface for QuestSource (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/core/QuestTargets.h"
#include "og/world/QuestGivers.h"
#include "og/world/ZoneBounds.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace og
{

// A mount vendor, by its title ("Mechanostrider Merchant", "Ram Breeder"):
// it sells and buys only for those exalted with its race, so a bags-full trip
// to one ends at a window that never opens. Never chosen to sell or repair at.
bool isMountVendor(std::string_view subName);
// A vendor of a holiday or of the Darkmoon Faire ("Lunar Festival Vendor",
// "Hallow's End Treats"): in the data all year, there only while it lasts.
bool isEventVendor(std::string_view name, std::string_view subName);
// The class an NPC trains by its title: "Paladin" of "Paladin Trainer". Not a
// "Demon Trainer", "Pet Trainer" or a profession's ("Junior Mining Trainer").
std::optional<std::string> trainerClass(std::string_view subName);

// Quest Mode's knowledge of the quests, from 's data (data/quests):
// for each quest in the log, the mobs it has killed or looted and the world
// spots  has them at, where the zone's map is placed - and whether it
// is done. One with nothing to do (a letter to take, someone to talk to) is
// done from the start; one with objectives the bot cannot count never is.
class QuestSource final : public IQuestSource
{
public:
    struct Files
    {
        std::string quests; // quests.json
        std::string npcs;   // npcs.json
        std::string items;  // items.json
        std::string zones;  // zones.json, the zones known to begin with
        // Where zones worked out from NPCs seen are kept, and read back from;
        // empty keeps them for the session only.
        std::string learnedZones;
        std::string objects; // objects.json; optional for older data exports
        // item_names.json: every item's name, not the quest items' alone.
        // Optional: without it only quest items have names.
        std::string itemNames;
        // subzones.json: the zone each subzone lies in. Optional: without it
        // only the few subzones og::canonicalZone knows are placed.
        std::string subzones;
        // Where the quest objects the data has no place for were seen, kept
        // and read back from; empty keeps them for the session only.
        std::string learnedObjects;
        std::string database; // world.db (optional; if set or if quests ends with .db, SQLite is used)
        std::string elites;   // elites.json (optional)
    };
    // `mapOfZone` gives the continent a zone lies on, so spots on another are
    // left out; nullopt when not known.
    using MapOfZone = std::function<std::optional<std::uint32_t>(std::uint32_t zone)>;

    bool load(const Files& files, MapOfZone mapOfZone, std::string* error = nullptr);
    bool loadFromDb(const std::filesystem::path& dbPath, MapOfZone mapOfZone, std::string* error = nullptr);
    const std::string* itemName(std::uint32_t itemId) const override;
    // For tests: the files' contents instead.
    bool loadFromJson(std::string_view quests, std::string_view npcs, std::string_view items, std::string_view zones, MapOfZone mapOfZone, std::string* error = nullptr, std::string_view objects = "", std::string_view elites = "");
    // Where to keep the quest objects seen, and read back from (Files::learnedObjects).
    void setLearnedObjects(std::string path);

    std::vector<QuestTarget> targets(const GameSnapshot& snapshot) override;
    void setIgnoredQuests(const std::set<std::uint32_t>& quests) override
    {
        ignoredQuests_ = quests;
        surveyedGivers_.clear();
    }
    void setStartZone(std::uint32_t zone) override;
    std::vector<Vec3> questGiverSpots(const GameSnapshot& snapshot) override;
    std::vector<VendorSpot> vendorSpots(const GameSnapshot& snapshot) override;
    std::vector<TrainerSpot> classTrainers(const GameSnapshot& snapshot) override;
    bool hasPendingQuests(const GameSnapshot& snapshot) override;
    std::vector<std::string> questItems(const GameSnapshot& snapshot) override;
    std::vector<std::string> offeredBy(const GameSnapshot& snapshot, std::uint32_t npcEntry) override;
    std::vector<std::string> mayOffer(const GameSnapshot& snapshot, std::uint32_t npcEntry) override;
    std::optional<std::uint32_t> questStartedByItem(const GameSnapshot& snapshot, std::uint32_t itemId) override;
    bool isDungeonQuest(std::uint32_t questId) const override;
    bool isEliteQuest(std::uint32_t questId) const override;
    // The starting zones' profession introduction: "The Adventurer" leads to
    // "The Great Outdoors" and that to "Camping 101: Cooking" (and the other
    // professions). Talking to trainers the bot has no use for.
    bool isCampingQuest(std::uint32_t questId) const;
    // A quest the bot cannot do, by its name: "Kyle's Gone Missing!" has Kyle
    // the dog found and fed a treat as he runs about Bloodhoof Village.
    bool isQuestTheBotCannotDo(std::uint32_t questId) const;
    bool isAutoIgnored(std::uint32_t questId) const override
    {
        return isDungeonQuest(questId) || isEliteQuest(questId) || isCampingQuest(questId) || isQuestTheBotCannotDo(questId);
    }
    void setEliteEntries(std::shared_ptr<const std::unordered_set<std::uint32_t>> entries) override
    {
        if (entries)
        {
            eliteNpcs_ = *entries;
        }
    }
    const std::unordered_set<std::uint32_t>& eliteNpcs() const
    {
        return eliteNpcs_;
    }
    const ZoneBounds& zones() const
    {
        return zones_;
    }

private:
    void loadElites(const std::string& path);
    std::unordered_set<std::uint32_t> eliteNpcs_;
    std::set<std::uint32_t> ignoredQuests_;
    struct Quest
    {
        std::string name;
        int minLevel = 0;
        int level = 0;
        std::vector<std::uint32_t> kill;
        std::vector<std::uint32_t> items;
        // How many of each the quest asks for, from its text; 0 where it does
        // not say, and that objective is never taken to be done.
        std::vector<int> killCounts;
        std::vector<int> itemCounts;
        // Objectives the bot cannot count (objects to use, reputation, spells).
        bool other = false;
        // Objects to use for credit - Alther's Mill's Tarantula Eggs to
        // destroy - one object for every one of its name, and how many.
        std::vector<std::uint32_t> use;
        std::vector<int> useCounts;
    };
    struct Spawn
    {
        std::uint32_t zone = 0;
        double x = 0, y = 0;
    };
    void calibrate(const GameSnapshot& snapshot);
    // Quest objects QuestieDB has nowhere for - Prickly Pear Fruit's bushes -
    // are placed where the client shows them: from then on their places are
    // quest spots, as the data's are.
    void noteObjects(const GameSnapshot& snapshot);
    void loadSeenObjects(std::string_view text);
    struct SeenObject
    {
        std::uint32_t map = 0;
        Vec3 at;
    };
    std::unordered_map<std::uint32_t, std::vector<SeenObject>> seenObjects_; // object -> where it was seen
    std::string learnedObjects_;
    // The start zone given at Start is the area the character stood in, and
    // its enclosing zone a guess (canonicalZone) that can be wrong: a house
    // in Kharanos came out as a Teldrassil area, and the bot went questing
    // there, 14000 yards off. Taken only where the character stands inside
    // it on this continent; otherwise the smallest zone around it.
    void settleStartZone(const GameSnapshot& snapshot);
    std::optional<std::uint32_t> enclosingZone(const GameSnapshot& snapshot) const;

    std::unordered_map<std::uint32_t, Quest> quests_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> drops_;       // item -> the mobs that drop it
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> objectDrops_; // item -> loot objects
    std::unordered_map<std::uint32_t, std::vector<Spawn>> objectSpawns_;
    std::unordered_map<std::uint32_t, std::string> objectNames_;
    // A quest's object to use -> every object of its name; filled by indexUsedObjects().
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> usedObjects_;
    void indexUsedObjects();
    std::unordered_map<std::uint32_t, std::vector<Spawn>> spawns_; // NPC -> where  has it
    std::vector<std::pair<std::uint32_t, std::string>> vendors_;   // entry, faction compatibility
    // Class trainers: entry, the class ("Paladin" of "Paladin Trainer"), faction compatibility.
    struct ClassTrainer
    {
        std::uint32_t entry;
        std::string className;
        std::string friendlyTo;
    };
    std::vector<ClassTrainer> classTrainers_;
    std::unordered_map<std::uint32_t, bool> repairers_;
    // Every spawn of an NPC friendly to no side, or one only, by zone: what
    // stands about a vendor (vendorSpots).
    struct Unfriendly
    {
        double x = 0, y = 0;
        int level = 0; // its highest
        bool toAlliance = false, toHorde = false; // friendly to that side
    };
    std::unordered_map<std::uint32_t, std::vector<Unfriendly>> unfriendly_;
    // A vendor's spawn (entry, spawn index) -> the levels of the mobs about it
    // hostile to the Alliance, and to the Horde; worked out once.
    std::map<std::pair<std::uint32_t, std::size_t>, std::pair<std::vector<int>, std::vector<int>>> vendorSurroundings_;
    std::unordered_map<std::uint32_t, std::string> npcNames_;
    std::unordered_map<std::uint32_t, float> npcLevels_; // halfway between its min and max level
    std::unordered_map<std::uint32_t, std::string> itemNames_;
    std::unordered_map<std::uint32_t, std::string> allItemNames_; // item_names.json
    std::unordered_map<std::uint32_t, std::uint32_t> subzones_;   // subzones.json
    // The zone a subzone lies in, by the data; og::canonicalZone's guess where
    // the data does not have it. Every canonicalZone() in here is this one.
    std::uint32_t canonicalZone(std::uint32_t zone) const;
    std::unordered_map<std::uint32_t, std::uint32_t> enders_;                // quest -> the NPC that takes it back
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> starters_; // quest -> NPCs that offer it
    ZoneBounds zones_;
    QuestGivers givers_;
    MapOfZone mapOfZone_;
    std::string learnedZones_;
    std::optional<std::uint32_t> startZone_;
    std::optional<std::uint32_t> unsettledStartZone_; // as given, until settleStartZone() places it
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> questZones_;
    std::unordered_map<std::uint32_t, bool> surveyedGivers_;
    std::vector<std::uint32_t> surveyedQuestLog_;
    // Quests seen in the log and gone from it since: handed in (the client
    // does not tell what was done before), for what needs them first.
    std::unordered_set<std::uint32_t> handedIn_;
    std::vector<std::uint32_t> lastQuestLog_;
    bool questLogSeen_ = false;
    // The log as last read with a quest in it.
    std::vector<std::uint32_t> lastFullLog_;
    // Quests taken during the session - the next step of a chain the bot
    // follows - worked on wherever they lead: Gryan Stoutmantle's step to
    // Mathias Shaw in Stormwind was left out for not being in Redridge, where
    // the bot started, and questing paused.
    std::unordered_set<std::uint32_t> taken_;
    void noteQuestLog(const GameSnapshot& snapshot);
    // The quests an NPC offers; strict: only those whose lead-in is known done.
    std::vector<std::string> offers(const GameSnapshot& snapshot, std::uint32_t npcEntry, bool strict, bool anyRaceOrClass = false);
    int surveyedLevel_ = 0;
    bool questInStartZone(std::uint32_t quest) const;
};

} // namespace og
