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
 *  File: ItemNames.cpp
 *
 *  Desc:
 *  Implementation of ItemNames (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ItemNames.h"

#include "og/state/UnitState.h"

#include <unordered_map>

namespace og::wowclient
{

namespace
{

const std::unordered_map<std::uint32_t, std::string>& table()
{
    static const std::unordered_map<std::uint32_t, std::string> names{
        // Conjure Water, rank 1-9
        {5350, "Conjured Water"},
        {2288, "Conjured Fresh Water"},
        {2136, "Conjured Purified Water"},
        {3772, "Conjured Spring Water"},
        {8077, "Conjured Mineral Water"},
        {8078, "Conjured Sparkling Water"},
        {8079, "Conjured Crystal Water"},
        {30703, "Conjured Mountain Spring Water"},
        {22018, "Conjured Glacier Water"},
        // Conjure Food, rank 1-8
        {5349, "Conjured Muffin"},
        {1113, "Conjured Bread"},
        {1114, "Conjured Rye"},
        {1487, "Conjured Pumpernickel"},
        {8075, "Conjured Sourdough"},
        {8076, "Conjured Sweet Roll"},
        {22895, "Conjured Cinnamon Roll"},
        {22019, "Conjured Croissant"},
        // Conjure Refreshment / Ritual of Refreshment (food and drink)
        {34062, "Conjured Mana Biscuit"},
        {43518, "Conjured Mana Pie"},
        {43523, "Conjured Mana Strudel"},
    };
    return names;
}

} // namespace

const std::string* itemName(std::uint32_t itemId)
{
    const auto it = table().find(itemId);
    return it == table().end() ? nullptr : &it->second;
}

// One list for the whole bot: the snapshot's water and food counts use it too.
bool isWaterItem(std::uint32_t itemId)
{
    return ::og::isWaterItem(itemId);
}

bool isFoodItem(std::uint32_t itemId)
{
    return ::og::isFoodItem(itemId);
}

} // namespace og::wowclient
