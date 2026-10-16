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
 *  File: WaypointRoute.cpp
 *
 *  Desc:
 *  Implementation of WaypointRoute (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/WaypointRoute.h"

#include <limits>

namespace og
{

std::string_view toString(RouteMode mode)
{
    switch (mode)
    {
    case RouteMode::Loop:
        return "loop";
    case RouteMode::PingPong:
        return "pingpong";
    case RouteMode::Once:
        return "once";
    }

    return "loop";
}

RouteMode parseRouteMode(std::string_view s)
{
    if (s == "pingpong" || s == "PingPong" || s == "reverse")
    {
        return RouteMode::PingPong;
    }

    if (s == "once" || s == "Once")
    {
        return RouteMode::Once;
    }

    return RouteMode::Loop;
}

WaypointRoute::WaypointRoute(std::vector<Vec3> points, RouteMode mode) : points_(std::move(points)), mode_(mode)
{
}

void WaypointRoute::advance()
{
    if (points_.size() < 2)
    {
        return;
    }

    const auto last = points_.size() - 1;

    switch (mode_)
    {
    case RouteMode::Loop:
        index_ = (index_ + 1) % points_.size();
        break;
    case RouteMode::PingPong:
        if (direction_ > 0 && index_ == last)
        {
            direction_ = -1;
        }
        else if (direction_ < 0 && index_ == 0)
        {
            direction_ = 1;
        }

        index_ = static_cast<std::size_t>(static_cast<long>(index_) + direction_);
        break;
    case RouteMode::Once:
        if (index_ == last)
        {
            finished_ = true;
        }
        else
        {
            ++index_;
        }

        break;
    }
}

void WaypointRoute::retreat()
{
    if (points_.size() < 2)
    {
        return;
    }

    switch (mode_)
    {
    case RouteMode::Loop:
        index_ = (index_ + points_.size() - 1) % points_.size();
        break;
    case RouteMode::PingPong:
    case RouteMode::Once:
        if (index_ > 0)
        {
            --index_;
        }

        break;
    }
}

void WaypointRoute::setIndex(std::size_t index)
{
    if (points_.empty())
    {
        return;
    }

    index_ = index % points_.size();
    finished_ = false;
}

void WaypointRoute::insert(std::size_t index, const std::vector<Vec3>& points)
{
    if (points.empty())
    {
        return;
    }

    index = std::min(index, points_.size());
    points_.insert(points_.begin() + static_cast<std::ptrdiff_t>(index), points.begin(), points.end());

    // Everything from the insertion point on has shifted along; keep the cursor
    // pointing at the same waypoint so the bot does not skip or repeat one.
    if (index_ >= index)
    {
        index_ += points.size();
    }
}

std::size_t WaypointRoute::nearestIndex(const Vec3& position) const
{
    std::size_t best = 0;
    float bestDist = std::numeric_limits<float>::max();

    for (std::size_t i = 0; i < points_.size(); ++i)
    {
        const float d = distance(points_[i], position);

        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }

    return best;
}

void WaypointRoute::resumeFrom(const Vec3& position)
{
    setIndex(nearestIndex(position));
}

} // namespace og
