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
 *  File: Shapeshift.cpp
 *
 *  Desc:
 *  Implementation of Shapeshift (game state representations, snapshots,
 *  and state providers).
 *
 *
 */
#include "og/state/Shapeshift.h"

#include <algorithm>
#include <array>

namespace og
{

namespace
{

constexpr std::array kSpells{
    // A warrior learns the stances in this order, so they sit on it.
    ShapeshiftSpell{2457, "Battle Stance", "Warrior.BattleStance", 1, Stance::Battle}, ShapeshiftSpell{71, "Defensive Stance", "Warrior.DefensiveStance", 2, Stance::Defensive}, ShapeshiftSpell{2458, "Berserker Stance", "Warrior.BerserkerStance", 3, Stance::Berserker}, ShapeshiftSpell{5487, "Bear Form", "Druid.BearForm", 0, Stance::None, Form::Bear}, ShapeshiftSpell{9634, "Dire Bear Form", "Druid.DireBearForm", 0, Stance::None, Form::Bear}, ShapeshiftSpell{1066, "Aquatic Form", "Druid.AquaticForm", 0, Stance::None, Form::Aquatic}, ShapeshiftSpell{768, "Cat Form", "Druid.CatForm", 0, Stance::None, Form::Cat}, ShapeshiftSpell{783, "Travel Form", "Druid.TravelForm", 0, Stance::None, Form::Travel}, ShapeshiftSpell{24858, "Moonkin Form", "Druid.MoonkinForm", 0, Stance::None, Form::Moonkin}, ShapeshiftSpell{1784, "Stealth", "Rogue.Stealth", 1, Stance::None, Form::None, true},
};

// Each rank of Stealth and Prowl is an aura of its own.
constexpr std::array<std::uint32_t, 7> kStealthAuras{1784, 1785, 1786, 1787, 5215, 6783, 9913};

constexpr std::array<std::string_view, 4> kStanceNames{"None", "Battle", "Defensive", "Berserker"};
constexpr std::array<std::string_view, 6> kFormNames{"None", "Bear", "Aquatic", "Cat", "Travel", "Moonkin"};

bool has(std::span<const Aura> auras, std::uint32_t spellId)
{
    return std::any_of(auras.begin(), auras.end(), [&](const Aura& a) { return a.spellId == spellId; });
}

} // namespace

std::span<const ShapeshiftSpell> shapeshiftSpells()
{
    return kSpells;
}

Stance stanceOf(std::span<const Aura> auras)
{
    for (const auto& spell : kSpells)
    {
        if (spell.stance != Stance::None && has(auras, spell.spellId))
        {
            return spell.stance;
        }
    }

    return Stance::None;
}

Form formOf(std::span<const Aura> auras)
{
    for (const auto& spell : kSpells)
    {
        if (spell.form != Form::None && has(auras, spell.spellId))
        {
            return spell.form;
        }
    }

    return Form::None;
}

bool isStealthed(std::span<const Aura> auras)
{
    return std::any_of(kStealthAuras.begin(), kStealthAuras.end(), [&](std::uint32_t id) { return has(auras, id); });
}

const ShapeshiftSpell* shapeshiftFor(std::string_view action)
{
    const auto it = std::find_if(kSpells.begin(), kSpells.end(), [&](const auto& s) { return s.action == action; });
    return it == kSpells.end() ? nullptr : &*it;
}

bool alreadyIn(const ShapeshiftSpell& spell, std::span<const Aura> auras)
{
    if (spell.stealth)
    {
        return isStealthed(auras);
    }

    if (spell.stance != Stance::None)
    {
        return stanceOf(auras) == spell.stance;
    }

    return formOf(auras) == spell.form;
}

std::string_view toString(Stance stance)
{
    return kStanceNames[static_cast<std::size_t>(stance)];
}

std::string_view toString(Form form)
{
    return kFormNames[static_cast<std::size_t>(form)];
}

std::span<const std::string_view> stanceNames()
{
    return kStanceNames;
}

std::span<const std::string_view> formNames()
{
    return kFormNames;
}

int mainBarFirstSlot(std::span<const Aura> auras)
{
    switch (stanceOf(auras))
    {
    case Stance::Battle:
        return 73;
    case Stance::Defensive:
        return 85;
    case Stance::Berserker:
        return 97;
    case Stance::None:
        break;
    }

    switch (formOf(auras))
    {
    case Form::Cat:
        return 73;
    case Form::Bear:
        return 97;
    case Form::Moonkin:
        return 109;
    case Form::Aquatic:
    case Form::Travel:
    case Form::None:
        break;
    }

    // Stealth swaps the rogue's bar in; Prowl leaves the cat's.
    for (const std::uint32_t id : {1784u, 1785u, 1786u, 1787u})
    {
        if (has(auras, id))
        {
            return 73;
        }
    }

    return 1;
}

} // namespace og
