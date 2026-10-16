/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-04
 *  File: QuestCounts.h
 *
 *  Desc:
 *  Declarations and interface for QuestCounts (WoW client memory reading,
 *  process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og::wowclient
{

// How many of a quest objective the game asks for, as its UI shows it: the
// quest data's counts are read from the quest text, and one that gives none
// ("Recover Half-Eaten Fish from the nets") was taken for 1.
struct ObjectiveCount
{
    std::string name; // "Half-Eaten Fish", "Kobold Worker"
    int have = 0;
    int want = 0;
};

// A shown font string and where it is on the screen (0..1, from the bottom
// left); 0s where the client had not laid it out.
struct PlacedText
{
    std::string text;
    float bottom = 0;
    float left = 0;
};

// An objective line: the tracker's "1/7 Half-Eaten Fish", the message at the
// top of the screen "Half-Eaten Fish: 1/7", "Kobold Worker slain: 6/10".
// Colours and links are left out of the name, and " slain".
std::optional<ObjectiveCount> parseObjectiveCount(std::string_view line);

// The counts in the shown UI texts, by lower-case name; 0 for a name shown
// with two different counts (two quests, no telling which is whose). (Not the hand-in
// window's: it hides a count of 1 and keeps the last quest's number.)
std::map<std::string, int> objectiveCountsIn(const std::vector<PlacedText>& texts);

// The quest tracker: each quest's title (gold) and the objective lines under
// it, by lower-case title. A line belongs to the nearest title above it in
// its column, indented a little. A quest of no lines is not in it.
std::map<std::string, std::vector<ObjectiveCount>> trackedQuests(const std::vector<PlacedText>& texts);

} // namespace og::wowclient
