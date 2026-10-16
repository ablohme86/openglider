/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-12
 *  File: GatheringData.h
 *
 *  Desc:
 *  Declarations and interface for GatheringData (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/state/UnitState.h"
#include <string_view>

namespace og
{

// Returns the required Herbalism or Mining skill for the given game object.
// Returns 0 if kind is not Herb or Mineral.
// Returns 1 if kind is Herb or Mineral but the specific node name is unrecognized.
int requiredGatheringSkill(GameObjectKind kind, std::string_view name);

// Checks if the player meets the required profession skill to gather the node.
// If player.skills is empty (e.g. mock provider that doesn't populate skills),
// gathering is allowed.
bool canGatherNode(const GameObjectState& node, const PlayerState& player);

// How a herb or ore node stands against the player's skill, as the game colours
// it: red cannot be gathered, orange always raises the skill, yellow and green
// less and less often, gray never. Unknown when the skills are not read or the
// object is no herb or ore.
enum class GatherDifficulty
{
    Unknown,
    Red,
    Orange,
    Yellow,
    Green,
    Gray
};
GatherDifficulty gatherDifficulty(const GameObjectState& node, const PlayerState& player);

// Skinning skill a corpse of this level needs (1.12 through 3.3.5a): 1 up to
// level 10, then 10 per level to 100 at 20, then 5 per level. 0 when unknown.
int requiredSkinningSkill(int creatureLevel);

// Checks the player's Skinning against the corpse. Unknown skills allow it, and
// so does Herbalism or Mining without Skinning: from TBC on, the skinnable flag
// also marks herb and ore "skinning" on plants and elementals.
bool canSkinUnit(const UnitState& unit, const PlayerState& player);

} // namespace og
