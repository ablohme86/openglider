/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-14
 *  File: GameObjectKind.cpp
 *
 *  Desc:
 *  Implementation of GameObjectKind (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/GameObjectKind.h"

#include "og/wowclient/ClientLayout.h"

#include <initializer_list>

namespace og::wowclient
{

namespace c = consts;

namespace
{

bool containsAny(const std::string& s, std::initializer_list<const char*> words)
{
    for (const char* w : words)
    {
        if (s.find(w) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

} // namespace

bool keptAtAnyDistance(const GameObjectState& object)
{
    return object.kind == GameObjectKind::Transport || object.kind == GameObjectKind::Ship || object.name.find("Elevator") != std::string::npos || object.name.find("Lift") != std::string::npos;
}

GameObjectKind classifyGameObject(std::uint8_t type, const std::string& name)
{
    switch (type)
    {
    case c::GoTypeFishingNode:
        return GameObjectKind::FishingBobber;
    case c::GoTypeFishingHole:
        return GameObjectKind::FishingHole;
    case c::GoTypeDoor:
        return GameObjectKind::Door;
    case c::GoTypeTransport:
        return GameObjectKind::Transport;
    case c::GoTypeMoTransport:
        return GameObjectKind::Ship;
    case c::GoTypeChest:
        if (containsAny(name, {"Vein", "Deposit", "Lode"}))
        {
            return GameObjectKind::Mineral;
        }

        if (containsAny(name, {"Peacebloom", "Silverleaf", "Earthroot", "Mageroyal", "Briarthorn", "Stranglekelp", "Bruiseweed", "Wild Steelbloom", "Grave Moss", "Kingsblood", "Liferoot", "Fadeleaf", "Goldthorn", "Khadgar", "Firebloom", "Sungrass", "Gromsblood", "Purple Lotus", "Arthas' Tears", "Blindweed", "Ghost Mushroom", "Golden Sansam", "Dreamfoil", "Mountain Silversage", "Plaguebloom", "Icecap", "Black Lotus", "Felweed", "Dreaming Glory", "Ragveil", "Terocone", "Nightmare Vine", "Mana Thistle", "Flame Cap", "Ancient Lichen", "Netherbloom", "Firethorn", "Goldclover", "Tiger Lily", "Talandra", "Adder's Tongue", "Lichbloom", "Icethorn", "Frost Lotus", "Deadnettle", "Frozen Herb"}))
        {
            return GameObjectKind::Herb;
        }

        return GameObjectKind::Chest;
    default:
        return GameObjectKind::Generic;
    }
}

} // namespace og::wowclient
