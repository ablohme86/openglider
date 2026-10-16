/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-22
 *  File: SpecialBags.cpp
 *
 *  Desc:
 *  Implementation of SpecialBags (game state representations,
 *  snapshots, and state providers).
 *
 *
 */
#include "og/state/SpecialBags.h"

#include <algorithm>
#include <array>

namespace og
{

namespace
{

// Sorted, for the binary search.
constexpr auto kSpecialBags = []
{
    std::array<std::uint32_t, 42> ids{
        // Herb bags
        22250,
        22251,
        22252,
        38225,
        // Soul bags
        21340,
        21341,
        21342,
        21872,
        22243,
        22244,
        // Enchanting bags
        21858,
        22246,
        22248,
        22249,
        // Mining bags
        29540,
        30746,
        // Gem bags
        24270,
        30747,
        // Engineering toolboxes
        23774,
        23775,
        30745,
        // Quivers and ammo pouches
        2101,
        2102,
        2662,
        2663,
        3573,
        3574,
        3604,
        3605,
        5439,
        5441,
        7278,
        7279,
        7371,
        7372,
        8217,
        8218,
        11362,
        11363,
        18714,
        19319,
        19320,
    };
    std::sort(ids.begin(), ids.end());
    return ids;
}();

} // namespace

bool isSpecialBag(std::uint32_t itemId)
{
    return std::binary_search(kSpecialBags.begin(), kSpecialBags.end(), itemId);
}

} // namespace og
