/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-13
 *  File: ObstacleMemory.cpp
 *
 *  Desc:
 *  Implementation of ObstacleMemory (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/ObstacleMemory.h"
#include "og/navigation/Avoidance.h"

#include "og/common/Log.h"

#include <cmath>

namespace og
{

std::string_view toString(ObstacleManeuver maneuver)
{
    switch (maneuver)
    {
    case ObstacleManeuver::Jump:
        return "jump";
    case ObstacleManeuver::StrafeLeft:
        return "strafeLeft";
    case ObstacleManeuver::StrafeRight:
        return "strafeRight";
    }

    return "?";
}

std::optional<ObstacleManeuver> parseObstacleManeuver(std::string_view name)
{
    if (name == "jump")
    {
        return ObstacleManeuver::Jump;
    }

    if (name == "strafeLeft")
    {
        return ObstacleManeuver::StrafeLeft;
    }

    if (name == "strafeRight")
    {
        return ObstacleManeuver::StrafeRight;
    }

    return std::nullopt;
}

void ObstacleMemory::setLearned(LearnedObstacles learned)
{
    learned_ = std::move(learned);
    lastFired_.assign(learned_.maneuvers.size(), TimePoint{});
    revision_ = 0;
    maneuversLearned_ = 0;
}

const std::vector<AvoidZone>& ObstacleMemory::avoidZones() const
{
    static const std::vector<AvoidZone> none;
    return pathfinding_ ? none : learned_.avoidZones;
}

const std::vector<LearnedDetour>& ObstacleMemory::detours() const
{
    static const std::vector<LearnedDetour> none;
    return pathfinding_ ? none : learned_.detours;
}

std::optional<std::size_t> ObstacleMemory::find(const Vec3& position, float heading) const
{
    for (std::size_t i = 0; i < learned_.maneuvers.size(); ++i)
    {
        const LearnedManeuver& m = learned_.maneuvers[i];

        if (distance2D(m.position, position) <= config_.sameSpot && std::fabs(normalizeAngle(m.heading - heading)) <= config_.sameDirection)
        {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<LearnedManeuver> ObstacleMemory::due(const Vec3& position, float travelHeading, TimePoint now)
{
    if (pathfinding_)
    {
        return std::nullopt;
    }

    for (std::size_t i = 0; i < learned_.maneuvers.size(); ++i)
    {
        const LearnedManeuver& m = learned_.maneuvers[i];
        const float d = distance2D(position, m.position);

        if (m.maneuver == ObstacleManeuver::Jump)
        {
            // Ahead by up to jumpDistance, on a line up to jumpSideways off the spot.
            const Vec3 to = m.position - position;
            const Vec3 along = directionFromHeading(travelHeading);
            const float ahead = to.x * along.x + to.y * along.y;
            const float side = std::fabs(to.x * along.y - to.y * along.x);

            if (ahead > config_.jumpDistance || ahead < -0.5f || side > config_.jumpSideways)
            {
                continue;
            }
        }
        else if (d > config_.strafeDistance)
        {
            continue;
        }

        if (std::fabs(normalizeAngle(travelHeading - m.heading)) > config_.sameDirection)
        {
            continue;
        }

        // Only on the way in. Right on top of the spot the bearing is noise.
        if (d > 0.5f && std::fabs(headingDelta(travelHeading, position, m.position)) > kPi / 2.0f)
        {
            continue;
        }

        if (lastFired_[i] != TimePoint{} && now - lastFired_[i] < config_.retrigger)
        {
            continue;
        }

        lastFired_[i] = now;
        return m;
    }

    return std::nullopt;
}

void ObstacleMemory::learnManeuver(const Vec3& position, float heading, ObstacleManeuver maneuver, Logger* log)
{
    if (!config_.learn || pathfinding_)
    {
        return;
    }

    ++maneuversLearned_;

    if (const auto i = find(position, heading))
    {
        LearnedManeuver& m = learned_.maneuvers[*i];

        if (m.maneuver == maneuver)
        {
            ++m.successes;
            m.failures = 0;
        }
        else
        {
            if (log)
            {
                log->info(logcat::Stuck, "obstacle at {}: {} works where {} did not", position, toString(maneuver), toString(m.maneuver));
            }

            m = LearnedManeuver{position, heading, maneuver};
            lastFired_[*i] = TimePoint{};
        }

        ++revision_;
        return;
    }

    if (static_cast<int>(learned_.maneuvers.size()) >= config_.maxManeuvers)
    {
        if (log)
        {
            log->debug(logcat::Stuck, "not remembering the obstacle at {}: {} already", position, learned_.maneuvers.size());
        }

        return;
    }

    learned_.maneuvers.push_back(LearnedManeuver{position, heading, maneuver});
    lastFired_.push_back(TimePoint{});
    ++revision_;

    if (log)
    {
        log->info(logcat::Stuck, "learned to {} at {} heading {:.0f} degrees", toString(maneuver), position, radToDeg(heading));
    }
}

void ObstacleMemory::reportAhead(const LearnedManeuver& made, bool passed, Logger* log)
{
    if (pathfinding_)
    {
        return;
    }

    const auto i = find(made.position, made.heading);

    if (!i || learned_.maneuvers[*i].maneuver != made.maneuver)
    {
        return;
    }

    LearnedManeuver& m = learned_.maneuvers[*i];

    if (passed)
    {
        ++m.successes;
        m.failures = 0;
        return; // not worth a write on every lap
    }

    ++m.failures;
    ++revision_;

    if (m.failures < config_.maxFailures)
    {
        return;
    }

    if (log)
    {
        log->info(logcat::Stuck, "forgetting the {} at {}: made ahead it failed {} times running", toString(m.maneuver), m.position, m.failures);
    }

    learned_.maneuvers.erase(learned_.maneuvers.begin() + static_cast<std::ptrdiff_t>(*i));
    lastFired_.erase(lastFired_.begin() + static_cast<std::ptrdiff_t>(*i));
}

bool ObstacleMemory::learnDetour(LearnedDetour detour, Logger* log)
{
    if (!config_.learn || pathfinding_ || detour.points.empty())
    {
        return false;
    }

    if (static_cast<int>(learned_.detours.size()) >= config_.maxDetours)
    {
        if (log)
        {
            log->debug(logcat::Stuck, "not remembering the way round {}: {} detours already", detour.stuckAt, learned_.detours.size());
        }

        return false;
    }

    learned_.detours.push_back(std::move(detour));
    ++revision_;
    return true;
}

bool ObstacleMemory::learnAvoidZone(const Vec3& center, Logger* log, bool force, bool byHand)
{
    if ((!config_.learn && !force) || pathfinding_ || avoids(center))
    {
        return false;
    }

    // One added by hand goes in whatever the cap.
    if (!byHand && static_cast<int>(learned_.avoidZones.size()) >= config_.maxAvoidZones)
    {
        if (log)
        {
            log->debug(logcat::Stuck, "not avoiding {}: {} areas already", center, learned_.avoidZones.size());
        }

        return false;
    }

    learned_.avoidZones.push_back(AvoidZone{center, config_.avoidRadius});
    ++revision_;

    if (log)
    {
        log->info(logcat::Stuck, "navigation stays {:.0f} yd clear of obstacle at {} from now on", config_.avoidRadius, center);
    }

    return true;
}

bool ObstacleMemory::avoids(const Vec3& point) const
{
    if (!clearOfZones(point, point, noGoZones_))
    {
        return true;
    }

    for (const AvoidZone& zone : avoidZones())
    {
        if (distance2D(point, zone.center) <= zone.radius)
        {
            return true;
        }
    }

    return false;
}

bool ObstacleMemory::clearPath(const Vec3& a, const Vec3& b) const
{
    if (!clearOfZones(a, b, noGoZones_))
    {
        return false;
    }

    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = dx * dx + dy * dy;

    for (const AvoidZone& zone : avoidZones())
    {
        // The point of the segment nearest the zone's centre.
        float t = lengthSquared > 0.0f ? ((zone.center.x - a.x) * dx + (zone.center.y - a.y) * dy) / lengthSquared : 0.0f;
        t = std::fmax(0.0f, std::fmin(1.0f, t));
        const Vec3 nearest{a.x + t * dx, a.y + t * dy, a.z};

        if (distance2D(nearest, zone.center) <= zone.radius)
        {
            return false;
        }
    }

    return true;
}

void ObstacleMemory::forget()
{
    if (learned_.empty())
    {
        return;
    }

    learned_ = LearnedObstacles{};
    lastFired_.clear();
    ++revision_;
}

std::size_t applyDetours(WaypointRoute& route, const std::vector<LearnedDetour>& detours, float tolerance)
{
    std::size_t applied = 0;

    for (const LearnedDetour& detour : detours)
    {
        const std::vector<Vec3>& points = route.points();
        const std::size_t n = points.size();

        if (n < 2 || detour.points.empty())
        {
            continue;
        }

        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec3& to = points[i];
            const Vec3& from = points[i == 0 ? n - 1 : i - 1];

            if (distance(from, detour.from) <= tolerance && distance(to, detour.to) <= tolerance)
            {
                route.insert(i, detour.points);
                ++applied;
                break;
            }
        }
    }

    return applied;
}

std::size_t applyDetours(std::vector<Vec3>& path, const std::vector<LearnedDetour>& detours, std::size_t first, float tolerance)
{
    std::size_t applied = 0;

    for (const LearnedDetour& detour : detours)
    {
        if (detour.points.empty())
        {
            continue;
        }

        for (std::size_t i = first > 0 ? first : 1; i < path.size(); ++i)
        {
            const Vec3 from = path[i - 1];
            const Vec3 to = path[i];
            const auto at = path.begin() + static_cast<std::ptrdiff_t>(i);

            if (distance(from, detour.from) <= tolerance && distance(to, detour.to) <= tolerance)
            {
                path.insert(at, detour.points.begin(), detour.points.end());
            }
            else if (distance(from, detour.to) <= tolerance && distance(to, detour.from) <= tolerance)
            {
                path.insert(at, detour.points.rbegin(), detour.points.rend());
            }
            else
            {
                continue;
            }

            ++applied;
            break;
        }
    }

    return applied;
}

} // namespace og
