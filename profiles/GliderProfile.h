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
 *  File: GliderProfile.h
 *
 *  Desc:
 *  Declarations and interface for GliderProfile (glider profile
 *  parsing, validation, and recording).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/navigation/ObstacleMemory.h"
#include "og/navigation/WaypointRoute.h"

#include <cstdint>
#include <algorithm>
#include <string>
#include <vector>

namespace og
{

struct FishingSpot
{
    std::string name;
    Vec3 position;
    float heading = 0.0f;
    float safeRadius = 15.0f;
    std::vector<std::string> tags;
};

// Classic Glider profile: where to go, what to kill, what to do with corpses.
struct GliderProfile
{
    std::string name;

    int minLevel = 1; // mob level range to attack
    int maxLevel = 255;

    std::vector<std::string> targets;    // mob names to attack (empty = every attackable mob)
    std::vector<std::string> exclusions; // never attack these

    std::vector<Vec3> waypoints;
    std::vector<Vec3> ghostWaypoints; // graveyard -> grind area, used during corpse runs
    std::vector<Vec3> vendorWaypoints;
    // Which NPC to sell to at the end of the vendor route. Empty means the
    // nearest vendor found there, which is enough when only one stands about.
    std::string vendorName;
    // The way to the vendor, as recorded.
    const std::vector<Vec3>& vendorRoute() const
    {
        return vendorWaypoints;
    }

    // User-painted areas, independent of automatically learned obstacles.
    std::vector<AvoidZone> noGoZones;
    RouteMode routeMode = RouteMode::Loop;

    struct Options
    {
        bool attackElites = false;
        // "Mob scan range": how far a mob may be to be pulled; 0 no limit.
        float pullDistance = 40.0f;
        // "Gather scan range": how far a herb or ore node may be to go for it;
        // nodes are sought far, mobs near. 0 no limit.
        float gatherDistance = 250.0f;
        // Stop the bot and log out once the character reaches this level;
        // 0 grinds on forever.
        int stopAtLevel = 0;
        // Never pull a mob below this level (it is still fought if it attacks);
        // 0 sets no limit beyond the profile's level range.
        int minPullLevel = 0;
        // Never pull a mob above this level (it is still fought if it attacks);
        // 0 sets no limit beyond the profile's level range.
        int maxPullLevel = 0;
        // Fish at the fishing spots (or where the character stands) instead
        // of grinding the route: Glider's "Fishing" profile.
        bool fish = false;
        // Put a lure on the pole this often; 0 never does.
        int lureMinutes = 0;
    } options;

    std::vector<FishingSpot> fishingSpots;

    // The zones the profile runs through (the client's AreaTable ids: 12
    // Elwynn Forest, 10 Duskwood, ...), gathered by the bot as it goes.
    std::vector<std::uint32_t> zoneIds;

    // Jumps, sidesteps and detours the bot found for obstacles on these routes.
    // Written by the bot itself; see ObstacleMemory.
    LearnedObstacles learned;
};

} // namespace og
