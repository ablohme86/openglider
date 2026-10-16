/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-27
 *  File: Shapeshift.h
 *
 *  Desc:
 *  Declarations and interface for Shapeshift (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include "og/state/UnitState.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace og
{

// Warrior stances and druid forms: the character is in at most one of each.
enum class Stance
{
    None,
    Battle,
    Defensive,
    Berserker
};
enum class Form
{
    None,
    Bear,
    Aquatic,
    Cat,
    Travel,
    Moonkin
};

// A stance, form or Stealth the character enters by casting it. The order is
// the BarKeys addon's: the spell at index i has the addon's stance key i.
struct ShapeshiftSpell
{
    std::uint32_t spellId;
    std::string_view name;   // "Battle Stance"
    std::string_view action; // "Warrior.BattleStance"
    // Its button on the stance bar, where that does not depend on what else
    // the character has learned; 0 where it does (the druid's forms).
    int barButton = 0;
    Stance stance = Stance::None;
    Form form = Form::None;
    bool stealth = false;
};
std::span<const ShapeshiftSpell> shapeshiftSpells();

// What the character's auras put it in.
Stance stanceOf(std::span<const Aura> auras);
Form formOf(std::span<const Aura> auras);
// Stealth (rogue) or Prowl (druid).
bool isStealthed(std::span<const Aura> auras);

// The spell an action enters a stance, form or Stealth with; nullptr for any other action.
const ShapeshiftSpell* shapeshiftFor(std::string_view action);
// Pressing it now would take the character out again (a form, Stealth) or do
// nothing (the stance it is in): it is in what the action enters already.
bool alreadyIn(const ShapeshiftSpell& spell, std::span<const Aura> auras);

std::string_view toString(Stance stance);
std::string_view toString(Form form);
std::span<const std::string_view> stanceNames();
std::span<const std::string_view> formNames();

// The first action bar slot the main bar shows: a stance, form or Stealth swaps
// in a page of its own (Battle Stance: 73-84), and the main bar's keys press
// that page's slots. 1 without one.
int mainBarFirstSlot(std::span<const Aura> auras);

} // namespace og
