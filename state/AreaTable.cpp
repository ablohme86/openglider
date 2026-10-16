/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-01
 *  File: AreaTable.cpp
 *
 *  Desc:
 *  Implementation of AreaTable (game state representations, snapshots,
 *  and state providers).
 *
 *
 */
#include "og/state/AreaTable.h"

#include <atomic>
#include <memory>

namespace og
{

namespace
{

using SubzoneParents = std::unordered_map<std::uint32_t, std::uint32_t>;
std::atomic<std::shared_ptr<const SubzoneParents>> g_subzoneParents;

std::uint32_t builtInCanonicalZone(std::uint32_t zoneId);

} // namespace

void setSubzoneParents(std::unordered_map<std::uint32_t, std::uint32_t> parents)
{
    g_subzoneParents.store(parents.empty() ? nullptr : std::make_shared<const SubzoneParents>(std::move(parents)));
}

std::uint32_t canonicalZone(std::uint32_t zoneId)
{
    if (const auto parents = g_subzoneParents.load())
    {
        std::uint32_t zone = zoneId;
        bool known = false;

        // A subzone of a subzone leads on; no table is that deep.
        for (int depth = 0; depth < 8; ++depth)
        {
            const auto parent = parents->find(zone);

            if (parent == parents->end() || parent->second == zone || parent->second == 0)
            {
                break;
            }

            zone = parent->second;
            known = true;
        }

        if (known)
        {
            return zone;
        }
    }

    return builtInCanonicalZone(zoneId);
}

namespace
{

std::uint32_t builtInCanonicalZone(std::uint32_t zoneId)
{
    switch (zoneId)
    {
    // Durotar (14)
    case 363: // Valley of Trials
    case 362: // Echo Isles
    case 364: // Sen'jin Village
    case 365: // Razor Hill
    case 366: // Scuttle Coast
    case 367: // Tiragarde Keep
    case 368: // Skull Rock
    case 369: // Darkspear Strand
    case 370: // Bladefist Bay
    case 371: // Deadeye Shore
    case 372: // Southfury River
    case 373: // The Dry Hills
    case 374: // Dustwind Cave
    case 375: // Kolkar Crag
    case 377: // Rocktusk Farm
    case 378: // Razormane Grounds
    case 379: // Skull Rock (upper)
        return 14;

    // Dun Morogh (1)
    case 132: // Coldridge Valley
    case 131: // Anvilmar
    case 133: // Brewnall Village
    case 151: // Kharanos
    case 152: // Gol'Bolar Quarry
    case 153: // Frostmane Hold
    case 275: // The Grizzled Den
    case 809: // The Tundrid Hills
    case 810: // South Gate Pass
    case 811: // North Gate Pass
    case 812: // Ironband's Compound
        return 1;

    // Elwynn Forest (12)
    case 9:   // Northshire Valley
    case 22:  // Northshire Abbey
    case 42:  // Crystal Lake
    case 43:  // Mirror Lake
    case 87:  // Goldshire
    case 103: // Fargodeep Mine
    case 104: // Jasperlode Mine
    case 105: // Forest's Edge
    case 106: // Brackwell Pumpkin Patch
    case 107: // Stonefield Farm
    case 108: // Maclure Vineyards
    case 109: // Tower of Azora
    case 110: // Westbrook Garrison
    case 111: // Eastvale Logging Camp
    case 112: // Ridgepoint Tower
    case 113: // Heroes' Vigil
    case 114: // Furlbrow's Pumpkin Farm
        return 12;

    // Tirisfal Glades (85)
    case 154: // Deathknell
    case 155: // Brill
    case 156: // Agamand Mills
    case 157: // Solliden Farmstead
    case 158: // Nightmare Vale
    case 159: // Venomweb Vale
    case 160: // Garren's Haunt
    case 161: // Brightwater Lake
    case 162: // Calston Estate
    case 163: // Crusader Outpost
    case 164: // Gunther's Retreat
    case 165: // Stillwater Pond
        return 85;

    // Mulgore (215)
    case 220: // Camp Narache / Red Cloud Mesa
    case 221: // Bloodhoof Village
    case 222: // Stonebull Lake
    case 223: // Ravaged Caravan
    case 224: // Red Rocks
    case 225: // The Golden Plains
    case 226: // The Rolling Plains
        return 215;

    // Teldrassil (141)
    case 188: // Shadowglen
    case 186: // Dolanaar
    case 187: // Aldrassil
    case 189: // Starbreeze Village
    case 190: // Gnarlpine Hold
    case 191: // Ban'ethil Barrow Den
    case 192: // The Cleft
    case 193: // The Oracle Glade
    case 195: // Lake Al'Ameth
    case 196: // Wellspring Lake
        return 141;

    // Eversong Woods (3430)
    case 3431: // Sunstrider Isle
    case 3485: // The Living Wood
    case 3486: // Silvermoon City Suburb
        return 3430;

    // Azuremyst Isle (3524)
    case 3526: // Ammen Vale
    case 3527: // Odesyus' Landing
        return 3524;

    // Ghostlands (3433)
    case 3434: // Tranquillien
        return 3433;

    // Bloodmyst Isle (3525)
    case 3528: // Blood Watch
        return 3525;

    // Westfall (40)
    case 20: // Moonbrook
    case 24: // Fallbrow Farm
    case 25: // Sentinel Hill
    case 26: // Alexston Farmstead
    case 27: // The Gold Coast Quarry
    case 88: // The Dust Plains
        return 40;

    // Loch Modan (38)
    case 268: // Thelsamar
        return 38;

    // Darkshore (148)
    case 230: // Auberdine
    case 231: // Ruins of Mathystra
        return 148;

    // The Barrens (17)
    case 380: // The Crossroads
    case 381: // Camp Taurajo
    case 382: // Ratchet
    case 383: // The Mor'shan Rampart
    case 384: // Forgetron Geyser
    case 385: // Northwatch Hold
    case 386: // The Sludge Fen
    case 387: // Dreadmist Peak
    case 388: // Bael Modan
        return 17;

    // Silverpine Forest (130)
    case 166: // The Sepulcher
    case 167: // Ambermill
    case 168: // Pyrewood Village
    case 169: // The Ivar Patch
    case 170: // The Skittering Dark
        return 130;

    default:
        return zoneId;
    }
}

} // namespace

} // namespace og
