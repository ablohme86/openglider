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
 *  File: RouteCorrector.cpp
 *
 *  Desc:
 *  Implementation of RouteCorrector (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/RouteCorrector.h"

#include "og/common/Log.h"

#include <algorithm>

namespace og
{

namespace
{

float pathLength(const Vec3& from, const std::vector<Vec3>& via, const Vec3& to)
{
    float total = 0.0f;
    Vec3 previous = from;

    for (const Vec3& point : via)
    {
        total += distance(previous, point);
        previous = point;
    }

    return total + distance(previous, to);
}

} // namespace

void RouteCorrector::update(const Vec3& position, std::size_t waypointIndex, bool recovering)
{
    if (!config_.enabled)
    {
        return;
    }

    if (recovering && !recording_)
    {
        // Just jammed: start remembering the way out.
        recording_ = true;
        stuckIndex_ = waypointIndex;
        stuckAt_ = position;
        trail_.clear();
        return;
    }

    if (!recording_)
    {
        return;
    }

    // Heading somewhere else now (combat pulled us away, the route moved on):
    // whatever we recorded no longer belongs to this leg.
    if (waypointIndex != stuckIndex_)
    {
        abandon();
        return;
    }

    // Sample the way out, thinly - this is a route, not a breadcrumb trail.
    const Vec3& last = trail_.empty() ? stuckAt_ : trail_.back();

    if (distance(last, position) >= config_.sampleSpacing)
    {
        trail_.push_back(position);
    }
}

void RouteCorrector::abandon()
{
    recording_ = false;
    trail_.clear();
}

bool RouteCorrector::onWaypointReached(std::size_t index, WaypointRoute& route, Logger* log)
{
    if (index >= route.size())
    {
        abandon();
        return false;
    }

    // The leg being fixed runs from the previous waypoint to this one.
    const auto& points = route.points();
    const Vec3 to = points[index];
    const Vec3 from = points[index == 0 ? points.size() - 1 : index - 1];
    auto fix = takeFix(index, from, to, log);

    if (!fix)
    {
        return false;
    }

    route.insert(index, fix->detour);
    corrections_.push_back(std::move(*fix));
    return true;
}

std::size_t RouteCorrector::onWaypointReached(std::size_t index, std::vector<Vec3>& path, Logger* log)
{
    // The first point is walked to from wherever the bot stood: no leg to fix.
    if (index == 0 || index >= path.size())
    {
        abandon();
        return 0;
    }

    auto fix = takeFix(index, path[index - 1], path[index], log);

    if (!fix)
    {
        return 0;
    }

    path.insert(path.begin() + static_cast<std::ptrdiff_t>(index), fix->detour.begin(), fix->detour.end());
    const std::size_t added = fix->detour.size();
    corrections_.push_back(std::move(*fix));
    return added;
}

std::optional<RouteCorrection> RouteCorrector::takeFix(std::size_t index, const Vec3& from, const Vec3& to, Logger* log)
{
    if (!config_.enabled || !recording_)
    {
        return std::nullopt;
    }

    if (index != stuckIndex_)
    {
        abandon();
        return std::nullopt;
    }

    std::vector<Vec3> detour = std::move(trail_);
    abandon();

    if (detour.empty())
    {
        return std::nullopt;
    }

    if (static_cast<int>(corrections_.size()) >= config_.maxFixes)
    {
        return std::nullopt;
    }

    // Drop anything that sits on top of a waypoint the route already has: it
    // would only make the bot stutter.
    std::erase_if(detour, [&](const Vec3& p) { return distance(p, from) < config_.minSpacing || distance(p, to) < config_.minSpacing; });

    if (detour.empty())
    {
        return std::nullopt;
    }

    // Keep the last few points: those are the ones nearest the far side of the
    // obstacle, and the earliest ones are usually still inside it.
    if (static_cast<int>(detour.size()) > config_.maxPointsPerFix)
    {
        detour.erase(detour.begin(), detour.end() - config_.maxPointsPerFix);
    }

    const float direct = distance(from, to);
    const float viaDetour = pathLength(from, detour, to);

    if (direct > 0.1f && viaDetour > direct * config_.maxDetourRatio)
    {
        if (log)
        {
            log->debug(logcat::Stuck, "not saving a way round waypoint {}: {:.0f} yd detour for a {:.0f} yd leg", index, viaDetour, direct);
        }

        return std::nullopt;
    }

    if (log)
    {
        log->info(logcat::Stuck, "learned a way round waypoint {}: {} point(s), leg {:.0f} -> {:.0f} yd", index, detour.size(), direct, viaDetour);
    }

    return RouteCorrection{index, stuckAt_, std::move(detour), from, to};
}

} // namespace og
