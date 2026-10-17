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
 *  File: QuestSource.cpp
 *
 *  Desc:
 *  Implementation of QuestSource (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/QuestSource.h"

#include "og/common/Json.h"
#include "og/common/Sqlite.h"
#include "og/state/AreaTable.h"
#include "og/state/Difficulty.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <limits>
#include <sstream>

namespace og
{

std::optional<std::string> trainerClass(std::string_view subName)
{
    constexpr std::string_view kTrainer = " Trainer";

    if (subName.size() <= kTrainer.size() || !subName.ends_with(kTrainer))
    {
        return std::nullopt;
    }

    const std::string className(subName.substr(0, subName.size() - kTrainer.size()));
    return parsePlayerClass(className) != PlayerClass::Unknown ? std::optional<std::string>(std::string(toString(parsePlayerClass(className)))) : std::nullopt;
}

bool isMountVendor(std::string_view subName)
{
    for (const std::string_view title : {"Breeder", "Mechanostrider", "Saber Handler", "Raptor Handler", "Kodo Mounts", "Horse Merchant", "Kennel Master"})
    {
        if (subName.find(title) != std::string_view::npos)
        {
            return true;
        }
    }

    return false;
}

// A vendor of a holiday (Lunar Festival, Hallow's End) or of the Darkmoon
// Faire: in the data all year, there only while it lasts.
bool isEventVendor(std::string_view name, std::string_view subName)
{
    if (name.find("Festival") != std::string_view::npos)
    {
        return true;
    }

    for (const std::string_view title : {"Hallow's End", "Darkmoon Faire", "Lunar Festival", "Midsummer", "Brewfest", "Winter Veil"})
    {
        if (subName.find(title) != std::string_view::npos)
        {
            return true;
        }
    }

    return false;
}

// Thunder Bluff and Undercity are reached by lift, which the bot does not take.
bool reachedByLift(std::uint32_t zone)
{
    return zone == 1638 || zone == 1497;
}

namespace
{

bool isDungeonZoneId(std::uint32_t zone)
{
    switch (zone)
    {
    case 209:  // Shadowfang Keep
    case 491:  // Razorfen Downs
    case 717:  // The Stockade
    case 718:  // Wailing Caverns
    case 719:  // Blackfathom Deeps
    case 721:  // Gnomeregan
    case 722:  // Razorfen Kraul
    case 796:  // Scarlet Monastery
    case 1176: // Zul'Farrak
    case 1337: // Uldaman
    case 1477: // Sunken Temple (Temple of Atal'Hakkar)
    case 1581: // Deadmines
    case 1583: // Blackrock Spire
    case 1584: // Blackrock Depths
    case 1585: // Blackrock Mountain (BRD subarea)
    case 1977: // Zul'Gurub
    case 2017: // Stratholme
    case 2057: // Scholomance
    case 2100: // Maraudon
    case 2159: // Onyxia's Lair
    case 2437: // Ragefire Chasm
    case 2557: // Dire Maul
    case 2677: // Blackwing Lair
    case 2717: // Molten Core
    case 3428: // Ahn'Qiraj Temple
    case 3429: // Ruins of Ahn'Qiraj
    case 3456: // Naxxramas
    case 10022:
    case 10023:
    case 10024:
    case 10025:
    case 10026:
    case 10027:
        return true;
    default:
        return false;
    }
}

bool hasTag(std::string_view text, std::string_view tag)
{
    if (text.size() < tag.size())
    {
        return false;
    }
    auto it = std::search(text.begin(), text.end(), tag.begin(), tag.end(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
    return it != text.end();
}

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

bool readFile(const std::string& path, std::string& text)
{
    std::ifstream file(path);

    if (!file.is_open())
    {
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    text = ss.str();
    return true;
}

} // namespace

const std::string* QuestSource::itemName(std::uint32_t itemId) const
{
    for (const auto* names : {&allItemNames_, &itemNames_})
    {
        if (const auto it = names->find(itemId); it != names->end() && !it->second.empty())
        {
            return &it->second;
        }
    }

    return nullptr;
}

std::uint32_t QuestSource::canonicalZone(std::uint32_t zone) const
{
    bool known = false;

    // A subzone of a subzone leads on; no table is that deep.
    for (int depth = 0; depth < 8; ++depth)
    {
        const auto parent = subzones_.find(zone);

        if (parent == subzones_.end() || parent->second == zone)
        {
            break;
        }

        zone = parent->second;
        known = true;
    }

    return known ? zone : og::canonicalZone(zone);
}

bool QuestSource::load(const Files& files, MapOfZone mapOfZone, std::string* error)
{
    std::string dbFile;
    if (!files.database.empty() && std::filesystem::exists(files.database))
    {
        dbFile = files.database;
    }
    else if (files.quests.ends_with(".db") || files.quests.ends_with(".sqlite"))
    {
        dbFile = files.quests;
    }

    if (!dbFile.empty())
    {
        learnedZones_ = files.learnedZones;
        setLearnedObjects(files.learnedObjects);

        std::string dbError;
        if (loadFromDb(dbFile, mapOfZone, &dbError))
        {
            return true;
        }

        // A database that cannot be read: the JSON it was compiled from,
        // where the caller named any.
        if (dbFile == files.quests || files.quests.empty())
        {
            if (error)
            {
                *error = dbError;
            }
            return false;
        }

        Files json = files;
        json.database.clear();
        if (load(json, std::move(mapOfZone), error))
        {
            return true;
        }
        if (error)
        {
            *error = dbFile + ": " + dbError + "; " + *error;
        }
        return false;
    }

    std::string quests, npcs, items, zones, objects;
    // Before the quests: their zones are worked out with it.
    subzones_.clear();

    if (std::string text; !files.subzones.empty() && readFile(files.subzones, text))
    {
        try
        {
            const Json json = Json::parse(text);

            if (const Json* table = json.find("subzones"); table && table->isObject())
            {
                for (const auto& [key, parent] : table->asObject())
                {
                    subzones_[idOf(key)] = static_cast<std::uint32_t>(parent.asInt(0));
                }
            }
        }
        catch (const std::exception&)
        {
            subzones_.clear(); // the built-in guesses then
        }
    }

    // For everyone who asks og::canonicalZone: the client's reader too,
    // which takes the map from the zone. A source loaded without them
    // leaves the table there is.
    if (!subzones_.empty())
    {
        og::setSubzoneParents(subzones_);
    }

    for (const auto& [path, text] : {std::pair{&files.quests, &quests}, std::pair{&files.npcs, &npcs}, std::pair{&files.items, &items}})
    {
        if (!readFile(*path, *text))
        {
            if (error)
            {
                *error = "could not open " + *path;
            }

            return false;
        }
    }

    if (!readFile(files.zones, zones))
    {
        zones = R"({"zones":{}})";
    }

    if (!files.objects.empty() && !readFile(files.objects, objects))
    {
        if (error)
        {
            *error = "could not open " + files.objects;
        }

        return false;
    }

    std::string elitesText;
    if (!files.elites.empty())
    {
        readFile(files.elites, elitesText);
    }
    else if (!files.quests.empty())
    {
        const auto sibling = std::filesystem::path(files.quests).parent_path() / "elites.json";
        if (std::filesystem::exists(sibling))
        {
            readFile(sibling.string(), elitesText);
        }
    }

    if (!loadFromJson(quests, npcs, items, zones, std::move(mapOfZone), error, objects, elitesText))
    {
        return false;
    }

    allItemNames_.clear();

    if (std::string text; !files.itemNames.empty() && readFile(files.itemNames, text))
    {
        try
        {
            const Json json = Json::parse(text);

            if (const Json* names = json.find("names"); names && names->isObject())
            {
                for (const auto& [key, name] : names->asObject())
                {
                    if (name.isString())
                    {
                        allItemNames_[idOf(key)] = name.asString();
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            allItemNames_.clear(); // names are a nicety: the quests work without
        }
    }

    learnedZones_ = files.learnedZones;
    setLearnedObjects(files.learnedObjects);

    // The zones worked out before; the shipped ones still come first.
    if (std::string learned; !learnedZones_.empty() && readFile(learnedZones_, learned))
    {
        ZoneBounds found;

        if (found.loadFromJson(learned))
        {
            for (std::uint32_t zone = 1; zone < 10000; ++zone)
            {
                if (const auto b = found.find(zone); b && !zones_.find(zone))
                {
                    zones_.set(zone, *b);
                }
            }
        }
    }

    return true;
}

bool QuestSource::loadFromDb(const std::filesystem::path& dbPath, MapOfZone mapOfZone, std::string* error)
{
    SqliteDb db;
    if (!db.openReadOnly(dbPath, error))
    {
        return false;
    }

    subzones_.clear();
    quests_.clear();
    drops_.clear();
    objectDrops_.clear();
    objectSpawns_.clear();
    objectNames_.clear();
    spawns_.clear();
    vendors_.clear();
    unfriendly_.clear();
    vendorSurroundings_.clear();
    repairers_.clear();
    questZones_.clear();
    npcNames_.clear();
    npcLevels_.clear();
    itemNames_.clear();
    allItemNames_.clear();
    enders_.clear();
    starters_.clear();
    eliteNpcs_.clear();

    try
    {
        // 1. Zones
        if (!zones_.loadFromDb(db, error))
        {
            return false;
        }

        // 2. QuestGivers
        if (!givers_.loadFromDb(db, error))
        {
            return false;
        }

        // 3. Subzones
        auto subStmt = db.prepare("SELECT id, parent_zone_id FROM subzones");
        while (subStmt.step())
        {
            subzones_[subStmt.columnUint32(0)] = subStmt.columnUint32(1);
        }
        if (!subzones_.empty())
        {
            og::setSubzoneParents(subzones_);
        }

        // 4. Quests
        auto qStmt = db.prepare("SELECT id, name, min_level, level, other FROM quests");
        while (qStmt.step())
        {
            const auto qid = qStmt.columnUint32(0);
            Quest q;
            q.name = qStmt.columnString(1);
            q.minLevel = qStmt.columnInt(2);
            q.level = qStmt.columnInt(3);
            q.other = qStmt.columnInt(4) != 0;
            quests_.emplace(qid, std::move(q));
        }

        auto objStmt = db.prepare("SELECT quest_id, type, target_id, count FROM quest_objectives ORDER BY quest_id, obj_index");
        while (objStmt.step())
        {
            const auto qid = objStmt.columnUint32(0);
            auto it = quests_.find(qid);
            if (it != quests_.end())
            {
                const auto type = objStmt.columnText(1);
                const auto target = objStmt.columnUint32(2);
                const auto count = objStmt.columnInt(3);
                if (type == "kill")
                {
                    it->second.kill.push_back(target);
                    it->second.killCounts.push_back(count);
                }
                else if (type == "item")
                {
                    it->second.items.push_back(target);
                    it->second.itemCounts.push_back(count);
                }
                else if (type == "use")
                {
                    it->second.use.push_back(target);
                    it->second.useCounts.push_back(count);
                }
            }
        }

        // 5. Items & item drops
        auto itemStmt = db.prepare("SELECT id, name FROM items");
        while (itemStmt.step())
        {
            const auto id = itemStmt.columnUint32(0);
            auto name = itemStmt.columnString(1);
            itemNames_.emplace(id, name);
            allItemNames_.emplace(id, std::move(name));
        }

        auto indStmt = db.prepare("SELECT item_id, npc_id FROM item_npc_drops");
        while (indStmt.step())
        {
            drops_[indStmt.columnUint32(0)].push_back(indStmt.columnUint32(1));
        }

        auto iodStmt = db.prepare("SELECT item_id, object_id FROM item_object_drops");
        while (iodStmt.step())
        {
            objectDrops_[iodStmt.columnUint32(0)].push_back(iodStmt.columnUint32(1));
        }

        // 6. Object names and spawns
        auto objNameStmt = db.prepare("SELECT id, name FROM objects");
        while (objNameStmt.step())
        {
            objectNames_[objNameStmt.columnUint32(0)] = objNameStmt.columnString(1);
        }

        auto ospStmt = db.prepare("SELECT object_id, zone_id, x, y FROM object_spawns");
        while (ospStmt.step())
        {
            objectSpawns_[ospStmt.columnUint32(0)].push_back(Spawn{ospStmt.columnUint32(1), ospStmt.columnDouble(2), ospStmt.columnDouble(3)});
        }

        // 7. NPC spawns
        auto nspStmt = db.prepare("SELECT npc_id, zone_id, x, y FROM npc_spawns");
        while (nspStmt.step())
        {
            spawns_[nspStmt.columnUint32(0)].push_back(Spawn{nspStmt.columnUint32(1), nspStmt.columnDouble(2), nspStmt.columnDouble(3)});
        }

        // 8. Quest starters & enders
        auto qsStmt = db.prepare("SELECT quest_id, npc_id FROM quest_starters");
        while (qsStmt.step())
        {
            const auto qid = qsStmt.columnUint32(0);
            const auto nid = qsStmt.columnUint32(1);
            starters_[qid].push_back(nid);
            if (auto it = spawns_.find(nid); it != spawns_.end())
            {
                for (const auto& sp : it->second)
                {
                    questZones_[qid].push_back(sp.zone);
                }
            }
        }

        auto qeStmt = db.prepare("SELECT quest_id, npc_id FROM quest_enders");
        while (qeStmt.step())
        {
            const auto qid = qeStmt.columnUint32(0);
            const auto nid = qeStmt.columnUint32(1);
            enders_.emplace(qid, nid);
            if (auto it = spawns_.find(nid); it != spawns_.end())
            {
                for (const auto& sp : it->second)
                {
                    questZones_[qid].push_back(sp.zone);
                }
            }
        }

        // 9. NPCs (vendors, names, levels, unfriendly)
        auto npcStmt = db.prepare("SELECT id, name, sub_name, min_level, max_level, friendly_to, npc_flags FROM npcs");
        while (npcStmt.step())
        {
            const auto entry = npcStmt.columnUint32(0);
            const auto name = npcStmt.columnString(1);
            const auto subName = npcStmt.columnString(2);
            const auto low = npcStmt.columnInt(3);
            const auto high = npcStmt.columnInt(4);
            const auto friendlyTo = npcStmt.columnString(5);
            const auto flags = npcStmt.columnInt(6);

            if ((flags & 4) && !isMountVendor(subName) && !isEventVendor(name, subName))
            {
                vendors_.emplace_back(entry, friendlyTo);
                repairers_[entry] = (flags & 16384) != 0;
            }

            if (const auto className = trainerClass(subName))
            {
                classTrainers_.push_back({entry, *className, friendlyTo});
            }

            if (!name.empty())
            {
                npcNames_[entry] = name;
            }

            if (low > 0)
            {
                const int top = high ? std::max(high, low) : low;
                npcLevels_[entry] = (low + top) / 2.0f;
            }

            const int topLevel = high ? high : 0;
            if (topLevel > 1 && friendlyTo != "AH")
            {
                if (auto it = spawns_.find(entry); it != spawns_.end())
                {
                    for (const auto& spawn : it->second)
                    {
                        unfriendly_[spawn.zone].push_back({spawn.x, spawn.y, topLevel, friendlyTo.find('A') != std::string::npos, friendlyTo.find('H') != std::string::npos});
                    }
                }
            }
        }

        // 10. Elites
        try
        {
            auto eliteStmt = db.prepare("SELECT npc_id FROM elites");
            while (eliteStmt.step())
            {
                eliteNpcs_.insert(eliteStmt.columnUint32(0));
            }
            auto eliteNpcStmt = db.prepare("SELECT id FROM npcs WHERE is_elite = 1");
            while (eliteNpcStmt.step())
            {
                eliteNpcs_.insert(eliteNpcStmt.columnUint32(0));
            }
        }
        catch (const std::exception&)
        {
        }

        mapOfZone_ = std::move(mapOfZone);
        indexUsedObjects();

        if (!learnedZones_.empty())
        {
            if (std::string learned; readFile(learnedZones_, learned))
            {
                ZoneBounds found;
                if (found.loadFromJson(learned))
                {
                    for (std::uint32_t zone = 1; zone < 10000; ++zone)
                    {
                        if (const auto b = found.find(zone); b && !zones_.find(zone))
                        {
                            zones_.set(zone, *b);
                        }
                    }
                }
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
        std::fprintf(stderr, "QuestSource::loadFromDb exception: %s\n", e.what());
        return false;
    }
}

bool QuestSource::loadFromJson(std::string_view questsText, std::string_view npcsText, std::string_view itemsText, std::string_view zonesText, MapOfZone mapOfZone, std::string* error, std::string_view objectsText, std::string_view elitesText)
{
    try
    {
        const Json questsJson = Json::parse(questsText);
        const Json npcsJson = Json::parse(npcsText);
        const Json itemsJson = Json::parse(itemsText);
        const Json objectsJson = Json::parse(objectsText.empty() ? R"({"objects":{}})" : objectsText);
        const Json* quests = questsJson.find("quests");
        const Json* npcs = npcsJson.find("npcs");
        const Json* items = itemsJson.find("items");
        const Json* objects = objectsJson.find("objects");

        if (!quests || !npcs || !items || !objects || !quests->isObject() || !npcs->isObject() || !items->isObject() || !objects->isObject())
        {
            if (error)
            {
                *error = "expected \"quests\", \"npcs\", \"items\" and \"objects\" objects";
            }

            return false;
        }

        std::unordered_set<std::uint32_t> elites;
        if (!elitesText.empty())
        {
            try
            {
                const Json elitesJson = Json::parse(elitesText);
                if (const Json* list = elitesJson.find("elites"); list && list->isArray())
                {
                    for (const auto& entry : list->asArray())
                    {
                        elites.insert(static_cast<std::uint32_t>(entry.asInt()));
                    }
                }
            }
            catch (const std::exception&)
            {
            }
        }

        if (!zones_.loadFromJson(zonesText, error))
        {
            return false;
        }

        if (!givers_.loadFromJson(questsText, npcsText, error))
        {
            return false;
        }

        eliteNpcs_ = std::move(elites);
        quests_.clear();
        drops_.clear();
        objectDrops_.clear();
        objectSpawns_.clear();
        objectNames_.clear();
        spawns_.clear();
        vendors_.clear();
        // Worked out from the last data: loaded again, it is the new data's.
        unfriendly_.clear();
        vendorSurroundings_.clear();
        repairers_.clear();
        questZones_.clear();

        for (const auto& [key, q] : quests->asObject())
        {
            const auto counts = [&](std::string_view countKey)
            {
                std::vector<int> out;

                for (const auto id : idsOf(q, countKey))
                {
                    out.push_back(static_cast<int>(id));
                }

                return out;
            };
            const int minLvl = q.find("minLevel") ? static_cast<int>(q.find("minLevel")->asInt(0)) : 0;
            const int lvl = q.find("level") ? static_cast<int>(q.find("level")->asInt(0)) : 0;
            Quest quest{q.find("name") ? q.find("name")->asString("") : "", minLvl, lvl, idsOf(q, "kill"), idsOf(q, "items"), counts("killCounts"), counts("itemCounts"), q.find("other") && q.find("other")->asBool(false)};
            quest.use = idsOf(q, "use");
            quest.useCounts = counts("useCounts");
            quests_.emplace(idOf(key), std::move(quest));
        }

        npcNames_.clear();
        itemNames_.clear();
        enders_.clear();
        starters_.clear();

        for (const auto& [key, i] : items->asObject())
        {
            drops_[idOf(key)] = idsOf(i, "npcDrops");
            objectDrops_[idOf(key)] = idsOf(i, "objectDrops");

            if (const Json* name = i.find("name"); name && name->isString())
            {
                itemNames_[idOf(key)] = name->asString();
            }
        }

        const auto spawnsOf = [](const Json& entity)
        {
            std::vector<Spawn> list;
            const Json* spawns = entity.find("spawns");

            if (!spawns || !spawns->isObject())
            {
                return list;
            }

            for (const auto& [zone, coords] : spawns->asObject())
            {
                if (!coords.isArray())
                {
                    continue;
                }

                for (const auto& c : coords.asArray())
                {
                    if (c.isArray() && c.asArray().size() >= 2)
                    {
                        list.push_back({idOf(zone), c.asArray()[0].asDouble(), c.asArray()[1].asDouble()});
                    }
                }
            }

            return list;
        };

        for (const auto& [key, object] : objects->asObject())
        {
            objectSpawns_[idOf(key)] = spawnsOf(object);

            if (const Json* name = object.find("name"); name && name->isString())
            {
                objectNames_[idOf(key)] = name->asString();
            }
        }

        for (const auto& [key, n] : npcs->asObject())
        {
            const std::uint32_t entry = idOf(key);
            const int flags = n.find("npcFlags") ? n.find("npcFlags")->asInt() : 0;
            const Json* subName = n.find("subName");

            const std::string vendorName = n.find("name") && n.find("name")->isString() ? n.find("name")->asString("") : std::string();
            const std::string vendorTitle = subName && subName->isString() ? subName->asString("") : std::string();

            if ((flags & 4) && !isMountVendor(vendorTitle) && !isEventVendor(vendorName, vendorTitle))
            {
                vendors_.emplace_back(entry, n.find("friendlyTo") ? n.find("friendlyTo")->asString("") : "");
                repairers_[entry] = (flags & 16384) != 0;
            }

            if (const auto className = trainerClass(subName && subName->isString() ? subName->asString("") : std::string()))
            {
                classTrainers_.push_back({entry, *className, n.find("friendlyTo") ? n.find("friendlyTo")->asString("") : ""});
            }

            const Json* name = n.find("name");

            if (name && name->isString())
            {
                npcNames_[idOf(key)] = name->asString();
            }

            if (const Json* low = n.find("minLevel"); low && low->asInt(0) > 0)
            {
                const Json* high = n.find("maxLevel");
                const int top = high ? std::max(high->asInt(0), low->asInt(0)) : low->asInt(0);
                npcLevels_[entry] = (low->asInt(0) + top) / 2.0f;
            }

            for (const auto quest : idsOf(n, "questEnds"))
            {
                enders_.emplace(quest, idOf(key));
            }

            auto list = spawnsOf(n);
            const std::string friendlyTo = n.find("friendlyTo") ? n.find("friendlyTo")->asString("") : "";
            const int topLevel = n.find("maxLevel") ? n.find("maxLevel")->asInt(0) : 0;

            // Critters (level 1) do not count.
            if (topLevel > 1 && friendlyTo != "AH")
            {
                for (const auto& spawn : list)
                {
                    unfriendly_[spawn.zone].push_back({spawn.x, spawn.y, topLevel, friendlyTo.find('A') != std::string::npos, friendlyTo.find('H') != std::string::npos});
                }
            }

            for (const auto quest : idsOf(n, "questStarts"))
            {
                starters_[quest].push_back(entry);

                for (const auto& spawn : list)
                {
                    questZones_[quest].push_back(spawn.zone);
                }
            }

            // Active quests can outlive a chain's original giver and some
            //  exports omit that giver's zone. The turn-in NPC is
            // still authoritative evidence that the quest belongs to this
            // zone, especially for simple delivery quests with no objectives.
            for (const auto quest : idsOf(n, "questEnds"))
            {
                for (const auto& spawn : list)
                {
                    questZones_[quest].push_back(spawn.zone);
                }
            }

            if (!list.empty())
            {
                spawns_[idOf(key)] = std::move(list);
            }
        }

        mapOfZone_ = std::move(mapOfZone);
        indexUsedObjects();
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

void QuestSource::loadElites(const std::string& path)
{
    if (std::string text; readFile(path, text))
    {
        try
        {
            const Json json = Json::parse(text);
            if (const Json* list = json.find("elites"); list && list->isArray())
            {
                for (const auto& entry : list->asArray())
                {
                    eliteNpcs_.insert(static_cast<std::uint32_t>(entry.asInt()));
                }
            }
        }
        catch (const std::exception&)
        {
        }
    }
}

bool QuestSource::isDungeonQuest(std::uint32_t questId) const
{
    const auto it = quests_.find(questId);
    if (it == quests_.end())
    {
        return false;
    }

    const Quest& q = it->second;
    if (hasTag(q.name, "[dungeon]") || hasTag(q.name, "(dungeon)") ||
        hasTag(q.name, "[raid]") || hasTag(q.name, "(raid)") ||
        hasTag(q.name, "[heroic]") || hasTag(q.name, "(heroic)"))
    {
        return true;
    }

    for (const auto npcId : q.kill)
    {
        if (const auto spIt = spawns_.find(npcId); spIt != spawns_.end() && !spIt->second.empty())
        {
            for (const auto& sp : spIt->second)
            {
                if (isDungeonZoneId(sp.zone))
                {
                    return true;
                }
            }
        }
    }

    for (const auto itemId : q.items)
    {
        if (const auto dropIt = drops_.find(itemId); dropIt != drops_.end())
        {
            for (const auto mobId : dropIt->second)
            {
                if (const auto spIt = spawns_.find(mobId); spIt != spawns_.end() && !spIt->second.empty())
                {
                    for (const auto& sp : spIt->second)
                    {
                        if (isDungeonZoneId(sp.zone))
                        {
                            return true;
                        }
                    }
                }
            }
        }

        if (const auto objDropIt = objectDrops_.find(itemId); objDropIt != objectDrops_.end())
        {
            for (const auto objId : objDropIt->second)
            {
                if (const auto spIt = objectSpawns_.find(objId); spIt != objectSpawns_.end() && !spIt->second.empty())
                {
                    for (const auto& sp : spIt->second)
                    {
                        if (isDungeonZoneId(sp.zone))
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }

    for (const auto objId : q.use)
    {
        if (const auto spIt = objectSpawns_.find(objId); spIt != objectSpawns_.end() && !spIt->second.empty())
        {
            for (const auto& sp : spIt->second)
            {
                if (isDungeonZoneId(sp.zone))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool QuestSource::isCampingQuest(std::uint32_t questId) const
{
    const auto it = quests_.find(questId);

    if (it == quests_.end())
    {
        return false;
    }

    const std::string& name = it->second.name;
    return name == "The Adventurer" || name == "The Great Outdoors" || name.starts_with("Camping 101:");
}

bool QuestSource::isEliteQuest(std::uint32_t questId) const
{
    const auto it = quests_.find(questId);
    if (it == quests_.end())
    {
        return false;
    }

    const Quest& q = it->second;
    if (hasTag(q.name, "[elite]") || hasTag(q.name, "(elite)"))
    {
        return true;
    }

    for (const auto npcId : q.kill)
    {
        if (eliteNpcs_.contains(npcId))
        {
            return true;
        }
    }

    for (const auto itemId : q.items)
    {
        if (const auto dropIt = drops_.find(itemId); dropIt != drops_.end() && !dropIt->second.empty())
        {
            bool allElite = true;
            for (const auto mobId : dropIt->second)
            {
                if (!eliteNpcs_.contains(mobId))
                {
                    allElite = false;
                    break;
                }
            }
            if (allElite)
            {
                return true;
            }
        }
    }

    return false;
}

void QuestSource::indexUsedObjects()
{
    usedObjects_.clear();

    for (const auto& [id, quest] : quests_)
    {
        for (const auto use : quest.use)
        {
            auto& same = usedObjects_[use];
            const auto name = objectNames_.find(use);

            for (const auto& [object, objectName] : objectNames_)
            {
                // (Not by an empty name: every object the data has no name for would be one of them.)
                if (name != objectNames_.end() && !name->second.empty() && objectName == name->second && std::find(same.begin(), same.end(), object) == same.end())
                {
                    same.push_back(object);
                }
            }

            if (same.empty())
            {
                same.push_back(use);
            }

            std::sort(same.begin(), same.end());
        }
    }
}

void QuestSource::calibrate(const GameSnapshot& s)
{
    bool learnt = false;

    for (const auto& u : s.units)
    {
        if (u.isPlayer || u.entry == 0)
        {
            continue;
        }

        const auto it = spawns_.find(u.entry);

        // Only an NPC  has in the one place: a mob's many spawns say
        // nothing about which one this is.
        if (it == spawns_.end() || it->second.size() != 1)
        {
            continue;
        }

        const Spawn& spawn = it->second.front();

        if (mapOfZone_ && s.mapId)
        {
            if (const auto map = mapOfZone_(spawn.zone); map && *map != *s.mapId)
            {
                continue;
            }
        }

        learnt |= zones_.calibrate(spawn.zone, spawn.x, spawn.y, u.position);
    }

    if (learnt && !learnedZones_.empty())
    {
        std::ofstream file(learnedZones_, std::ios::trunc);
        file << zones_.toJson();
    }
}

void QuestSource::setLearnedObjects(std::string path)
{
    // Another file: what the last one held is not this one's.
    learnedObjects_ = std::move(path);
    seenObjects_.clear();

    if (std::string text; !learnedObjects_.empty() && readFile(learnedObjects_, text))
    {
        loadSeenObjects(text);
    }
}

void QuestSource::loadSeenObjects(std::string_view text)
{
    try
    {
        const Json json = Json::parse(text);

        if (const Json* objects = json.find("objects"); objects && objects->isObject())
        {
            for (const auto& [key, places] : objects->asObject())
            {
                std::uint32_t entry = 0;
                std::from_chars(key.data(), key.data() + key.size(), entry);

                if (entry == 0 || !places.isArray())
                {
                    continue;
                }

                for (const auto& p : places.asArray())
                {
                    if (p.isArray() && p.size() == 4)
                    {
                        seenObjects_[entry].push_back({static_cast<std::uint32_t>(p[0].asInt()), {p[1].asFloat(), p[2].asFloat(), p[3].asFloat()}});
                    }
                }
            }
        }
    }
    catch (const std::exception&)
    {
        seenObjects_.clear(); // learnt again as they are seen
    }
}

void QuestSource::noteObjects(const GameSnapshot& s)
{
    if (!s.mapId || s.objects.empty())
    {
        return;
    }

    // One place to every few yards: a bush seen again is not a new one.
    constexpr float kSamePlace = 5.0f;
    constexpr std::size_t kMostPlaces = 200;
    bool learnt = false;

    for (const auto& object : s.objects)
    {
        if (object.entry == 0)
        {
            continue;
        }

        if (const auto known = objectSpawns_.find(object.entry); known != objectSpawns_.end() && !known->second.empty())
        {
            continue;
        }

        const auto dropsOrIsUsed = [&](const auto& objects) { return std::find(objects.second.begin(), objects.second.end(), object.entry) != objects.second.end(); };
        const bool questObject = std::any_of(objectDrops_.begin(), objectDrops_.end(), dropsOrIsUsed) || std::any_of(usedObjects_.begin(), usedObjects_.end(), dropsOrIsUsed);

        if (!questObject)
        {
            continue;
        }

        auto& places = seenObjects_[object.entry];

        if (places.size() >= kMostPlaces || std::any_of(places.begin(), places.end(), [&](const SeenObject& p) { return p.map == *s.mapId && distance2D(p.at, object.position) < kSamePlace; }))
        {
            continue;
        }

        places.push_back({*s.mapId, object.position});
        learnt = true;
    }

    if (learnt && !learnedObjects_.empty())
    {
        std::string out = "{\"objects\":{\n";
        bool first = true;

        for (const auto& [entry, places] : seenObjects_)
        {
            out += std::format("{}\"{}\":[", first ? "" : ",\n", entry);

            for (std::size_t i = 0; i < places.size(); ++i)
            {
                out += std::format("{}[{},{:.2f},{:.2f},{:.2f}]", i ? "," : "", places[i].map, places[i].at.x, places[i].at.y, places[i].at.z);
            }

            out += "]";
            first = false;
        }

        std::ofstream file(learnedObjects_, std::ios::trunc);
        file << out << "\n}}\n";
    }
}

void QuestSource::noteQuestLog(const GameSnapshot& s)
{
    for (const auto id : lastQuestLog_)
    {
        if (std::find(s.player.questLog.begin(), s.player.questLog.end(), id) == s.player.questLog.end())
        {
            handedIn_.insert(id);
        }
    }

    // Only a quest or two at once, and none the log held before it was read
    // empty: a log read empty for a moment and back is not taken. One taken
    // after the log emptied is - the next step of a chain, the step before
    // handed in first (The Defias Brotherhood, the only quest in the log).
    constexpr std::size_t kTakenAtOnce = 2;
    std::vector<std::uint32_t> added;

    for (const auto id : s.player.questLog)
    {
        const bool had = std::find(lastQuestLog_.begin(), lastQuestLog_.end(), id) != lastQuestLog_.end();
        const bool back = lastQuestLog_.empty() && std::find(lastFullLog_.begin(), lastFullLog_.end(), id) != lastFullLog_.end();

        if (!had && !back)
        {
            added.push_back(id);
        }
    }

    if (questLogSeen_ && added.size() <= kTakenAtOnce)
    {
        taken_.insert(added.begin(), added.end());
    }

    questLogSeen_ = true;
    lastQuestLog_ = s.player.questLog;

    if (!s.player.questLog.empty())
    {
        lastFullLog_ = s.player.questLog;
    }
}

std::vector<std::string> QuestSource::offers(const GameSnapshot& s, std::uint32_t npcEntry, bool strict, bool anyRaceOrClass)
{
    settleStartZone(s);
    noteQuestLog(s);
    QuestGivers::Character who{s.player.level, s.player.race, static_cast<int>(s.player.playerClass), s.player.questLog};
    who.done = &handedIn_;
    who.strict = strict;
    who.anyRaceOrClass = anyRaceOrClass;
    std::vector<std::string> out;

    for (const auto* q : givers_.offers(npcEntry, who))
    {
        // Elite quests may be accepted and left in the quest log; their
        // objectives are still auto-ignored once active. Dungeon and camping
        // quests are excluded from pickup as well as execution.
        if (questInStartZone(q->id) && !ignoredQuests_.contains(q->id) && !isDungeonQuest(q->id) && !isCampingQuest(q->id))
        {
            out.push_back(q->name);
        }
    }

    return out;
}

std::vector<std::string> QuestSource::offeredBy(const GameSnapshot& s, std::uint32_t npcEntry)
{
    // Asked about an NPC in sight with a yellow !: what it needs first was
    // most likely done before the session, where nothing says otherwise.
    return offers(s, npcEntry, false);
}

std::vector<std::string> QuestSource::mayOffer(const GameSnapshot& s, std::uint32_t npcEntry)
{
    return offers(s, npcEntry, false, true);
}

std::optional<std::uint32_t> QuestSource::questStartedByItem(const GameSnapshot& s, std::uint32_t itemId)
{
    settleStartZone(s);
    noteQuestLog(s);
    QuestGivers::Character who{s.player.level, s.player.race, static_cast<int>(s.player.playerClass), s.player.questLog};
    who.done = &handedIn_;
    who.strict = false;
    who.anyRaceOrClass = false;
    const auto* q = givers_.itemOffer(itemId, who);
    // As with NPC offers, allow elite quest starters but keep dungeon and
    // camping quests out; active elite quests are filtered from execution in targets().
    if (!q || ignoredQuests_.contains(q->id) || isDungeonQuest(q->id) || isCampingQuest(q->id))
    {
        return std::nullopt;
    }

    if (std::find(s.player.questLog.begin(), s.player.questLog.end(), q->id) != s.player.questLog.end())
    {
        return std::nullopt;
    }

    if (handedIn_.contains(q->id))
    {
        return std::nullopt;
    }

    return q->id;
}

void QuestSource::setStartZone(std::uint32_t zone)
{
    startZone_ = canonicalZone(zone);
    unsettledStartZone_ = zone;
}

namespace
{
bool insideBounds(const ZoneBounds::Bounds& b, const Vec3& p)
{
    return p.x >= std::min(b.top, b.bottom) && p.x <= std::max(b.top, b.bottom) && p.y >= std::min(b.left, b.right) && p.y <= std::max(b.left, b.right);
}

} // namespace

std::optional<std::uint32_t> QuestSource::enclosingZone(const GameSnapshot& s) const
{
    if (!s.mapId || !mapOfZone_)
    {
        return std::nullopt;
    }

    // Zones are known by their maps' rectangles, which overlap: Saldean's Farm
    // in Westfall lies inside Elwynn Forest's too. Of the zones whose rectangle
    // holds the character, the one with an NPC nearest to it - by the smaller
    // rectangle, Elwynn was taken for the start zone there, and none of the
    // Westfall quests in the log was worked on.
    std::optional<std::uint32_t> enclosing;
    float nearest = std::numeric_limits<float>::max();

    for (const auto& [npc, spawns] : spawns_)
    {
        for (const auto& spawn : spawns)
        {
            const auto bounds = zones_.find(spawn.zone);
            const auto map = mapOfZone_(spawn.zone);

            if (!bounds || !map || *map != *s.mapId || !insideBounds(*bounds, s.player.position))
            {
                continue;
            }

            const auto at = zones_.toWorld(spawn.zone, spawn.x, spawn.y);

            if (!at)
            {
                continue;
            }

            if (const float d = distance2D(*at, s.player.position); d < nearest)
            {
                nearest = d;
                enclosing = spawn.zone;
            }
        }
    }

    return enclosing;
}

void QuestSource::settleStartZone(const GameSnapshot& s)
{
    if (!unsettledStartZone_)
    {
        return;
    }

    const std::uint32_t zone = *unsettledStartZone_;
    unsettledStartZone_.reset();
    const auto fits = [&](std::uint32_t z)
    {
        const auto bounds = zones_.find(z);

        if (!bounds)
        {
            return false;
        }

        if (mapOfZone_ && s.mapId)
        {
            if (const auto map = mapOfZone_(z); !map || *map != *s.mapId)
            {
                return false;
            }
        }

        return insideBounds(*bounds, s.player.position);
    };

    if (fits(zone))
    {
        startZone_ = zone;
    }
    else if (fits(canonicalZone(zone)))
    {
        startZone_ = canonicalZone(zone);
    }
    else if (const auto around = enclosingZone(s))
    {
        startZone_ = around;
    }

    // Otherwise the guess stands, as before.
}

bool QuestSource::questInStartZone(std::uint32_t quest) const
{
    if (!startZone_ || taken_.contains(quest))
    {
        return true;
    }

    const auto it = questZones_.find(quest);

    if (it == questZones_.end())
    {
        return false;
    }

    const auto targetZone = canonicalZone(*startZone_);
    return std::any_of(it->second.begin(), it->second.end(), [&](std::uint32_t z) { return canonicalZone(z) == targetZone; });
}

bool QuestSource::hasPendingQuests(const GameSnapshot& s)
{
    settleStartZone(s);
    return std::any_of(s.player.questLog.begin(), s.player.questLog.end(), [this](auto id) {
        return questInStartZone(id) && !ignoredQuests_.contains(id) && !isAutoIgnored(id);
    });
}

std::vector<std::string> QuestSource::questItems(const GameSnapshot& s)
{
    std::vector<std::string> out;

    // Every quest in the log, whatever its zone or level: sold meat is as
    // gone for a quest left for later.
    for (const auto id : s.player.questLog)
    {
        const auto quest = quests_.find(id);

        if (quest == quests_.end())
        {
            continue;
        }

        for (const auto item : quest->second.items)
        {
            if (const auto name = itemNames_.find(item); name != itemNames_.end() && !name->second.empty() && std::find(out.begin(), out.end(), name->second) == out.end())
            {
                out.push_back(name->second);
            }
        }
    }

    return out;
}

std::vector<Vec3> QuestSource::questGiverSpots(const GameSnapshot& s)
{
    settleStartZone(s);
    std::vector<Vec3> out;

    if (!startZone_)
    {
        return out;
    }

    if (surveyedQuestLog_ != s.player.questLog || surveyedLevel_ != s.player.level)
    {
        surveyedGivers_.clear();
        surveyedQuestLog_ = s.player.questLog;
        surveyedLevel_ = s.player.level;
    }

    for (const auto& u : s.units)
    {
        if (!u.isPlayer && u.isAlive() && !u.name.empty() && distance2D(u.position, s.player.position) < 100)
        {
            surveyedGivers_[u.entry] = u.questMarker == QuestMarker::Available;
        }
    }

    for (const auto& [npc, spawns] : spawns_)
    {
        const auto visible = std::find_if(s.units.begin(), s.units.end(), [npc](const auto& u) { return !u.isPlayer && u.entry == npc; });

        // Out of sight, gone to only for a quest whose lead-in is known done:
        // nothing else tells whether it has one for the character yet.
        if (offers(s, npc, visible == s.units.end()).empty())
        {
            continue;
        }

        if (const auto seen = surveyedGivers_.find(npc); seen != surveyedGivers_.end() && !seen->second)
        {
            continue;
        }

        if (visible != s.units.end())
        {
            if (visible->questMarker == QuestMarker::Available)
            {
                out.push_back(visible->position);
            }

            continue;
        }

        for (const auto& spawn : spawns)
        {
            if (canonicalZone(spawn.zone) != canonicalZone(*startZone_))
            {
                continue;
            }

            if (auto spot = zones_.toWorld(spawn.zone, spawn.x, spawn.y))
            {
                spot->z = s.player.position.z;
                out.push_back(*spot);
            }
        }
    }

    return out;
}

std::vector<VendorSpot> QuestSource::vendorSpots(const GameSnapshot& s)
{
    std::vector<VendorSpot> out;
    const auto faction = s.player.faction();
    // The zone the character stands in now, by the subzone the client reports.
    const std::uint32_t here = s.zoneId ? canonicalZone(s.zoneId) : 0;

    for (const auto& [entry, friendly] : vendors_)
    {
        if (faction == Faction::Alliance && friendly.find('A') == std::string::npos)
        {
            continue;
        }

        if (faction == Faction::Horde && friendly.find('H') == std::string::npos)
        {
            continue;
        }

        if (faction == Faction::Unknown || !s.mapId)
        {
            continue;
        }

        const auto found = spawns_.find(entry);

        if (found == spawns_.end())
        {
            continue;
        }

        for (std::size_t index = 0; index < found->second.size(); ++index)
        {
            const Spawn& spawn = found->second[index];

            if (mapOfZone_)
            {
                const auto map = mapOfZone_(spawn.zone);

                if (!map || *map != *s.mapId)
                {
                    continue;
                }
            }

            // Unless the character is up there already.
            if (reachedByLift(canonicalZone(spawn.zone)) && canonicalZone(spawn.zone) != here)
            {
                continue;
            }

            auto position = zones_.toWorld(spawn.zone, spawn.x, spawn.y);

            if (!position)
            {
                continue;
            }

            position->z = s.player.position.z;
            auto [cached, fresh] = vendorSurroundings_.try_emplace({entry, index});

            if (fresh)
            {
                for (const Unfriendly& u : unfriendly_[spawn.zone])
                {
                    const auto at = zones_.toWorld(spawn.zone, u.x, u.y);

                    if (at && distance2D(*at, *position) <= VendorSpot::kDangerRadius)
                    {
                        if (!u.toAlliance)
                        {
                            cached->second.first.push_back(u.level);
                        }

                        if (!u.toHorde)
                        {
                            cached->second.second.push_back(u.level);
                        }
                    }
                }
            }

            const auto& levels = faction == Faction::Alliance ? cached->second.first : cached->second.second;
            const int gray = grayLevel(s.player.level);
            const int hostiles = static_cast<int>(std::count_if(levels.begin(), levels.end(), [&](int level) { return level > gray; }));
            out.push_back({entry, npcNames_[entry], *position, repairers_[entry], here == 0 || canonicalZone(spawn.zone) == here, hostiles});
        }
    }

    std::sort(out.begin(), out.end(), [&](const VendorSpot& a, const VendorSpot& b) { return distance2D(a.position, s.player.position) < distance2D(b.position, s.player.position); });
    return out;
}

std::vector<TrainerSpot> QuestSource::classTrainers(const GameSnapshot& s)
{
    std::vector<TrainerSpot> out;
    const auto faction = s.player.faction();
    const std::string_view className = toString(s.player.playerClass);

    if (faction == Faction::Unknown || !s.mapId || s.player.playerClass == PlayerClass::Unknown)
    {
        return out;
    }

    for (const ClassTrainer& trainer : classTrainers_)
    {
        if (trainer.className != className || trainer.friendlyTo.find(faction == Faction::Alliance ? 'A' : 'H') == std::string::npos)
        {
            continue;
        }

        const auto found = spawns_.find(trainer.entry);

        if (found == spawns_.end())
        {
            continue;
        }

        for (const Spawn& spawn : found->second)
        {
            if (mapOfZone_)
            {
                const auto map = mapOfZone_(spawn.zone);

                if (!map || *map != *s.mapId)
                {
                    continue;
                }
            }

            if (auto position = zones_.toWorld(spawn.zone, spawn.x, spawn.y))
            {
                position->z = s.player.position.z;
                out.push_back({trainer.entry, npcNames_[trainer.entry], *position});
            }
        }
    }

    std::sort(out.begin(), out.end(), [&](const TrainerSpot& a, const TrainerSpot& b) { return distance2D(a.position, s.player.position) < distance2D(b.position, s.player.position); });
    return out;
}

std::vector<QuestTarget> QuestSource::targets(const GameSnapshot& s)
{
    calibrate(s);
    settleStartZone(s);
    noteQuestLog(s);
    noteObjects(s);

    // The client may report a subarea (Northshire Valley, for example),
    // whereas  stores spawns under its enclosing zone map.
    if (startZone_ && !zones_.find(*startZone_))
    {
        if (const auto canon = canonicalZone(*startZone_); canon != *startZone_)
        {
            startZone_ = canon;
        }
        else if (const auto enclosing = enclosingZone(s))
        {
            startZone_ = enclosing;
        }
    }

    std::vector<QuestTarget> out;

    for (const auto id : s.player.questLog)
    {
        if (!questInStartZone(id) || ignoredQuests_.contains(id) || isAutoIgnored(id))
        {
            continue;
        }

        const auto quest = quests_.find(id);

        if (quest == quests_.end())
        {
            continue;
        }

        const Quest& q = quest->second;
        const auto progress = std::find_if(s.player.questProgress.begin(), s.player.questProgress.end(), [&](const auto& p) { return p.questId == id; });
        QuestTarget target{id, q.name, {}, {}};
        target.level = q.level;
        target.minLevel = q.minLevel;

        if (const auto ender = enders_.find(id); ender != enders_.end())
        {
            target.handInEntry = ender->second;

            if (const auto name = npcNames_.find(ender->second); name != npcNames_.end())
            {
                target.handInTo = name->second;
            }

            // Its spawn nearest the character, on this continent.
            if (const auto spawns = spawns_.find(ender->second); spawns != spawns_.end())
            {
                bool hasStartZoneSpawn = false;

                if (startZone_)
                {
                    const auto canonStart = canonicalZone(*startZone_);

                    for (const auto& spawn : spawns->second)
                    {
                        if (canonicalZone(spawn.zone) == canonStart)
                        {
                            hasStartZoneSpawn = true;
                            break;
                        }
                    }
                }

                for (const auto& spawn : spawns->second)
                {
                    if (hasStartZoneSpawn && startZone_ && canonicalZone(spawn.zone) != canonicalZone(*startZone_))
                    {
                        continue;
                    }

                    if (mapOfZone_ && s.mapId)
                    {
                        if (const auto map = mapOfZone_(spawn.zone); !map || *map != *s.mapId)
                        {
                            continue;
                        }
                    }

                    auto at = zones_.toWorld(spawn.zone, spawn.x, spawn.y);

                    if (!at)
                    {
                        continue;
                    }

                    at->z = s.player.position.z;

                    if (!target.handInAt || distance2D(*at, s.player.position) < distance2D(*target.handInAt, s.player.position))
                    {
                        target.handInAt = at;
                        target.handInInZone = s.zoneId && canonicalZone(spawn.zone) == canonicalZone(s.zoneId);
                    }
                }
            }
        }

        const auto describe = [](const std::string& name, int have, int wanted) { return std::format("{} {}/{}", name, have, wanted > 0 ? std::to_string(wanted) : "?"); };
        // The game's count where its UI showed one: the data's is read from
        // the quest text, and one without a number in it was taken for 1
        // (A Net Disaster's 7 Half-Eaten Fish).
        const auto lower = [](std::string text)
        {
            for (char& c : text)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            return text;
        };
        // The quest tracker's lines for this quest, where it shows them.
        const auto trackedIt = s.player.trackedQuests.find(lower(q.name));
        const auto* tracked = trackedIt != s.player.trackedQuests.end() ? &trackedIt->second : nullptr;
        const auto gameCount = [&](const std::string& name, int data)
        {
            const std::string key = lower(name);

            if (tracked)
            {
                for (const auto& line : *tracked)
                {
                    if (lower(line.name) == key && line.want > 0)
                    {
                        return line.want;
                    }
                }
            }

            const auto found = s.player.objectiveWanted.find(key);
            return found != s.player.objectiveWanted.end() && found->second > 0 ? found->second : data;
        };
        // The tracker's line for an objective, by its name; noted as one of
        // the data's objectives.
        std::vector<bool> lineKnown(tracked ? tracked->size() : 0, false);
        // (An object to use by the start of its line: "Tarantula Egg Destroyed".)
        const auto trackedLine = [&](const std::string& name, bool startsWith = false) -> const PlayerState::TrackedObjective*
        {
            const std::string key = lower(name);

            for (std::size_t i = 0; tracked && !key.empty() && i < tracked->size(); ++i)
            {
                if (const std::string line = lower((*tracked)[i].name); line == key || (startsWith && line.starts_with(key + " ")))
                {
                    lineKnown[i] = true;
                    return &(*tracked)[i];
                }
            }

            return nullptr;
        };
        // Only what is still wanted: an objective whose count is reached is done.
        bool allDone = true;

        for (std::size_t k = 0; k < q.kill.size(); ++k)
        {
            const auto name = npcNames_.find(q.kill[k]);
            const int wanted = gameCount(name != npcNames_.end() ? name->second : "", k < q.killCounts.size() ? q.killCounts[k] : 0);
            int killed = progress != s.player.questProgress.end() && k < progress->kills.size() ? progress->kills[k] : 0;

            // The tracker's own count while it shows the quest: the log's
            // counters go by the game's objectives, and the data may lack one
            // (Alther's Mill's Tarantula Eggs: 12 Greater Tarantulas killed
            // stood as 1/12).
            // (With an object to use among them, the tracker's last count
            // rather than a counter that may be another objective's.)
            if (const auto* line = trackedLine(name != npcNames_.end() ? name->second : ""); line && (line->shown || !q.use.empty()))
            {
                killed = line->have;
            }

            target.progress.push_back(describe(name != npcNames_.end() ? name->second : "mob", killed, wanted));

            if (wanted != 0 && killed >= wanted)
            {
                continue;
            }

            allDone = false;
            target.mobs.push_back(q.kill[k]);
        }

        for (std::size_t k = 0; k < q.items.size(); ++k)
        {
            const auto name = itemNames_.find(q.items[k]);
            const int wanted = gameCount(name != itemNames_.end() ? name->second : "", k < q.itemCounts.size() ? q.itemCounts[k] : 0);
            trackedLine(name != itemNames_.end() ? name->second : "");
            int carried = 0;

            for (const auto& stack : s.player.bagItems)
            {
                if (stack.itemId == q.items[k])
                {
                    carried += stack.count;
                }
            }

            target.progress.push_back(describe(name != itemNames_.end() ? name->second : "item", carried, wanted));

            if (wanted != 0 && carried >= wanted)
            {
                continue;
            }

            allDone = false;

            if (const auto drops = drops_.find(q.items[k]); drops != drops_.end())
            {
                target.mobs.insert(target.mobs.end(), drops->second.begin(), drops->second.end());
            }

            if (const auto objects = objectDrops_.find(q.items[k]); objects != objectDrops_.end())
            {
                for (const auto entry : objects->second)
                {
                    target.objects.push_back({entry, q.items[k], wanted});
                }
            }
        }

        // Objects to use for credit, counted by the tracker alone: which of
        // the log's counters is theirs the data does not tell.
        // An object whose line the tracker words its own way - "Followed
        // Kobold Tracks" for the Kobold Tracks - is found by its name anywhere
        // in a line no other objective has. One with no line of its own at
        // all goes by the lines the data has no objective for: done when
        // every one of those is.
        std::vector<std::uint32_t> unmatched;
        const auto lineNaming = [&](const std::string& name) -> const PlayerState::TrackedObjective*
        {
            const std::string key = lower(name);

            for (std::size_t i = 0; tracked && !key.empty() && i < tracked->size(); ++i)
            {
                if (!lineKnown[i])
                {
                    const std::string line = lower((*tracked)[i].name);
                    if (line.find(key) != std::string::npos || key.find(line) != std::string::npos)
                    {
                        lineKnown[i] = true;
                        return &(*tracked)[i];
                    }

                    if ((line.starts_with("track") && key.find("track") != std::string::npos) ||
                        (line.find("track") != std::string::npos && key.find("track") != std::string::npos))
                    {
                        lineKnown[i] = true;
                        return &(*tracked)[i];
                    }
                }
            }

            if (q.use.size() == 1 && q.kill.empty() && q.items.empty() && tracked && tracked->size() == 1 && !lineKnown[0])
            {
                lineKnown[0] = true;
                return &(*tracked)[0];
            }

            return nullptr;
        };

        for (std::size_t k = 0; k < q.use.size(); ++k)
        {
            const auto name = objectNames_.find(q.use[k]);
            const auto* line = trackedLine(name != objectNames_.end() ? name->second : "", true);
            line = line ? line : lineNaming(name != objectNames_.end() ? name->second : "");
            const int logCount = (q.kill.empty() && q.items.empty() && progress != s.player.questProgress.end() && k < progress->kills.size()) ? progress->kills[k] : 0;
            const int wanted = line && line->want > 0 ? line->want : (k < q.useCounts.size() ? q.useCounts[k] : 0);
            const int used = line ? (line->have > 0 ? line->have : std::max(line->have, logCount)) : logCount;
            target.progress.push_back(describe(name != objectNames_.end() ? name->second : (line ? line->name : "object"), used, wanted));

            if (wanted != 0 && used >= wanted)
            {
                continue;
            }

            allDone = false;

            if (const auto same = usedObjects_.find(q.use[k]); same != usedObjects_.end())
            {
                for (const auto entry : same->second)
                {
                    target.objects.push_back({entry, 0, wanted});

                    if (!line)
                    {
                        unmatched.push_back(entry);
                    }
                }
            }
            else if (!line)
            {
                unmatched.push_back(0);
            }
        }

        if (!unmatched.empty())
        {
            bool any = false, complete = true;

            for (std::size_t i = 0; tracked && i < tracked->size(); ++i)
            {
                if (!lineKnown[i])
                {
                    any = true;
                    complete = complete && (*tracked)[i].want > 0 && (*tracked)[i].have >= (*tracked)[i].want;
                }
            }

            if (any && complete)
            {
                std::erase_if(target.objects, [&](const QuestObjectTarget& object) { return object.itemId == 0 && std::find(unmatched.begin(), unmatched.end(), object.entry) != unmatched.end(); });
            }
            else
            {
                allDone = false;
            }
        }

        if (q.kill.empty() && q.items.empty() && q.use.empty())
        {
            const auto enderIt = enders_.find(id);
            const auto starterIt = starters_.find(id);
            const bool sameNpc = enderIt != enders_.end() && starterIt != starters_.end() && std::find(starterIt->second.begin(), starterIt->second.end(), enderIt->second) != starterIt->second.end();

            if (sameNpc)
            {
                // An exploration, event, or script quest from an NPC that cannot be
                // turned right back in to the same NPC without completing its objective.
                allDone = false;

                if (enderIt != enders_.end())
                {
                    const auto visibleEnder = std::find_if(s.units.begin(), s.units.end(), [&](const auto& u) { return !u.isPlayer && u.entry == enderIt->second; });

                    if (visibleEnder != s.units.end() && visibleEnder->questMarker == QuestMarker::Reward)
                    {
                        allDone = true;
                    }
                }
            }
        }

        // The tracker showing a line of it not yet done: not done, whatever
        // the data has - Shhh! We're Hunting Kobolds has no objective there,
        // was done the moment it was taken, and its hand-in failed.
        if (allDone && tracked && std::any_of(tracked->begin(), tracked->end(), [](const auto& line) { return line.have < line.want; }))
        {
            allDone = false;
        }

        // The tracker's lines the data has no objective for, while there is
        // something left to do: shown as they stand there.
        for (std::size_t i = 0; !allDone && tracked && i < tracked->size(); ++i)
        {
            if (!lineKnown[i])
            {
                target.progress.push_back(describe((*tracked)[i].name, (*tracked)[i].have, (*tracked)[i].want));
            }
        }

        // What the bot cannot count: left to the player.
        if (q.other)
        {
            continue;
        }

        target.done = allDone;
        std::sort(target.mobs.begin(), target.mobs.end());
        target.mobs.erase(std::unique(target.mobs.begin(), target.mobs.end()), target.mobs.end());

        if (target.done)
        {
            out.push_back(std::move(target));
            continue;
        }

        if (target.mobs.empty() && target.objects.empty())
        {
            continue; // no drop sources known
        }

        // One spot to every 10 yards: mobs stand in packs, and one visit looks at them all.
        std::map<std::pair<int, int>, std::size_t> cells;
        std::vector<int> levelCounts; // mobs of known level at each spot
        const auto addSpot = [&](const Vec3& world, float level)
        {
            const std::pair cell{static_cast<int>(std::floor(world.x / 10.0f)), static_cast<int>(std::floor(world.y / 10.0f))};
            const auto [at, added] = cells.emplace(cell, target.spots.size());

            if (added)
            {
                target.spots.push_back(world);
                target.spotLevels.push_back(0);
                levelCounts.push_back(0);
            }

            // The average level of the mobs there; 0 where none is known.
            if (level > 0)
            {
                float& average = target.spotLevels[at->second];
                int& count = levelCounts[at->second];
                average = (average * count + level) / (count + 1);
                ++count;
            }
        };
        const auto addSpots = [&](const std::vector<Spawn>& spawns, float level)
        {
            for (const auto& spawn : spawns)
            {
                // A quest taken during the session is worked on where it leads.
                if (startZone_ && !taken_.contains(id) && canonicalZone(spawn.zone) != canonicalZone(*startZone_))
                {
                    continue;
                }

                if (mapOfZone_ && s.mapId)
                {
                    if (const auto map = mapOfZone_(spawn.zone); !map || *map != *s.mapId)
                    {
                        continue;
                    }
                }

                auto world = zones_.toWorld(spawn.zone, spawn.x, spawn.y);

                if (!world)
                {
                    continue;
                }

                world->z = s.player.position.z;
                addSpot(*world, level);
            }
        };

        for (const auto mob : target.mobs)
        {
            const auto level = npcLevels_.find(mob);

            if (const auto spawns = spawns_.find(mob); spawns != spawns_.end())
            {
                addSpots(spawns->second, level != npcLevels_.end() ? level->second : 0.0f);
            }
        }

        for (const auto& object : target.objects)
        {
            if (const auto spawns = objectSpawns_.find(object.entry); spawns != objectSpawns_.end())
            {
                addSpots(spawns->second, 0.0f);
            }

            // Where the client showed one the data has nowhere for.
            if (const auto seen = seenObjects_.find(object.entry); seen != seenObjects_.end())
            {
                for (const auto& p : seen->second)
                {
                    // Not until the map is known: on another map's places
                    // the way was planned during a map change.
                    if (s.mapId && p.map == *s.mapId)
                    {
                        addSpot(p.at, 0.0f);
                    }
                }
            }
        }

        // Objects to use the data has nowhere for, and none seen yet: by the
        // quest's mobs, as Alther's Mill's eggs lie among its tarantulas.
        if (target.spots.empty() && !q.use.empty())
        {
            for (const auto mob : q.kill)
            {
                if (const auto spawns = spawns_.find(mob); spawns != spawns_.end())
                {
                    addSpots(spawns->second, 0.0f);
                }
            }
        }

        out.push_back(std::move(target));
    }

    return out;
}

} // namespace og
