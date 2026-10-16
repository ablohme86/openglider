/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-21
 *  File: ZoneMaps.cpp
 *
 *  Desc:
 *  Implementation of ZoneMaps (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ZoneMaps.h"

#include <unordered_map>

namespace og::wowclient
{

std::optional<std::uint32_t> mapForZone(std::uint32_t zoneId)
{
    static const std::unordered_map<std::uint32_t, std::uint32_t> kZones = {
        // Eastern Kingdoms
        {1, 0},    // Dun Morogh
        {3, 0},    // Badlands
        {4, 0},    // Blasted Lands
        {8, 0},    // Swamp of Sorrows
        {10, 0},   // Duskwood
        {11, 0},   // Wetlands
        {12, 0},   // Elwynn Forest
        {28, 0},   // Western Plaguelands
        {33, 0},   // Stranglethorn Vale
        {36, 0},   // Alterac Mountains
        {38, 0},   // Loch Modan
        {40, 0},   // Westfall
        {41, 0},   // Deadwind Pass
        {44, 0},   // Redridge Mountains
        {45, 0},   // Arathi Highlands
        {46, 0},   // Burning Steppes
        {47, 0},   // The Hinterlands
        {51, 0},   // Searing Gorge
        {85, 0},   // Tirisfal Glades
        {130, 0},  // Silverpine Forest
        {139, 0},  // Eastern Plaguelands
        {267, 0},  // Hillsbrad Foothills
        {1497, 0}, // Undercity
        {1519, 0}, // Stormwind City
        {1537, 0}, // Ironforge
        // Kalimdor
        {14, 1},   // Durotar
        {15, 1},   // Dustwallow Marsh
        {16, 1},   // Azshara
        {17, 1},   // The Barrens
        {141, 1},  // Teldrassil
        {148, 1},  // Darkshore
        {215, 1},  // Mulgore
        {331, 1},  // Ashenvale
        {357, 1},  // Feralas
        {361, 1},  // Felwood
        {400, 1},  // Thousand Needles
        {405, 1},  // Desolace
        {406, 1},  // Stonetalon Mountains
        {440, 1},  // Tanaris
        {490, 1},  // Un'Goro Crater
        {493, 1},  // Moonglade
        {618, 1},  // Winterspring
        {1377, 1}, // Silithus
        {1637, 1}, // Orgrimmar
        {1638, 1}, // Thunder Bluff
        {1657, 1}, // Darnassus
        // Outland, and the Blood and Draenei elf starting zones
        {3430, 530}, // Eversong Woods
        {3433, 530}, // Ghostlands
        {3483, 530}, // Hellfire Peninsula
        {3487, 530}, // Silvermoon City
        {3518, 530}, // Nagrand
        {3519, 530}, // Terokkar Forest
        {3520, 530}, // Shadowmoon Valley
        {3521, 530}, // Zangarmarsh
        {3522, 530}, // Blade's Edge Mountains
        {3523, 530}, // Netherstorm
        {3524, 530}, // Azuremyst Isle
        {3525, 530}, // Bloodmyst Isle
        {3557, 530}, // The Exodar
        {3703, 530}, // Shattrath City
        {4080, 530}, // Isle of Quel'Danas
        // Classic 1.60.1
        {16622, 2991}, // the new race's starting zone
    };
    const auto it = kZones.find(zoneId);

    if (it == kZones.end())
    {
        return std::nullopt;
    }

    return it->second;
}

} // namespace og::wowclient
