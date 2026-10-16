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
 *  File: WaypointRoute.h
 *
 *  Desc:
 *  Declarations and interface for WaypointRoute (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace og
{

enum class RouteMode
{
    Loop,     // A -> B -> C -> D -> A -> ...
    PingPong, // A -> B -> C -> D -> C -> B -> A -> ...
    Once,     // A -> B -> C -> D, then finished
};

std::string_view toString(RouteMode mode);
RouteMode parseRouteMode(std::string_view s);

// Classic Glider waypoint route with a cursor to the current waypoint.
class WaypointRoute
{
public:
    WaypointRoute() = default;
    explicit WaypointRoute(std::vector<Vec3> points, RouteMode mode = RouteMode::Loop);

    bool empty() const
    {
        return points_.empty();
    }

    std::size_t size() const
    {
        return points_.size();
    }

    const std::vector<Vec3>& points() const
    {
        return points_;
    }

    RouteMode mode() const
    {
        return mode_;
    }

    std::size_t index() const
    {
        return index_;
    }

    const Vec3& current() const
    {
        return points_[index_];
    }

    bool finished() const
    {
        return finished_;
    }

    // Moves the cursor to the next waypoint according to the route mode.
    void advance();
    // Moves the cursor back to the previously visited waypoint.
    void retreat();
    void setIndex(std::size_t index);
    // Splices points into the route before `index`, keeping the cursor on the
    // waypoint it was already heading to. Used by the obstacle auto-fix.
    void insert(std::size_t index, const std::vector<Vec3>& points);

    std::size_t nearestIndex(const Vec3& position) const;
    // Resume at the nearest waypoint (after combat, looting, death...).
    void resumeFrom(const Vec3& position);

private:
    std::vector<Vec3> points_;
    RouteMode mode_ = RouteMode::Loop;
    std::size_t index_ = 0;
    int direction_ = 1;
    bool finished_ = false;
};

} // namespace og
