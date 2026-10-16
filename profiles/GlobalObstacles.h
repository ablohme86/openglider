/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-29
 *  File: GlobalObstacles.h
 *
 *  Desc:
 *  Declarations and interface for GlobalObstacles (glider profile
 *  parsing, validation, and recording).
 *
 *
 */
#pragma once

#include "og/navigation/ObstacleMemory.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace og
{

// One obstacle from any profile, or learned while botting: a spot free
// roaming got stuck at, a jump or sidestep, or a way round.
struct GlobalObstacle
{
    enum class Kind
    {
        AvoidZone,
        Maneuver,
        Detour
    };
    Kind kind = Kind::AvoidZone;
    // The continent, when known: profiles do not say, so what is read from
    // them has none until the bot learns the same spot while there.
    std::optional<std::uint32_t> mapId;
    // The zone (AreaTable id) it lies in: the client says only the player's
    // zone, so it is filled in once the bot stands near the spot.
    std::optional<std::uint32_t> zoneId;
    Vec3 position;                                      // the zone's centre, where the move starts, or where the bot got stuck
    float radius = 0.0f;                                // AvoidZone
    float heading = 0.0f;                               // Maneuver
    ObstacleManeuver maneuver = ObstacleManeuver::Jump; // Maneuver
    Vec3 from, to;                                      // Detour: the leg it goes round on
    std::vector<Vec3> points;                           // Detour
    std::string source;                                 // the profile file it came from, or where it was learned
};

// Every obstacle known from every profile, kept in one file
// (data/global_obstacles.json): new ones are added, none is ever dropped.
class GlobalObstacles
{
public:
    // Two of the same kind this close count as one.
    static constexpr float kSameSpot = 2.0f;

    // A file that is not there yet is an empty list.
    bool load(const std::filesystem::path& path, std::string* error = nullptr);
    bool save(const std::filesystem::path& path) const;

    // Adds what is not there yet, and fills in the map of one already there
    // that did not know it. Returns how many were added.
    int merge(const LearnedObstacles& learned, std::optional<std::uint32_t> mapId, const std::string& source);
    // Gives the obstacles on `mapId` within `radius` of `at` - where the player
    // stands in zone `zoneId` - that zone, where they have none yet. Returns
    // how many it gave one.
    int tagZone(std::uint32_t mapId, std::uint32_t zoneId, const Vec3& at, float radius = kZoneRadius);
    // Near enough the player to share their zone: zones are hundreds of yards
    // across, so only a spot right at a border is ever given the wrong one.
    static constexpr float kZoneRadius = 50.0f;
    // Reads every profile (*.json) in `directory` and merges its obstacles.
    int importProfiles(const std::filesystem::path& directory);

    const std::vector<GlobalObstacle>& obstacles() const
    {
        return obstacles_;
    }

    // Something was added or had its map filled in since the last load or save.
    bool changed() const
    {
        return changed_;
    }

private:
    bool add(GlobalObstacle obstacle);
    std::vector<GlobalObstacle> obstacles_;
    mutable bool changed_ = false;
};

} // namespace og
