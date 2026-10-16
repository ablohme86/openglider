/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-12
 *  File: RouteCorrector.h
 *
 *  Desc:
 *  Declarations and interface for RouteCorrector (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/navigation/WaypointRoute.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace og
{

class Logger;

struct RouteCorrectorConfig
{
    bool enabled = false;
    // Distance between recorded points while working free of an obstacle.
    float sampleSpacing = 5.0f;
    // A detour point this close to a waypoint the route already has adds
    // nothing, so it is dropped.
    float minSpacing = 4.0f;
    // Reject the whole detour if going round costs more than this multiple of
    // the straight line. A recovery that wandered is not a route improvement.
    float maxDetourRatio = 3.0f;
    // Caps, so one bad patch of terrain cannot bloat the route.
    int maxPointsPerFix = 4;
    int maxFixes = 24;
};

// One remembered way around an obstacle.
struct RouteCorrection
{
    std::size_t index = 0;    // where the points went into the route
    Vec3 stuckAt;             // where the bot jammed
    std::vector<Vec3> detour; // the path that got it out
    Vec3 from;                // the leg the detour belongs to: its first waypoint...
    Vec3 to;                  // ...and the one it was heading for
};

// Watches a route being walked and, when the bot works itself free of an
// obstacle, remembers the way round so the next lap avoids it.
//
// Corrections are applied to the running route immediately and collected here
// as well, so the UI can show them and write them back to the profile on
// request. Nothing touches the profile on disk.
class RouteCorrector
{
public:
    void configure(const RouteCorrectorConfig& config)
    {
        config_ = config;
    }

    const RouteCorrectorConfig& config() const
    {
        return config_;
    }

    // Call every navigation tick with where the bot is and whether the
    // navigator is currently working itself free.
    void update(const Vec3& position, std::size_t waypointIndex, bool recovering);

    // Call when the waypoint the bot was heading to is reached. Splices the
    // recorded detour into `route` if it looks like an improvement; returns
    // true when it did.
    bool onWaypointReached(std::size_t index, WaypointRoute& route, Logger* log = nullptr);
    // The same for a path walked once, first point to last (a vendor trip, a
    // corpse run). Returns how many points went in before `index`.
    std::size_t onWaypointReached(std::size_t index, std::vector<Vec3>& path, Logger* log = nullptr);

    // Call when the route is abandoned (death, a vendor trip) so a half
    // recorded detour is not spliced in somewhere unrelated later.
    void abandon();

    const std::vector<RouteCorrection>& corrections() const
    {
        return corrections_;
    }

    bool empty() const
    {
        return corrections_.empty();
    }

    // True while a detour is being recorded.
    bool recording() const
    {
        return recording_;
    }

private:
    // The recorded way round the leg from -> to ending at `index`, if it makes one.
    std::optional<RouteCorrection> takeFix(std::size_t index, const Vec3& from, const Vec3& to, Logger* log);

    RouteCorrectorConfig config_;
    bool recording_ = false;
    std::size_t stuckIndex_ = 0;
    Vec3 stuckAt_;
    std::vector<Vec3> trail_;
    std::vector<RouteCorrection> corrections_;
};

} // namespace og
