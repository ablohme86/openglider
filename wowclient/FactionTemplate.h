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
 *  File: FactionTemplate.h
 *
 *  Desc:
 *  Declarations and interface for FactionTemplate (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/state/Types.h"

#include <array>
#include <cstdint>

namespace og::wowclient
{

// One FactionTemplate row: the faction groups a unit belongs to, and the groups
// and factions it helps or fights. Group bits: 1 player, 2 Alliance, 4 Horde,
// 8 monster.
struct FactionTemplate
{
    std::uint32_t id = 0;
    std::uint32_t faction = 0;
    std::uint32_t flags = 0;
    std::uint32_t factionGroupMask = 0;
    std::uint32_t friendGroupMask = 0;
    std::uint32_t enemyGroupMask = 0;
    std::array<std::uint32_t, 8> enemyFaction{}; // 3.3.5a stores four, the modern table eight
    std::array<std::uint32_t, 8> friendFaction{};

    bool isFriendlyTo(const FactionTemplate& other) const
    {
        if (other.faction != 0)
        {
            for (std::uint32_t e : enemyFaction)
            {
                if (e != 0 && e == other.faction)
                {
                    return false;
                }
            }

            for (std::uint32_t f : friendFaction)
            {
                if (f != 0 && f == other.faction)
                {
                    return true;
                }
            }
        }

        return (friendGroupMask & other.factionGroupMask) != 0 || (factionGroupMask & other.friendGroupMask) != 0;
    }

    bool isHostileTo(const FactionTemplate& other) const
    {
        if (other.faction != 0)
        {
            for (std::uint32_t e : enemyFaction)
            {
                if (e != 0 && e == other.faction)
                {
                    return true;
                }
            }

            for (std::uint32_t f : friendFaction)
            {
                if (f != 0 && f == other.faction)
                {
                    return false;
                }
            }
        }

        return (enemyGroupMask & other.factionGroupMask) != 0;
    }

    Faction side() const
    {
        if (factionGroupMask & 2)
        {
            return Faction::Alliance;
        }

        if (factionGroupMask & 4)
        {
            return Faction::Horde;
        }

        return Faction::Unknown;
    }
};

// The template flag of a unit that is hostile unless something says otherwise.
inline constexpr std::uint32_t kFactionHostileByDefault = 0x2000;

// How the unit with template `otherId` stands towards the character with
// `selfId`, as the client colours it: hostile when it is hostile to us (it
// attacks), friendly when either side is friendly, else neutral. Unknown when
// either id is 0 or a row is missing.
//
// Only the unit's side makes it hostile. A player's template is the enemy of
// every monster group, so asking both ways made a mob that leaves players be -
// Northshire's Kobold Vermin (template 25: a monster, no enemy of players) -
// come out hostile where the client shows it neutral.
inline Reaction reactionBetween(std::uint32_t selfId, const FactionTemplate* self, std::uint32_t otherId, const FactionTemplate* other)
{
    if (selfId == 0 || otherId == 0)
    {
        return Reaction::Unknown;
    }

    if (selfId == otherId)
    {
        return Reaction::Friendly;
    }

    if (!self || !other)
    {
        return Reaction::Unknown;
    }

    if (other->isHostileTo(*self))
    {
        return Reaction::Hostile;
    }

    if (self->isFriendlyTo(*other) || other->isFriendlyTo(*self))
    {
        return Reaction::Friendly;
    }

    if (other->flags & kFactionHostileByDefault)
    {
        return Reaction::Hostile;
    }

    return Reaction::Neutral;
}

// The FactionTemplate table of WoW 2.5.6 (build 69795), built in: that client
// keeps it in a DB2 with no known address.
const FactionTemplate* factionTemplate256(std::uint32_t id);

} // namespace og::wowclient
