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
 *  File: Types.cpp
 *
 *  Desc:
 *  Implementation of Types (game state representations, snapshots, and
 *  state providers).
 *
 *
 */
#include "og/state/Types.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace og
{

namespace
{
std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

std::string_view toString(PlayerClass c)
{
    switch (c)
    {
    case PlayerClass::Warrior:
        return "Warrior";
    case PlayerClass::Paladin:
        return "Paladin";
    case PlayerClass::Hunter:
        return "Hunter";
    case PlayerClass::Rogue:
        return "Rogue";
    case PlayerClass::Priest:
        return "Priest";
    case PlayerClass::DeathKnight:
        return "DeathKnight";
    case PlayerClass::Shaman:
        return "Shaman";
    case PlayerClass::Mage:
        return "Mage";
    case PlayerClass::Warlock:
        return "Warlock";
    case PlayerClass::Druid:
        return "Druid";
    case PlayerClass::Unknown:
        break;
    }

    return "Unknown";
}

PlayerClass parsePlayerClass(std::string_view s)
{
    const auto l = lower(s);

    if (l == "warrior")
    {
        return PlayerClass::Warrior;
    }

    if (l == "paladin")
    {
        return PlayerClass::Paladin;
    }

    if (l == "hunter")
    {
        return PlayerClass::Hunter;
    }

    if (l == "rogue")
    {
        return PlayerClass::Rogue;
    }

    if (l == "priest")
    {
        return PlayerClass::Priest;
    }

    if (l == "deathknight" || l == "death knight")
    {
        return PlayerClass::DeathKnight;
    }

    if (l == "shaman")
    {
        return PlayerClass::Shaman;
    }

    if (l == "mage")
    {
        return PlayerClass::Mage;
    }

    if (l == "warlock")
    {
        return PlayerClass::Warlock;
    }

    if (l == "druid")
    {
        return PlayerClass::Druid;
    }

    return PlayerClass::Unknown;
}

std::string_view toString(PowerType p)
{
    switch (p)
    {
    case PowerType::Mana:
        return "Mana";
    case PowerType::Rage:
        return "Rage";
    case PowerType::Focus:
        return "Focus";
    case PowerType::Energy:
        return "Energy";
    case PowerType::Happiness:
        return "Happiness";
    case PowerType::Runes:
        return "Runes";
    case PowerType::RunicPower:
        return "RunicPower";
    }

    return "Mana";
}

std::string_view toString(Reaction r)
{
    switch (r)
    {
    case Reaction::Hostile:
        return "Hostile";
    case Reaction::Neutral:
        return "Neutral";
    case Reaction::Friendly:
        return "Friendly";
    case Reaction::Unknown:
        break;
    }

    return "Unknown";
}

Reaction parseReaction(std::string_view s)
{
    const auto l = lower(s);

    if (l == "hostile")
    {
        return Reaction::Hostile;
    }

    if (l == "neutral")
    {
        return Reaction::Neutral;
    }

    if (l == "friendly")
    {
        return Reaction::Friendly;
    }

    return Reaction::Unknown;
}

std::string_view toString(Classification c)
{
    switch (c)
    {
    case Classification::Normal:
        return "Normal";
    case Classification::Elite:
        return "Elite";
    case Classification::RareElite:
        return "RareElite";
    case Classification::WorldBoss:
        return "WorldBoss";
    case Classification::Rare:
        return "Rare";
    }

    return "Normal";
}

Classification parseClassification(std::string_view s)
{
    const auto l = lower(s);

    if (l == "elite")
    {
        return Classification::Elite;
    }

    if (l == "rareelite")
    {
        return Classification::RareElite;
    }

    if (l == "worldboss")
    {
        return Classification::WorldBoss;
    }

    if (l == "rare")
    {
        return Classification::Rare;
    }

    return Classification::Normal;
}

std::string_view toString(GameObjectKind k)
{
    switch (k)
    {
    case GameObjectKind::Generic:
        return "Generic";
    case GameObjectKind::Door:
        return "Door";
    case GameObjectKind::Chest:
        return "Chest";
    case GameObjectKind::Herb:
        return "Herb";
    case GameObjectKind::Mineral:
        return "Mineral";
    case GameObjectKind::FishingBobber:
        return "FishingBobber";
    case GameObjectKind::FishingHole:
        return "FishingHole";
    case GameObjectKind::Unknown:
        break;
    }

    return "Unknown";
}

GameObjectKind parseGameObjectKind(std::string_view s)
{
    const auto l = lower(s);

    if (l == "generic")
    {
        return GameObjectKind::Generic;
    }

    if (l == "door")
    {
        return GameObjectKind::Door;
    }

    if (l == "chest")
    {
        return GameObjectKind::Chest;
    }

    if (l == "herb")
    {
        return GameObjectKind::Herb;
    }

    if (l == "mineral")
    {
        return GameObjectKind::Mineral;
    }

    if (l == "fishingbobber")
    {
        return GameObjectKind::FishingBobber;
    }

    if (l == "fishinghole")
    {
        return GameObjectKind::FishingHole;
    }

    return GameObjectKind::Unknown;
}

} // namespace og
