/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-13
 *  File: PlaystyleScript.h
 *
 *  Desc:
 *  Declarations and interface for PlaystyleScript (bot engine, behaviors,
 *  and core state machine).
 *
 *
 */
#pragma once

#include "og/core/Playstyle.h"

#include <string>
#include <string_view>
#include <vector>

namespace og
{

// A combat class script with the player's own way of fighting written into it
// ("Get Trained Data"): what they open with into `pull`, their rotation into
// `combat`, what they cast between fights into `rest`, and the health and mana
// they sit down and get up at into the settings.
//
// Only what was seen is rewritten. A rule for something the recorder never
// saw cast - auto attack, the pet, the wand, food and drink, a spell the
// player has not used - stays where it is; a rule for a spell it did see is
// replaced by what was learned of it, or dropped from a block the player does
// not use it in. The pull is the one exception: once the player's own openers
// are known, the class's other spells are no longer opened with.
struct TrainedScript
{
    std::string script;
    // What was written, a line each, for the player to read.
    std::vector<std::string> changes;
};

// `script` without its learned lines (see withoutTrained). The result has
// `changes` empty, and `script` as given, when the profile has too little to
// go by or the script names no class.
TrainedScript writeTrainedRules(std::string_view script, const PlaystyleProfile& profile);

// The name a spell is bound under: "Curse of Agony" -> "CurseOfAgony".
std::string playstyleActionName(std::string_view spellName);

} // namespace og
