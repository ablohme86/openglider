/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: Types.h
 *
 *  Desc:
 *  Declarations and interface for Types (game state representations,
 *  snapshots, and state providers).
 *
 *
 */
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace og
{

using Guid = std::uint64_t;

inline std::string formatGuid(Guid guid)
{
    return std::format("0x{:X}", guid);
}

enum class PlayerClass : std::uint8_t
{
    Unknown = 0,
    Warrior = 1,
    Paladin = 2,
    Hunter = 3,
    Rogue = 4,
    Priest = 5,
    DeathKnight = 6,
    Shaman = 7,
    Mage = 8,
    Warlock = 9,
    Druid = 11,
};

enum class Faction : std::uint8_t
{
    Unknown,
    Alliance,
    Horde
};

// A player's side from the race id in UNIT_FIELD_BYTES_0 (the same on every client).
inline Faction factionOfRace(int race)
{
    switch (race)
    {
    case 1:
    case 3:
    case 4:
    case 7:
    case 11:
        return Faction::Alliance; // Human, Dwarf, Night Elf, Gnome, Draenei
    case 2:
    case 5:
    case 6:
    case 8:
    case 10:
        return Faction::Horde; // Orc, Undead, Tauren, Troll, Blood Elf
    default:
        return Faction::Unknown;
    }
}

// A player's side from standard faction template ids (used across standard WoW DBCs).
inline Faction factionOfTemplateId(std::uint32_t id)
{
    switch (id)
    {
    case 1:
    case 3:
    case 4:
    case 115:
    case 1629:
    case 2203:
    case 2439:
        return Faction::Alliance; // Human, Dwarf, Night Elf, Gnome, Draenei, Worgen, Pandaren (Alliance)
    case 2:
    case 5:
    case 6:
    case 116:
    case 1610:
    case 2204:
    case 2440:
        return Faction::Horde; // Orc, Undead, Tauren, Troll, Blood Elf, Goblin, Pandaren (Horde)
    default:
        return Faction::Unknown;
    }
}

// Matches WoW power indices (UNIT_FIELD_POWER1 + index).
enum class PowerType : std::uint8_t
{
    Mana = 0,
    Rage = 1,
    Focus = 2,
    Energy = 3,
    Happiness = 4,
    Runes = 5,
    RunicPower = 6,
};
inline constexpr std::size_t kPowerTypeCount = 7;

enum class Reaction : std::uint8_t
{
    Unknown,
    Hostile,
    Neutral,
    Friendly
};

enum class Classification : std::uint8_t
{
    Normal,
    Elite,
    RareElite,
    WorldBoss,
    Rare
};

enum class GameObjectKind : std::uint8_t
{
    Unknown,
    Generic,
    Door,
    Chest,
    Herb,
    Mineral,
    FishingBobber,
    FishingHole,
};

std::string_view toString(PlayerClass c);
PlayerClass parsePlayerClass(std::string_view s);
std::string_view toString(PowerType p);
std::string_view toString(Reaction r);
Reaction parseReaction(std::string_view s);
std::string_view toString(Classification c);
Classification parseClassification(std::string_view s);
std::string_view toString(GameObjectKind k);
GameObjectKind parseGameObjectKind(std::string_view s);

} // namespace og
