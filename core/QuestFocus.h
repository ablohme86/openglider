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
 *  File: QuestFocus.h
 *
 *  Desc:
 *  Declarations and interface for QuestFocus (bot engine, behaviors and
 *  runtime orchestration).
 *
 *
 */
#pragma once

#include "og/core/QuestTargets.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace og
{

// Where the character is, for choosing the quests to work on.
struct QuestFocusInput
{
    Vec3 position;
    int level = 0;           // the character's; 0 where not known
    std::size_t logSize = 0; // quests in the log, of 20
};

struct QuestFocusChoice
{
    std::uint32_t lead = 0; // the quest the others are grouped round
    float distance = 0.0f;  // to where it is worked on, or handed in
    bool handIn = false;    // the lead is done: a trip to hand in
    int quests = 0;         // how many are marked
};

// Marks the quests to work on (QuestTarget::focus), as focusQuests() does, but
// by what they cost rather than by the lowest level alone: the way there,
// the level of the quest and of its mobs against the character's, how far
// along it is, and the other quests with places near by. The best one leads,
// and the quests with a place within kQuestArea of its place are worked on
// with it - one trip to the mine for all three of its quests.
//
// `underWay` is the group being worked on, kept between calls: it stays until
// every quest of it is done, so one taken on the way does not break it off -
// unless its place is here too, and then it joins.
//
// A quest done is handed in on a trip of its own only when that is the best
// thing to do: several ready for the same place, a full log, or nothing
// nearer to work on. One whose NPC is within kQuestHandInNear of the
// character is handed in at once, being on the way.
//
// Where no quest has a place, the lowest level as focusQuests() has it.
inline constexpr float kQuestArea = 150.0f;
inline constexpr float kQuestHandInNear = 100.0f;
std::optional<QuestFocusChoice> focusQuestsByArea(std::vector<QuestTarget>& quests, const QuestFocusInput& in, std::set<std::uint32_t>& underWay, const std::set<std::uint32_t>& heldBack = {});

} // namespace og
