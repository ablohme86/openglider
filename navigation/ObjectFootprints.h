/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-18
 *  File: ObjectFootprints.h
 *
 *  Desc:
 *  What a game object takes up on the ground, by its model: the pumpkins
 *  set out for Hallow's End, a mailbox, an anvil - things the server puts
 *  in the world, which the map's files (and so the navigation mesh) have not.
 *
 *
 */
#pragma once

#include "og/navigation/ClientGeometry.h"
#include "og/navigation/ObstacleMemory.h"
#include "og/state/GameSnapshot.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace og
{

class ObjectFootprints
{
public:
    // GameObjectDisplayInfo.db2 (dbfilesclient/gameobjectdisplayinfo.db2).
    static constexpr std::uint32_t kDisplayInfoFileDataId = 1266277;

    // Round the model's own collision, at scale 1: how far it reaches out
    // from where the object stands, and from how low to how high.
    struct Footprint
    {
        float radius = 0.0f;
        float bottom = 0.0f;
        float top = 0.0f;
    };

    // From the table's bytes; `read` gives a model's file by its FileDataID,
    // read the first time an object of that model is seen. Nullopt where the
    // table could not be read.
    static std::optional<ObjectFootprints> fromDb2(std::span<const std::uint8_t> displayInfo, ClientFileReader read, std::string& error);
    // A table already read: display ID -> model FileDataID (tests).
    ObjectFootprints(std::unordered_map<std::uint32_t, std::uint32_t> models, ClientFileReader read);

    // Nullopt where the model has no collision - a light hung in the air, a
    // ghost, a bat - or is unknown: nothing to go round.
    std::optional<Footprint> of(std::uint32_t displayId);

    std::size_t models() const
    {
        return models_.size();
    }

    // A zone round each object in the snapshot that stands where the
    // character walks, within `range` of it: a door, a chest or a herb is
    // gone to, not round, so only the ones there to look at.
    std::vector<AvoidZone> zones(const GameSnapshot& snapshot, float range = 60.0f);

private:
    std::unordered_map<std::uint32_t, std::uint32_t> models_;
    ClientFileReader read_;
    std::unordered_map<std::uint32_t, std::optional<Footprint>> known_;
};

} // namespace og
