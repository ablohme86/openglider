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
 *  File: QuestTargets.h
 *
 *  Desc:
 *  Declarations and interface for QuestTargets (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/state/GameSnapshot.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace og
{

// An object to loot while this item is still needed. The bag count is checked
// again before interacting, so a cached quest target cannot over-collect it.
// With no item (0), an object to use for quest credit, while it is listed.
struct QuestObjectTarget
{
    std::uint32_t entry = 0;
    std::uint32_t itemId = 0;
    int wanted = 0;
    std::uint32_t questId = 0;
};

// Quest Mode (experimental): what a quest in the log has the character kill or
// loot, and where those mobs and quest objects are to be found.
struct QuestTarget
{
    std::uint32_t questId = 0;
    std::string questName;
    // The mobs to kill, and the ones that drop the items to loot.
    std::vector<std::uint32_t> mobs;
    // Where the mobs and objects stand; the height is the character's.
    std::vector<Vec3> spots;
    // One to each objective: "Tough Wolf Meat 3/8", "Kobold Vermin 5/10"
    // ("5/?" where the quest does not say how many).
    std::vector<std::string> progress;
    // Every objective reached: nothing left to kill or loot, ready to hand in.
    bool done = false;
    std::string handInTo; // who takes it back; empty when not known
    std::uint32_t handInEntry = 0;
    // Where that NPC stands (its spawn nearest the character); nullopt where
    // its zone is not placed.
    std::optional<Vec3> handInAt;
    // That NPC stands in the zone the character is in; false as well where
    // either zone is not known.
    bool handInInZone = false;
    std::vector<QuestObjectTarget> objects;
    int level = 0;
    int minLevel = 0;
    // One of the quests being worked on now (see focusQuests()).
    bool focus = false;
    // The average level of the mobs at each of spots (same order); 0 where
    // not known, as for an object. May be shorter than spots: then unknown.
    std::vector<float> spotLevels;
};

inline int questLevel(const QuestTarget& quest)
{
    return quest.level > 0 ? quest.level : quest.minLevel;
}

// Completed quests still count until handed in, including delivery quests.
// Once the quests to work on are chosen (focus), those are the ones.
inline bool isLowestLevelQuest(const QuestTarget& quest, const std::vector<QuestTarget>& quests)
{
    for (const auto& other : quests)
    {
        if (other.focus)
        {
            return quest.focus;
        }
    }

    for (const auto& other : quests)
    {
        if (questLevel(other) < questLevel(quest))
        {
            return false;
        }
    }

    return true;
}

// Marks the quests to work on: the lowest level in the log - but the level
// already being worked on stays until every quest of it is handed in, so one
// taken on the way (a lower one from a quest giver passed) waits its turn
// instead of breaking off the quest under way. Returns the level chosen.
// A quest the data has no place for - This Fruit Could Bite Back, whose
// Prickly Pear Fruit QuestieDB has nowhere - is left out of the choice: the
// lowest, it held quest mode up, with nowhere to go and every other quest
// waiting on it. Unless no quest has a place. One with mobs still counts:
// they are fought where they are met. Nor does a quest done that has nobody
// to hand it in to the data knows of: The Ravaged Caravan's first step ends
// at a supply crate, and the lowest, it held up Longwalker Malah and Rite of
// Wisdom, both ready to hand in, while the bot walked its grind route.
inline bool questHasNowhereToGo(const QuestTarget& q)
{
    return q.done ? !q.handInAt || q.handInEntry == 0 : q.spots.empty() && q.mobs.empty();
}

// A quest `heldBack` names (its hand-in failed a moment ago) is out of the
// choice too, while another is in it.
inline std::optional<int> focusQuests(std::vector<QuestTarget>& quests, std::optional<int> current, const std::set<std::uint32_t>& heldBack = {})
{
    if (quests.empty())
    {
        return std::nullopt;
    }

    const bool anyFree = std::any_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return !heldBack.contains(q.questId); });
    const auto free = [&](const QuestTarget& q) { return !anyFree || !heldBack.contains(q.questId); };
    const bool anyPlace = std::any_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return free(q) && !questHasNowhereToGo(q); });
    const auto counts = [&](const QuestTarget& q) { return free(q) && (!anyPlace || !questHasNowhereToGo(q)); };
    const bool stillThere = current && std::any_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return counts(q) && questLevel(q) == *current; });
    std::optional<int> lowest;

    for (const auto& q : quests)
    {
        if (counts(q))
        {
            lowest = std::min(lowest.value_or(questLevel(q)), questLevel(q));
        }
    }

    int level = *lowest;

    if (stillThere)
    {
        level = *current;
    }

    for (auto& q : quests)
    {
        q.focus = counts(q) && questLevel(q) == level;
    }

    return level;
}

// The spots of a quest fall into groups: spots within kGroupLink of one
// another are one group (one mine, one farm). Where its mobs stand in more
// than one, the group with the lowest mobs is the one to go to - Gold Dust
// drops in both Fargodeep Mine and Jasperlode Mine, and the bot went to
// Jasperlode, whose kobolds are a level or two higher. Returns, for each spot,
// whether it is in that group; only spots `usable` says may be gone to count,
// so a group whose spots were all just looked at gives way to the next.
// Where no level is known every usable spot is in.
struct SpotGroups
{
    std::vector<bool> chosen;
    int groups = 0;  // groups with a usable spot
    float level = 0; // the chosen group's average mob level; 0 if unknown
};
template <class Usable> SpotGroups lowestLevelSpots(const QuestTarget& quest, Usable usable)
{
    constexpr float kGroupLink = 60.0f;
    const std::size_t n = quest.spots.size();
    std::vector<int> group(n, -1);
    int groups = 0;

    for (std::size_t i = 0; i < n; ++i)
    {
        if (group[i] >= 0)
        {
            continue;
        }

        std::vector<std::size_t> open{i};
        group[i] = groups;

        while (!open.empty())
        {
            const std::size_t at = open.back();
            open.pop_back();

            for (std::size_t j = 0; j < n; ++j)
            {
                if (group[j] < 0 && distance2D(quest.spots[at], quest.spots[j]) <= kGroupLink)
                {
                    group[j] = groups;
                    open.push_back(j);
                }
            }
        }

        ++groups;
    }

    std::vector<float> sum(groups, 0), count(groups, 0);
    std::vector<bool> open(groups, false);

    for (std::size_t i = 0; i < n; ++i)
    {
        if (usable(i))
        {
            open[group[i]] = true;
        }

        if (i < quest.spotLevels.size() && quest.spotLevels[i] > 0)
        {
            sum[group[i]] += quest.spotLevels[i];
            ++count[group[i]];
        }
    }

    SpotGroups result;
    result.chosen.assign(n, false);
    std::optional<float> lowest;

    for (int g = 0; g < groups; ++g)
    {
        if (!open[g])
        {
            continue;
        }

        ++result.groups;

        if (count[g] > 0 && (!lowest || sum[g] / count[g] < *lowest))
        {
            lowest = sum[g] / count[g];
        }
    }

    if (lowest)
    {
        result.level = *lowest;
    }

    // Groups about as low as the lowest are as good: the nearest of them then.
    const auto low = [&](int g) { return !lowest || count[g] == 0 || sum[g] / count[g] <= *lowest + 0.25f; };

    for (std::size_t i = 0; i < n; ++i)
    {
        result.chosen[i] = usable(i) && low(group[i]);
    }

    return result;
}

struct VendorSpot
{
    std::uint32_t entry = 0;
    std::string name;
    Vec3 position;
    bool repairs = false;
    // It stands in the zone the character is in (Westfall, not Elwynn Forest
    // next door); true as well where the character's zone is not known.
    bool inZone = true;
    // Hostile mobs about it by the data (spawns within kVendorDangerRadius of
    // it, not friendly to the character's side) that are not grey to the
    // character: Defias Profiteer stands among the Defias of Moonbrook.
    int hostilesAbout = 0;
    static constexpr float kDangerRadius = 40.0f;
    // As many as this, and it is gone to only where there is no other.
    static constexpr int kDangerous = 3;
};

// A trainer of a class ("Paladin Trainer"), where the data has it stand.
struct TrainerSpot
{
    std::uint32_t entry = 0;
    std::string name;
    Vec3 position;
};

// Knows the quests (Questie's data): the quests in the character's log that
// have it kill or loot, finished ones too. Called from the bot's thread.
class IQuestSource
{
public:
    virtual ~IQuestSource() = default;
    virtual void setIgnoredQuests(const std::set<std::uint32_t>&)
    {
    }
    virtual std::vector<QuestTarget> targets(const GameSnapshot& snapshot) = 0;
    // An item's name, where the data has it; the client gives ids only.
    virtual const std::string* itemName(std::uint32_t) const
    {
        return nullptr;
    }

    virtual void setStartZone(std::uint32_t)
    {
    }

    virtual std::vector<Vec3> questGiverSpots(const GameSnapshot&)
    {
        return {};
    }

    virtual std::vector<VendorSpot> vendorSpots(const GameSnapshot&)
    {
        return {};
    }

    // The trainers of the character's class friendly to its side on its
    // continent, nearest first.
    virtual std::vector<TrainerSpot> classTrainers(const GameSnapshot&)
    {
        return {};
    }

    virtual bool hasPendingQuests(const GameSnapshot& snapshot)
    {
        return !targets(snapshot).empty();
    }

    // The names of the items the quests in the log ask for: never sold.
    virtual std::vector<std::string> questItems(const GameSnapshot&)
    {
        return {};
    }

    // The names of the quests this NPC offers the character now.
    virtual std::vector<std::string> offeredBy(const GameSnapshot& snapshot, std::uint32_t npcEntry)
    {
        (void)snapshot;
        (void)npcEntry;
        return {};
    }

    // Those, and the ones it starts for other races and classes by the data:
    // to look for in the window open, which says what is offered.
    virtual std::vector<std::string> mayOffer(const GameSnapshot& snapshot, std::uint32_t npcEntry)
    {
        return offeredBy(snapshot, npcEntry);
    }

    // The quest started by an item carried in inventory, if eligible and not already taken/done.
    virtual std::optional<std::uint32_t> questStartedByItem(const GameSnapshot& snapshot, std::uint32_t itemId)
    {
        (void)snapshot;
        (void)itemId;
        return std::nullopt;
    }

    virtual bool isDungeonQuest(std::uint32_t questId) const
    {
        (void)questId;
        return false;
    }

    virtual bool isEliteQuest(std::uint32_t questId) const
    {
        (void)questId;
        return false;
    }

    virtual bool isAutoIgnored(std::uint32_t questId) const
    {
        return isDungeonQuest(questId) || isEliteQuest(questId);
    }

    virtual void setEliteEntries(std::shared_ptr<const std::unordered_set<std::uint32_t>> entries)
    {
        (void)entries;
    }
};

} // namespace og
