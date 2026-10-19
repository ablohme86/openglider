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
 *  File: GameSnapshot.h
 *
 *  Desc:
 *  Declarations and interface for GameSnapshot (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/state/UnitState.h"

#include <optional>
#include <vector>

namespace og
{

// The one standardized view of the game that all decision logic works on.
// Behaviors, combat classes, targeting and navigation only ever see this.
struct GameSnapshot
{
    TimePoint timestamp{};
    std::optional<std::uint32_t> mapId;
    std::uint32_t zoneId = 0;
    // What the minimap names the place: the subzone ("Northshire Valley"),
    // or the zone where there is none. Empty where the client's UI is not read.
    std::string zoneText;

    PlayerState player;
    std::vector<UnitState> units;         // every visible unit except the player
    std::vector<GameObjectState> objects; // nodes, chests, bobbers, ...
    Guid targetGuid = 0;
    FishingState fishing;
    // "Spell is not ready yet" / "Ability is not ready yet" errors the client
    // showed since the previous snapshot: the last spell press did nothing.
    int spellNotReadyErrors = 0;
    // "Out of range" / "Target too close" / "Target not in line of sight" errors since the previous
    // snapshot: the last spell press did nothing, and repeating it from the
    // same spot would fail the same way.
    int spellOutOfReachErrors = 0;
    // "Can't do that while moving" errors since the previous snapshot.
    int spellMovingErrors = 0;

    const UnitState* findUnit(Guid guid) const;
    const GameObjectState* findObject(Guid guid) const;
    const UnitState* target() const
    {
        return targetGuid ? findUnit(targetGuid) : nullptr;
    }

    const UnitState* pet() const;

    bool inCombat() const
    {
        return player.inCombat;
    }

    bool isDead() const
    {
        return player.isDeadOrGhost();
    }

    const Vec3& position() const
    {
        return player.position;
    }

    float heading() const
    {
        return player.heading;
    }

    // Living, non-friendly units that are fighting the player or the player's pet.
    std::vector<const UnitState*> attackers() const;
    bool isAttacker(const UnitState& unit) const;

    // Living, non-friendly units within `radius` of `point`.
    std::vector<const UnitState*> hostilesNear(const Vec3& point, float radius) const;
};

} // namespace og
