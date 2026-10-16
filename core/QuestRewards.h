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
 *  File: QuestRewards.h
 *
 *  Desc:
 *  Declarations and interface for QuestRewards (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/core/BotConfig.h"
#include "og/state/IGameStateProvider.h"

#include <cstddef>
#include <optional>
#include <tuple>
#include <vector>

namespace og
{

// Gear: the tooltip names a slot it goes in ("Feet", "One-Hand"...).
bool isGear(const UiItemTooltip& item);

// Which of a quest's rewards to take, by their tooltips (in the order shown):
// Highest Value the one that sells for the most. Gear For Character a piece
// of gear with no red line (usable by the character): first the armour of
// the best kind the class wears at its level - leather for a hunter, not
// the cloth it can wear too - or anything not armour (a weapon, a ring, a
// cloak); then the most of the class's main stats; then the most valuable.
// No gear to use: the one that sells for the most. Rewards without a
// tooltip are skipped; the first wins a tie. nullopt when none has a tooltip.
// An unknown class goes by the value of the usable gear alone.
// Bind on Equip, and not bound yet: "Binds when equipped", no "Soulbound".
bool bindsWhenEquipped(const UiItemTooltip& item);

// How good a piece of gear is for the character, to set beside what it wears
// in the slot (Auto Equip): ranked as chooseQuestReward ranks rewards - the
// armour of the kind the class wears first, then the class's main stats -
// then the armour it gives or a weapon's damage per second, then what it
// sells for. Higher is better. The lines a comparison tooltip adds after
// "If you replace this item" do not count.
std::tuple<int, int, int, int> gearRank(const UiItemTooltip& item, PlayerClass playerClass, int level);

std::optional<std::size_t> chooseQuestReward(const std::vector<std::optional<UiItemTooltip>>& rewards, QuestConfig::Reward policy, PlayerClass playerClass = PlayerClass::Unknown, int level = 0);

} // namespace og
