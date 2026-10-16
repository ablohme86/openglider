/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-09
 *  File: QuestFocus.cpp
 *
 *  Desc:
 *  Implementation of QuestFocus (bot engine, behaviors and runtime
 *  orchestration).
 *
 *
 */
#include "og/core/QuestFocus.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>

namespace og
{

namespace
{

// What a quest costs is counted in yards of the way to it: everything else
// about it is so many yards more or less.
// Hand-ins this near each other are one trip.
constexpr float kHub = 100.0f;
// A quest whose mobs the data has no place for: fought where they are met,
// and gone to only where there is nothing else.
constexpr float kNoPlace = 2000.0f;
// Each level of the quest above the lowest in the log: lower ones first,
// where they are about as near.
constexpr float kPerQuestLevel = 100.0f;
// Each level of its mobs more than one above the character.
constexpr float kPerLevelAbove = 300.0f;
// A quest all but done, against one not begun.
constexpr float kProgress = 80.0f;
// Each other quest with a place near its own, up to kNeighbours of them.
constexpr float kNeighbour = 60.0f;
constexpr int kNeighbours = 3;
// Each other quest ready for the same place: the trip pays the more.
constexpr float kReadyTogether = 150.0f;
// A log this full has no room for the quests met: hand in first.
constexpr std::size_t kLogNearlyFull = 18;
constexpr float kLogFull = 1000.0f;
// The one chosen last time keeps its place against one barely better.
constexpr float kUnderWay = 50.0f;
// Quests more than this many levels above the lowest one still to do wait for
// it, however near: a level 7 quest taken in Goldshire did not go before the
// level 4 ones left in Northshire.
constexpr int kLevelBand = 1;

const Vec3* nearestSpot(const QuestTarget& quest, const Vec3& to)
{
    const Vec3* best = nullptr;

    for (const auto& spot : quest.spots)
    {
        if (!best || distance2D(spot, to) < distance2D(*best, to))
        {
            best = &spot;
        }
    }

    return best;
}

float nearTo(const QuestTarget& quest, const Vec3& to)
{
    const Vec3* spot = nearestSpot(quest, to);
    return spot ? distance2D(*spot, to) : std::numeric_limits<float>::max();
}

// How far along it is, 0 to 1, from "Tough Wolf Meat 3/8"; an objective that
// does not say how many ("5/?") counts as not begun.
float progressOf(const QuestTarget& quest)
{
    float sum = 0.0f;
    int objectives = 0;

    for (const std::string& line : quest.progress)
    {
        ++objectives;
        const auto space = line.rfind(' ');
        const auto slash = line.rfind('/');

        if (slash == std::string::npos || (space != std::string::npos && space > slash))
        {
            continue;
        }

        const char* begin = line.data() + (space == std::string::npos ? 0 : space + 1);
        const char* mid = line.data() + slash;
        const char* end = line.data() + line.size();
        int have = 0;
        int want = 0;

        if (std::from_chars(begin, mid, have).ec != std::errc{} || std::from_chars(mid + 1, end, want).ec != std::errc{} || want <= 0)
        {
            continue;
        }

        sum += std::clamp(static_cast<float>(have) / static_cast<float>(want), 0.0f, 1.0f);
    }

    return objectives > 0 ? sum / static_cast<float>(objectives) : 0.0f;
}

// The level of the mobs where it would be worked on; the quest's where the
// data does not say.
float mobLevel(const QuestTarget& quest)
{
    const float level = lowestLevelSpots(quest, [](std::size_t) { return true; }).level;
    return level > 0 ? level : static_cast<float>(questLevel(quest));
}

} // namespace

std::optional<QuestFocusChoice> focusQuestsByArea(std::vector<QuestTarget>& quests, const QuestFocusInput& in, std::set<std::uint32_t>& underWay, const std::set<std::uint32_t>& heldBack)
{
    if (quests.empty())
    {
        underWay.clear();
        return std::nullopt;
    }

    // Which quests are in the choice at all: as focusQuests() has it.
    const bool anyFree = std::any_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return !heldBack.contains(q.questId); });
    const auto free = [&](const QuestTarget& q) { return !anyFree || !heldBack.contains(q.questId); };
    const bool anyPlace = std::any_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return free(q) && !questHasNowhereToGo(q); });
    QuestFocusChoice choice;

    if (!anyPlace)
    {
        focusQuests(quests, std::nullopt, heldBack);
        underWay.clear();

        for (const auto& q : quests)
        {
            if (q.focus)
            {
                choice.lead = choice.lead != 0 ? choice.lead : q.questId;
                ++choice.quests;
            }
        }

        return choice;
    }

    const auto counts = [&](const QuestTarget& q) { return free(q) && !questHasNowhereToGo(q); };
    const auto placed = [&](const QuestTarget& q) { return q.done ? q.handInAt.has_value() : !q.spots.empty(); };
    const Vec3& pos = in.position;
    int lowest = std::numeric_limits<int>::max();

    for (auto& q : quests)
    {
        q.focus = false;

        if (counts(q))
        {
            lowest = std::min(lowest, questLevel(q));
        }
    }

    // The lowest still to do, of those with a place to go to: done ones are
    // handed in, and one with no place is fought where met.
    int lowestOpen = std::numeric_limits<int>::max();

    for (const auto& q : quests)
    {
        if (counts(q) && !q.done && !q.spots.empty())
        {
            lowestOpen = std::min(lowestOpen, questLevel(q));
        }
    }

    const auto waits = [&](const QuestTarget& q) { return !q.done && lowestOpen != std::numeric_limits<int>::max() && questLevel(q) > lowestOpen + kLevelBand; };
    std::erase_if(underWay, [&](std::uint32_t id) { return std::none_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return q.questId == id && counts(q); }); });
    // The group under way goes on while a quest of it is still to do.
    const QuestTarget* lead = nullptr;
    float leadDistance = 0.0f;

    // A group of higher quests gives way to a lower one taken since, elsewhere.
    // Within its place one higher joins the lowest's trip, as below.
    const bool lowestOutside = std::any_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return counts(q) && !q.done && !q.spots.empty() && questLevel(q) == lowestOpen && !underWay.contains(q.questId); });

    if (lowestOutside && std::any_of(quests.begin(), quests.end(), [&](const QuestTarget& q) { return counts(q) && underWay.contains(q.questId) && !q.done && !q.spots.empty() && waits(q); }))
    {
        underWay.clear();
    }

    // A higher one of the group, waiting, does not lead it away from a lower.
    for (const auto& q : quests)
    {
        if (!counts(q) || q.done || q.spots.empty() || !underWay.contains(q.questId) || waits(q))
        {
            continue;
        }

        if (const float d = nearTo(q, pos); !lead || d < leadDistance)
        {
            lead = &q;
            leadDistance = d;
        }
    }

    if (!lead)
    {
        float best = 0.0f;

        for (const auto& q : quests)
        {
            if (!counts(q) || waits(q))
            {
                continue;
            }

            float way = kNoPlace;
            float cost = 0.0f;

            if (q.done)
            {
                way = q.handInAt ? distance2D(*q.handInAt, pos) : kNoPlace;
                const auto ready = std::count_if(quests.begin(), quests.end(), [&](const QuestTarget& o) { return &o != &q && counts(o) && o.done && o.handInAt && q.handInAt && distance2D(*o.handInAt, *q.handInAt) <= kHub; });
                cost = way - kReadyTogether * static_cast<float>(ready) - (in.logSize >= kLogNearlyFull ? kLogFull : 0.0f);
            }
            else
            {
                const Vec3* spot = nearestSpot(q, pos);
                way = spot ? distance2D(*spot, pos) : kNoPlace;
                const auto near = !spot ? 0 : std::count_if(quests.begin(), quests.end(), [&](const QuestTarget& o) { return &o != &q && counts(o) && !o.done && nearTo(o, *spot) <= kQuestArea; });
                cost = way + kPerQuestLevel * static_cast<float>(questLevel(q) - lowest) - kProgress * progressOf(q) - kNeighbour * static_cast<float>(std::min<std::ptrdiff_t>(near, kNeighbours));

                if (in.level > 0)
                {
                    cost += kPerLevelAbove * std::max(0.0f, mobLevel(q) - static_cast<float>(in.level) - 1.0f);
                }
            }

            if (underWay.contains(q.questId))
            {
                cost -= kUnderWay;
            }

            if (!lead || cost < best)
            {
                lead = &q;
                leadDistance = way;
                best = cost;
            }
        }

        underWay.clear();
    }

    choice.lead = lead->questId;
    choice.distance = leadDistance;
    choice.handIn = lead->done;
    const Vec3* leadSpot = lead->done ? nullptr : nearestSpot(*lead, pos);
    const Vec3 at = lead->done && lead->handInAt ? *lead->handInAt : leadSpot ? *leadSpot : pos;
    // One taken on the way joins only where it is no harder than what is
    // being done, or than the character.
    const int joinLevel = std::max(questLevel(*lead), in.level) + 1;

    for (auto& q : quests)
    {
        if (!counts(q))
        {
            continue;
        }

        bool with = &q == lead;

        if (!with && choice.handIn)
        {
            with = q.done && q.handInAt && distance2D(*q.handInAt, at) <= kHub;
        }
        else if (!with && !q.done)
        {
            with = underWay.contains(q.questId) || (questLevel(q) <= joinLevel && (q.spots.empty() || nearTo(q, at) <= kQuestArea || nearTo(q, pos) <= kQuestArea));
        }

        // On the way: its NPC is right here.
        const bool here = q.done && q.handInAt && distance2D(*q.handInAt, pos) <= kQuestHandInNear;

        if (with || here)
        {
            q.focus = true;
            ++choice.quests;
        }

        // The mobs of one with no place are fought where met; it holds no group together.
        if (with && placed(q))
        {
            underWay.insert(q.questId);
        }
    }

    return choice;
}

} // namespace og
