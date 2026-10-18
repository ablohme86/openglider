/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-15
 *  File: Roaming.cpp
 *
 *  Desc:
 *  Implementation of Roaming (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/Roaming.h"

#include "og/navigation/Avoidance.h"
#include "og/navigation/INavigationProvider.h"
#include "og/navigation/ObstacleMemory.h"
#include "og/state/AreaTable.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>

namespace og
{

namespace
{

// xorshift64*: small, and the same sequence for the same seed in tests.
float nextUnit(std::uint64_t& state)
{
    if (state == 0)
    {
        state = 0x9E3779B97F4A7C15ull;
    }

    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const std::uint64_t value = state * 0x2545F4914F6CDD1Dull;
    return static_cast<float>((value >> 11) * (1.0 / 9007199254740992.0));
}

// Positive when the route runs counter-clockwise.
float signedArea(const std::vector<Vec3>& route)
{
    double area = 0.0;

    for (std::size_t i = 0, j = route.size() - 1; i < route.size(); j = i++)
    {
        area += static_cast<double>(route[j].x) * route[i].y - static_cast<double>(route[i].x) * route[j].y;
    }

    return static_cast<float>(area / 2.0);
}

float polygonArea(const std::vector<Vec3>& route)
{
    return std::fabs(signedArea(route));
}

float cross(const Vec3& o, const Vec3& a, const Vec3& b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// Whether the segments cross each other at a point inside both.
bool segmentsCross(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d)
{
    const float d1 = cross(c, d, a);
    const float d2 = cross(c, d, b);
    const float d3 = cross(a, b, c);
    const float d4 = cross(a, b, d);
    return ((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) && ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f));
}

// The unit normal of the edge from `a` to `b` that points into the area.
Vec3 inwardNormal(const Vec3& a, const Vec3& b, bool counterClockwise)
{
    const Vec3 edge = b - a;
    const float length = edge.length2D();

    if (length <= 0.0f)
    {
        return {};
    }

    return counterClockwise ? Vec3{-edge.y / length, edge.x / length, 0.0f} : Vec3{edge.y / length, -edge.x / length, 0.0f};
}

float distanceToSegment2D(const Vec3& pt, const Vec3& a, const Vec3& b)
{
    const Vec3 ab = b - a;
    const float lenSq = ab.x * ab.x + ab.y * ab.y;

    if (lenSq <= 0.0f)
    {
        return distance2D(pt, a);
    }

    const float t = std::clamp(((pt.x - a.x) * ab.x + (pt.y - a.y) * ab.y) / lenSq, 0.0f, 1.0f);
    const Vec3 proj = a + ab * t;
    return distance2D(pt, proj);
}

float distanceToRoute2D(const std::vector<Vec3>& route, const Vec3& pt)
{
    if (route.empty())
    {
        return 0.0f;
    }

    float best = std::numeric_limits<float>::max();
    const std::size_t n = route.size();

    for (std::size_t i = 0; i < n; ++i)
    {
        best = std::min(best, distanceToSegment2D(pt, route[i], route[(i + 1) % n]));
    }

    return best;
}

bool withinRoute(const std::vector<Vec3>& route, const Vec3& point, float corridor = 0.0f)
{
    if (insideRoute(route, point))
    {
        return true;
    }

    const float limit = corridor > 0.0f ? corridor : 1.0f;
    return distanceToRoute2D(route, point) <= limit;
}

// Whether the straight line from `a` to `b` stays inside the area and clear of
// what `obstacles` avoids.
bool legInside(const std::vector<Vec3>& route, const Vec3& a, const Vec3& b, const ObstacleMemory* obstacles, float corridor = 0.0f)
{
    if (corridor <= 1.0f)
    {
        for (std::size_t i = 0, j = route.size() - 1; i < route.size(); j = i++)
        {
            if (segmentsCross(a, b, route[j], route[i]))
            {
                return false;
            }
        }
    }

    // A line through a corner crosses no edge, yet may still run outside.
    for (const float t : {0.25f, 0.5f, 0.75f})
    {
        if (!withinRoute(route, a + (b - a) * t, corridor))
        {
            return false;
        }
    }

    return !obstacles || obstacles->clearPath(a, b);
}

float nearestZ(const std::vector<Vec3>& route, const Vec3& point)
{
    float best = std::numeric_limits<float>::max();
    float z = point.z;

    for (const Vec3& p : route)
    {
        const float d = distance2D(p, point);

        if (d < best)
        {
            best = d;
            z = p.z;
        }
    }

    return z;
}

} // namespace

bool outlinesArea(const std::vector<Vec3>& route)
{
    return route.size() >= 3 && polygonArea(route) >= 100.0f;
}

bool insideRoute(const std::vector<Vec3>& route, const Vec3& point)
{
    if (route.size() < 3)
    {
        return false;
    }

    bool inside = false;

    for (std::size_t i = 0, j = route.size() - 1; i < route.size(); j = i++)
    {
        const Vec3& a = route[i];
        const Vec3& b = route[j];

        if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
        {
            inside = !inside;
        }
    }

    return inside;
}

bool insideZone(const INavigationProvider* provider, std::uint32_t zone, const Vec3& point)
{
    if (!zone || !provider)
    {
        return true;
    }

    const auto area = provider->areaAt(point);
    return !area || canonicalZone(*area) == zone;
}

std::optional<Vec3> pickRoamPoint(const std::vector<Vec3>& route, const Vec3& from, const ObstacleMemory* obstacles, std::uint64_t& state, float minDistance, int attempts, INavigationProvider* provider, std::uint32_t zone)
{
    if (route.empty())
    {
        return std::nullopt;
    }

    // Anything smaller outlines no area worth roaming: a path walked there and back.
    const bool area = outlinesArea(route);

    float minX = route.front().x, maxX = minX, minY = route.front().y, maxY = minY;

    for (const Vec3& p : route)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }

    for (int i = 0; i < attempts; ++i)
    {
        Vec3 candidate;

        if (area)
        {
            if (maxX - minX > 500.0f || maxY - minY > 500.0f)
            {
                // Large roam area (e.g. whole zone): roam in reachable steps around current location
                const float maxStep = std::max(250.0f, minDistance);
                const float angle = nextUnit(state) * 6.2831853f;
                const float dist = minDistance + nextUnit(state) * (maxStep - minDistance);
                candidate = {from.x + std::cos(angle) * dist, from.y + std::sin(angle) * dist, 0.0f};
            }
            else
            {
                candidate = {minX + nextUnit(state) * (maxX - minX), minY + nextUnit(state) * (maxY - minY), 0.0f};
            }

            if (!insideRoute(route, candidate))
            {
                continue;
            }
        }
        else
        {
            // Near a random waypoint, within a few yards either side.
            const Vec3& anchor = route[std::min(route.size() - 1, static_cast<std::size_t>(nextUnit(state) * route.size()))];
            const float angle = nextUnit(state) * 6.2831853f;
            const float radius = nextUnit(state) * 8.0f;
            candidate = {anchor.x + std::cos(angle) * radius, anchor.y + std::sin(angle) * radius, 0.0f};
        }

        candidate.z = nearestZ(route, candidate);
        const float effectiveMinDist = (i >= attempts / 2 && minDistance > 5.0f) ? 5.0f : minDistance;

        if (distance2D(candidate, from) < effectiveMinDist)
        {
            continue;
        }

        if (obstacles && obstacles->avoids(candidate))
        {
            continue;
        }

        if (!insideZone(provider, zone, candidate))
        {
            continue;
        }

        if (provider && provider->usesClientGeometry())
        {
            auto walkable = provider->findWalkablePoint(candidate, 5.0f, 1.0f);

            if (!walkable)
            {
                continue;
            }

            candidate = *walkable;

            if (obstacles && obstacles->avoids(candidate))
            {
                continue;
            }

            // Not the bed of a lake: roaming the whole of Mulgore, the bot
            // swam about the lake by Bloodhoof Village until out of breath.
            if (const auto surface = provider->waterSurface(candidate); surface && *surface > candidate.z + 1.0f)
            {
                continue;
            }

            if (area && !insideRoute(route, candidate))
            {
                continue;
            }

            auto path = provider->findPath(from, candidate);

            if (!path || path->points.empty())
            {
                continue;
            }

            return candidate;
        }

        if (roamPath(route, from, candidate, obstacles).empty())
        {
            continue;
        }

        return candidate;
    }

    return std::nullopt;
}

std::vector<Vec3> roamPath(const std::vector<Vec3>& route, const Vec3& from, const Vec3& to, const ObstacleMemory* obstacles, float inset)
{
    if (obstacles && obstacles->stopToken().stop_requested())
    {
        return {};
    }

    if (!outlinesArea(route))
    {
        if (obstacles && !obstacles->clearPath(from, to))
        {
            return {};
        }

        return {to};
    }

    const bool counterClockwise = signedArea(route) > 0.0f;
    const std::size_t n = route.size();
    std::vector<Vec3> path;

    // Outside the area: back in at the nearest point first.
    Vec3 start = from;

    if (!insideRoute(route, from))
    {
        float best = std::numeric_limits<float>::max();
        std::optional<Vec3> entry;

        for (std::size_t i = 0, j = n - 1; i < n; j = i++)
        {
            const Vec3& a = route[j];
            const Vec3& b = route[i];
            const Vec3 edge = b - a;
            const float lengthSquared = edge.x * edge.x + edge.y * edge.y;

            if (lengthSquared <= 0.0f)
            {
                continue;
            }

            float t = ((from.x - a.x) * edge.x + (from.y - a.y) * edge.y) / lengthSquared;
            t = std::clamp(t, 0.0f, 1.0f);
            Vec3 candidate = a + edge * t + inwardNormal(a, b, counterClockwise) * inset;
            candidate.z = nearestZ(route, candidate);

            if (!insideRoute(route, candidate))
            {
                continue;
            }

            if (obstacles && (obstacles->avoids(candidate) || !obstacles->clearPath(from, candidate)))
            {
                continue;
            }

            const float d = distance2D(from, candidate);

            if (d < best)
            {
                best = d;
                entry = candidate;
            }
        }

        if (!entry)
        {
            return {};
        }

        path.push_back(*entry);
        start = *entry;
    }

    if (legInside(route, start, to, obstacles))
    {
        path.push_back(to);
        return path;
    }

    // Round the corners: each waypoint taken `inset` yards in along the bisector
    // of its edges' inward normals, then the shortest way through those that see
    // each other.
    std::vector<Vec3> nodes{start, to};

    for (std::size_t i = 0; i < n; ++i)
    {
        const Vec3& previous = route[(i + n - 1) % n];
        const Vec3& corner = route[i];
        const Vec3& next = route[(i + 1) % n];
        Vec3 direction = inwardNormal(previous, corner, counterClockwise) + inwardNormal(corner, next, counterClockwise);
        const float length = direction.length2D();

        if (length < 1e-3f)
        {
            continue;
        }

        Vec3 point = corner + direction * (inset / length);
        point.z = corner.z;

        if (!insideRoute(route, point) || (obstacles && obstacles->avoids(point)))
        {
            continue;
        }

        nodes.push_back(point);
    }

    std::vector<float> cost(nodes.size(), std::numeric_limits<float>::max());
    std::vector<std::size_t> previous(nodes.size(), nodes.size());
    std::vector<bool> done(nodes.size(), false);
    using Entry = std::pair<float, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    cost[0] = 0.0f;
    open.push({0.0f, 0});

    while (!open.empty())
    {
        if (obstacles && obstacles->stopToken().stop_requested())
        {
            return {};
        }

        const std::size_t at = open.top().second;
        open.pop();

        if (done[at])
        {
            continue;
        }

        done[at] = true;

        if (at == 1)
        {
            break;
        }

        for (std::size_t next = 1; next < nodes.size(); ++next)
        {
            if (done[next])
            {
                continue;
            }

            const float through = cost[at] + distance2D(nodes[at], nodes[next]);

            if (through >= cost[next] || !legInside(route, nodes[at], nodes[next], obstacles))
            {
                continue;
            }

            cost[next] = through;
            previous[next] = at;
            open.push({through, next});
        }
    }

    if (!done[1])
    {
        return {};
    }

    std::vector<Vec3> corners;

    for (std::size_t at = 1; at != 0; at = previous[at])
    {
        corners.push_back(nodes[at]);
    }

    path.insert(path.end(), corners.rbegin(), corners.rend());
    return path;
}

namespace
{

// Where a circuit may go: inside the area and `clearance` yards clear of every
// No Go zone and learned obstacle.
struct CircuitSpace
{
    CircuitSpace(const std::vector<Vec3>& within, const ObstacleMemory* memory, float margin, float corridorWidth = 0.0f) : area(within), obstacles(memory), clearance(margin), corridor(corridorWidth)
    {
        if (memory)
        {
            zones = memory->noGoZones();
            const auto& learned = memory->avoidZones();
            zones.insert(zones.end(), learned.begin(), learned.end());
        }
    }

    bool clearPoint(const Vec3& p) const
    {
        return insideRoute(area, p) && clearOfZones(p, p, zones, clearance);
    }

    bool clearLeg(const Vec3& a, const Vec3& b) const
    {
        return legInside(area, a, b, nullptr) && clearOfZones(a, b, zones, clearance);
    }

    // The way from `at` to `target` in legs that are all clear, ending at
    // `target`; empty when there is none. Round the zones with only `clearance`
    // to spare, which finds the way between two that stand close - a coarse grid
    // first, which is quick, a fine one where a gap is narrower than its cells -
    // and failing that round the area's corners.
    std::vector<Vec3> connect(const Vec3& at, const Vec3& target) const
    {
        if (obstacles && obstacles->stopToken().stop_requested())
        {
            return {};
        }

        const auto clearAll = [&](const std::vector<Vec3>& points)
        {
            Vec3 previous = at;

            for (const Vec3& p : points)
            {
                if (!clearLeg(previous, p))
                {
                    return false;
                }

                previous = p;
            }

            return !points.empty();
        };

        if (clearLeg(at, target))
        {
            return {target};
        }

        for (const float step : {3.0f, 1.0f})
        {
            if (auto around = pathAroundZones(at, target, zones, clearance, step, obstacles ? obstacles->stopToken() : std::stop_token{}); around && clearAll(around->points))
            {
                return around->points;
            }
        }

        if (auto corners = roamPath(area, at, target, obstacles); clearAll(corners))
        {
            return corners;
        }

        return {};
    }

    const std::vector<Vec3>& area;
    const ObstacleMemory* obstacles;
    float clearance;
    float corridor;
    std::vector<AvoidZone> zones;
};

} // namespace

std::vector<Vec3> circuitWay(const std::vector<Vec3>& area, const Vec3& from, const Vec3& to, const ObstacleMemory* obstacles, float clearance)
{
    const ObstacleMemory::Calculation calculation(obstacles);

    if (obstacles && obstacles->stopToken().stop_requested())
    {
        return {};
    }

    if (outlinesArea(area))
    {
        const CircuitSpace space(area, obstacles, clearance);

        if (auto way = space.connect(from, to); !way.empty())
        {
            return way;
        }
    }

    // Outside the area, or hemmed in: round the area's corners, back in first.
    if (auto corners = roamPath(area, from, to, obstacles); !corners.empty())
    {
        return corners;
    }

    if (obstacles && obstacles->stopToken().stop_requested())
    {
        return {};
    }

    return {to};
}

bool repairCircuit(const std::vector<Vec3>& area, std::vector<Vec3>& circuit, std::size_t from, const ObstacleMemory* obstacles, float clearance)
{
    const ObstacleMemory::Calculation calculation(obstacles);

    if (obstacles && obstacles->stopToken().stop_requested())
    {
        return false;
    }

    if (from >= circuit.size())
    {
        return true;
    }

    float minX = area.front().x, maxX = minX, minY = area.front().y, maxY = minY;

    for (const Vec3& p : area)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }

    const bool largeArea = std::max(maxX - minX, maxY - minY) > 350.0f;
    const float corridor = largeArea ? 50.0f : 0.0f;
    const CircuitSpace space(area, obstacles, clearance, corridor);
    std::vector<Vec3> repaired(circuit.begin(), circuit.begin() + static_cast<std::ptrdiff_t>(from));
    const std::size_t n = circuit.size();

    for (std::size_t j = from; j < n;)
    {
        const bool pointClear = space.clearPoint(circuit[j]);

        if (pointClear && (j + 1 == n || space.clearLeg(circuit[j], circuit[j + 1])))
        {
            repaired.push_back(circuit[j++]);
            continue;
        }

        // From the last clear point round to the next one; the lap ends early
        // when none is left after it.
        if (pointClear)
        {
            repaired.push_back(circuit[j]);
        }

        if (repaired.size() <= from)
        {
            return false; // nothing clear to start the way round from
        }

        std::size_t k = j + 1;

        while (k < n && !space.clearPoint(circuit[k]))
        {
            ++k;
        }

        if (k == n)
        {
            break;
        }

        const auto way = space.connect(repaired.back(), circuit[k]);

        if (way.empty())
        {
            return false;
        }

        repaired.insert(repaired.end(), way.begin(), way.end());
        j = k + 1;
    }

    if (repaired.size() <= from)
    {
        return false;
    }

    circuit = std::move(repaired);
    return true;
}

std::vector<Vec3> generateCircuit(const std::vector<Vec3>& area, const Vec3& from, INavigationProvider* provider, const ObstacleMemory* obstacles, std::uint64_t& state, float clearance)
{
    const ObstacleMemory::Calculation calculation(obstacles);
    const auto stopped = [&] { return obstacles && obstacles->stopToken().stop_requested(); };

    if (stopped() || !outlinesArea(area))
    {
        return {};
    }

    float minX = area.front().x, maxX = minX, minY = area.front().y, maxY = minY;

    for (const Vec3& p : area)
    {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }

    const bool largeArea = std::max(maxX - minX, maxY - minY) > 350.0f;
    const CircuitSpace space(area, obstacles, clearance);
    const auto clearPoint = [&](const Vec3& p) { return space.clearPoint(p); };
    const auto clearLeg = [&](const Vec3& a, const Vec3& b) { return space.clearLeg(a, b); };

    const bool counterClockwise = signedArea(area) > 0.0f;
    std::vector<Vec3> anchors;

    if (largeArea)
    {
        // Keep samples near the boundary too: narrow arms can fall between the
        // interior lanes. These are stops in the same coverage loop, not its shape.
        const std::size_t n = area.size();

        for (std::size_t i = 0; i < n; ++i)
        {
            if (stopped())
            {
                return {};
            }

            const Vec3& a = area[i];
            const Vec3& b = area[(i + 1) % n];
            const float length = distance2D(a, b);
            const int steps = std::max(1, static_cast<int>(std::lround(length / 70.0f)));
            const Vec3 inward = inwardNormal(a, b, counterClockwise);

            for (int k = 0; k < steps; ++k)
            {
                const float t = static_cast<float>(k) / static_cast<float>(steps);
                Vec3 pt = a + (b - a) * t;
                pt.z = a.z + (b.z - a.z) * t;
                const float offset = 3.0f + nextUnit(state) * 8.0f;
                Vec3 candidate = pt + inward * offset;
                candidate.z = nearestZ(area, candidate);

                if (!clearPoint(candidate))
                {
                    candidate = pt + inward * 1.5f;
                    candidate.z = nearestZ(area, candidate);
                }

                if (!clearPoint(candidate))
                {
                    continue;
                }

                if (provider && provider->usesClientGeometry())
                {
                    auto walkable = provider->findWalkablePoint(candidate, 5.0f, clearance);

                    if (!walkable || !clearPoint(*walkable))
                    {
                        continue;
                    }

                    candidate = *walkable;
                }

                if (!anchors.empty() && distance2D(anchors.back(), candidate) < 15.0f)
                {
                    continue;
                }

                anchors.push_back(candidate);
            }
        }

        // Sweep the entire interior in alternating lanes. Vary their spacing,
        // direction and stops for each lap, rather than tracing the perimeter.
        const float spacing = 60.0f + nextUnit(state) * 20.0f;
        const bool acrossX = nextUnit(state) < 0.5f;
        const int columns = std::max(1, static_cast<int>(std::ceil((maxX - minX) / spacing)));
        const int rows = std::max(1, static_cast<int>(std::ceil((maxY - minY) / spacing)));
        const float dx = (maxX - minX) / columns;
        const float dy = (maxY - minY) / rows;

        for (int y = 0; y < rows; ++y)
        {
            for (int x = 0; x < columns; ++x)
            {
                if (stopped())
                {
                    return {};
                }

                Vec3 candidate{minX + (x + 0.35f + nextUnit(state) * 0.3f) * dx, minY + (y + 0.35f + nextUnit(state) * 0.3f) * dy, 0};

                if (!clearPoint(candidate))
                {
                    continue;
                }

                candidate.z = nearestZ(area, candidate);

                if (provider && provider->usesClientGeometry())
                {
                    auto walkable = provider->findWalkablePoint(candidate, 5.0f, clearance);

                    if (!walkable || !clearPoint(*walkable))
                    {
                        continue;
                    }

                    candidate = *walkable;
                }

                if (std::any_of(anchors.begin(), anchors.end(), [&](const Vec3& a) { return distance2D(a, candidate) < 20.0f; }))
                {
                    continue;
                }

                anchors.push_back(candidate);
            }
        }

        const auto lane = [&](const Vec3& p) { return acrossX ? static_cast<int>((p.y - minY) / dy) : static_cast<int>((p.x - minX) / dx); };
        std::sort(anchors.begin(), anchors.end(),
                  [&](const Vec3& a, const Vec3& b)
                  {
                      const int rowA = lane(a), rowB = lane(b);

                      if (rowA != rowB)
                      {
                          return rowA < rowB;
                      }

                      const float alongA = acrossX ? a.x : a.y;
                      const float alongB = acrossX ? b.x : b.y;
                      return rowA % 2 == 0 ? alongA < alongB : alongA > alongB;
                  });
    }
    else
    {
        // Small area: anchors spread over the area: one per 800 square yards or so, 8 to 40.
        const float size = polygonArea(area);
        const auto wanted = static_cast<std::size_t>(std::clamp(size / 800.0f, 8.0f, 40.0f));
        float spacing = std::sqrt(size / static_cast<float>(wanted)) * 0.6f;

        for (int tries = 0; anchors.size() < wanted && tries < 2000; ++tries)
        {
            if (stopped())
            {
                return {};
            }

            // Relax the spacing when the area leaves too little room for it.
            if (tries > 0 && tries % 300 == 0)
            {
                spacing *= 0.7f;
            }

            Vec3 candidate{minX + nextUnit(state) * (maxX - minX), minY + nextUnit(state) * (maxY - minY), 0.0f};

            if (!clearPoint(candidate))
            {
                continue;
            }

            candidate.z = nearestZ(area, candidate);

            if (provider && provider->usesClientGeometry())
            {
                auto walkable = provider->findWalkablePoint(candidate, 2.0f, clearance);

                if (!walkable)
                {
                    continue;
                }

                if (!clearPoint(*walkable))
                {
                    continue;
                }

                candidate = *walkable;
            }

            if (std::any_of(anchors.begin(), anchors.end(), [&](const Vec3& a) { return distance2D(a, candidate) < spacing; }))
            {
                continue;
            }

            anchors.push_back(candidate);
        }
    }

    if (anchors.size() < 3)
    {
        return {};
    }

    calculation.report(10);

    // A round trip through them.
    const auto first = std::min_element(anchors.begin(), anchors.end(), [&](const Vec3& a, const Vec3& b) { return distance2D(a, from) < distance2D(b, from); });

    if (largeArea)
    {
        std::rotate(anchors.begin(), first, anchors.end());

        if (nextUnit(state) < 0.5f)
        {
            std::reverse(anchors.begin() + 1, anchors.end());
        }
    }
    else
    {
        std::iter_swap(anchors.begin(), first);

        for (std::size_t i = 1; i < anchors.size(); ++i)
        {
            const auto next = std::min_element(anchors.begin() + static_cast<std::ptrdiff_t>(i), anchors.end(), [&](const Vec3& a, const Vec3& b) { return distance2D(a, anchors[i - 1]) < distance2D(b, anchors[i - 1]); });
            std::iter_swap(anchors.begin() + static_cast<std::ptrdiff_t>(i), next);
        }

        const std::size_t n = anchors.size();
        bool improved = true;

        for (int pass = 0; improved && pass < 50; ++pass)
        {
            improved = false;

            for (std::size_t i = 0; i + 2 < n; ++i)
            {
                for (std::size_t j = i + 2; j < n; ++j)
                {
                    const Vec3& a = anchors[i];
                    const Vec3& b = anchors[i + 1];
                    const Vec3& c = anchors[j];
                    const Vec3& d = anchors[(j + 1) % n];

                    if (distance2D(a, c) + distance2D(b, d) + 0.01f < distance2D(a, b) + distance2D(c, d))
                    {
                        std::reverse(anchors.begin() + static_cast<std::ptrdiff_t>(i + 1), anchors.begin() + static_cast<std::ptrdiff_t>(j + 1));
                        improved = true;
                    }
                }
            }
        }

        if (nextUnit(state) < 0.5f)
        {
            std::reverse(anchors.begin() + 1, anchors.end());
        }
    }

    const auto acceptable = [&](const Vec3& start, const Path& path)
    {
        if (path.continues || path.points.empty())
        {
            return false;
        }

        if (obstacles && obstacles->avoids(path.points.back()))
        {
            return false;
        }

        bool leftStartObstacle = !obstacles || !obstacles->avoids(start);
        Vec3 previous = start;

        for (const auto& point : path.points)
        {
            const int samples = std::max(1, static_cast<int>(std::ceil(distance2D(previous, point))));

            for (int j = 1; j <= samples; ++j)
            {
                const auto pt = previous + (point - previous) * (static_cast<float>(j) / samples);

                if (!insideRoute(area, pt))
                {
                    return false;
                }

                const bool blocked = obstacles && obstacles->avoids(pt);

                if (blocked && leftStartObstacle)
                {
                    return false;
                }

                if (!blocked)
                {
                    leftStartObstacle = true;
                }
            }

            previous = point;
        }

        return true;
    };

    const auto localConnection = [&](const Vec3& at, const Vec3& target) -> std::vector<Vec3>
    {
        if (stopped())
        {
            return {};
        }

        if (clearLeg(at, target) && (!provider || !provider->usesClientGeometry() || provider->canWalkStraight(at, target)))
        {
            return {target};
        }

        if (provider && provider->usesClientGeometry())
        {
            auto path = provider->findPath(at, target);

            if (path && acceptable(at, *path))
            {
                // Graph edges meet at the same projected anchor, never on
                // different floors that happen to share its map coordinates.
                if (distance(path->points.back(), target) > 0.01f)
                {
                    if (!clearLeg(path->points.back(), target) || !provider->canWalkStraight(path->points.back(), target))
                    {
                        return {};
                    }

                    path->points.push_back(target);
                }

                return std::move(path->points);
            }

            return {};
        }

        return space.connect(at, target);
    };

    // Client paths cover only a local part of the map. Join distant lanes and
    // close the loop through nearby walkable anchors, including around concave
    // boundaries and blocked lanes. Cache both successful and failed edges.
    std::vector<std::vector<std::size_t>> neighbors(anchors.size());

    if (largeArea)
    {
        for (std::size_t i = 0; i < anchors.size(); ++i)
        {
            if (stopped())
            {
                return {};
            }

            for (std::size_t j = i + 1; j < anchors.size(); ++j)
            {
                if (distance2D(anchors[i], anchors[j]) > 150.0f)
                {
                    continue;
                }

                neighbors[i].push_back(j);
                neighbors[j].push_back(i);
            }
        }
    }

    std::map<std::pair<std::size_t, std::size_t>, std::vector<Vec3>> connections;
    const auto edge = [&](std::size_t a, std::size_t b) -> const std::vector<Vec3>&
    {
        const auto key = std::pair{a, b};

        if (const auto it = connections.find(key); it != connections.end())
        {
            return it->second;
        }

        return connections.emplace(key, localConnection(anchors[a], anchors[b])).first->second;
    };
    const auto connect = [&](std::size_t fromIndex, std::size_t toIndex) -> std::vector<Vec3>
    {
        if (!largeArea)
        {
            return localConnection(anchors[fromIndex], anchors[toIndex]);
        }

        if (fromIndex == toIndex)
        {
            return {anchors[toIndex]};
        }

        if (distance2D(anchors[fromIndex], anchors[toIndex]) <= 150.0f)
        {
            const auto& direct = edge(fromIndex, toIndex);

            if (!direct.empty())
            {
                return direct;
            }
        }

        std::vector<float> cost(anchors.size(), std::numeric_limits<float>::max());
        std::vector<std::size_t> previous(anchors.size(), anchors.size());
        std::vector<bool> done(anchors.size(), false);
        using Entry = std::pair<float, std::size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
        cost[fromIndex] = 0;
        open.push({distance2D(anchors[fromIndex], anchors[toIndex]), fromIndex});

        while (!open.empty())
        {
            if (stopped())
            {
                return {};
            }

            const auto at = open.top().second;
            open.pop();

            if (done[at])
            {
                continue;
            }

            done[at] = true;

            if (at == toIndex)
            {
                break;
            }

            for (const auto next : neighbors[at])
            {
                if (done[next])
                {
                    continue;
                }

                const float lowerBound = cost[at] + distance2D(anchors[at], anchors[next]);

                if (lowerBound >= cost[next])
                {
                    continue;
                }

                const auto& path = edge(at, next);

                if (path.empty())
                {
                    continue;
                }

                float through = cost[at];
                Vec3 point = anchors[at];

                for (const auto& p : path)
                {
                    through += distance2D(point, p);
                    point = p;
                }

                if (through >= cost[next])
                {
                    continue;
                }

                cost[next] = through;
                previous[next] = at;
                open.push({through + distance2D(anchors[next], anchors[toIndex]), next});
            }
        }

        if (!done[toIndex])
        {
            return {};
        }

        std::vector<std::size_t> steps;

        for (auto at = toIndex; at != fromIndex; at = previous[at])
        {
            steps.push_back(at);
        }

        std::vector<Vec3> path;
        auto at = fromIndex;

        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
        {
            const auto& leg = edge(at, *it);
            path.insert(path.end(), leg.begin(), leg.end());
            at = *it;
        }

        return path;
    };

    // Join them, each leg round the corners or obstacles it would cut; an anchor
    // that cannot be reached cleanly is left out.
    std::vector<Vec3> skeleton{anchors.front()};
    std::vector<std::size_t> anchorEnds; // where each anchor's leg ends in the skeleton
    std::vector<std::size_t> reachedAnchors{0};

    for (std::size_t i = 1; i < anchors.size(); ++i)
    {
        if (stopped())
        {
            return {};
        }

        calculation.report(10 + static_cast<int>(55 * i / anchors.size()));
        const auto leg = connect(reachedAnchors.back(), i);

        if (leg.empty())
        {
            continue;
        }

        skeleton.insert(skeleton.end(), leg.begin(), leg.end());
        anchorEnds.push_back(skeleton.size());
        reachedAnchors.push_back(i);
    }

    // Back to the start. Never closed by a straight line across everything:
    // leave out the last anchors until the way back can be found.
    for (;;)
    {
        if (stopped())
        {
            return {};
        }

        const auto home = connect(reachedAnchors.back(), 0);

        if (!home.empty())
        {
            skeleton.insert(skeleton.end(), home.begin(), home.end());
            break;
        }

        if (anchorEnds.size() < 2)
        {
            return {};
        }

        anchorEnds.pop_back();
        reachedAnchors.pop_back();
        skeleton.resize(anchorEnds.back());
    }

    if (skeleton.size() > 1 && distance2D(skeleton.back(), skeleton.front()) < 0.5f)
    {
        skeleton.pop_back();
    }

    if (skeleton.size() < 3)
    {
        return {};
    }

    calculation.report(65);

    // From here on every step keeps each leg clear, so no point is ever thrown
    // away and no long straight jump can open up.

    // Pieces of about 8 yards, pushed sideways along a gentle wave. A piece is
    // bent only if both its leg from the point before and its leg on to the
    // next unbent point stay clear; unbent, it lies on a leg already checked.
    constexpr float kPiece = 8.0f;
    const float amplitude = 1.5f + 2.0f * nextUnit(state);
    const float wavelength = 40.0f + 30.0f * nextUnit(state);
    const float phase = nextUnit(state) * 6.2831853f;
    std::vector<Vec3> swayed;
    float walked = 0.0f;

    for (std::size_t i = 0; i < skeleton.size(); ++i)
    {
        if (stopped())
        {
            return {};
        }

        calculation.report(65 + static_cast<int>(15 * i / skeleton.size()));
        const Vec3& p = skeleton[i];
        const Vec3& q = skeleton[(i + 1) % skeleton.size()];
        const float length = distance2D(p, q);
        const int pieces = std::max(1, static_cast<int>(std::lround(length / kPiece)));
        const Vec3 side = length > 0.0f ? Vec3{-(q.y - p.y) / length, (q.x - p.x) / length, 0.0f} : Vec3{};

        for (int k = 0; k < pieces; ++k)
        {
            const float t = static_cast<float>(k) / static_cast<float>(pieces);
            Vec3 point = p + (q - p) * t;

            if (k > 0)
            {
                const Vec3 onward = p + (q - p) * (static_cast<float>(k + 1) / static_cast<float>(pieces));
                const float offset = amplitude * std::sin(phase + (walked + t * length) / wavelength * 6.2831853f);
                Vec3 bent = point + side * offset;

                if (provider && provider->usesClientGeometry())
                {
                    auto walkable = provider->findWalkablePoint(bent, 1.5f, 0.8f);

                    if (walkable && clearPoint(*walkable) && provider->canWalkStraight(swayed.back(), *walkable) && provider->canWalkStraight(*walkable, onward) && clearLeg(swayed.back(), *walkable) && clearLeg(*walkable, onward))
                    {
                        point = *walkable;
                    }
                }
                else if (clearPoint(bent) && clearLeg(swayed.back(), bent) && clearLeg(bent, onward))
                {
                    point = bent;
                }
            }

            if (provider && provider->usesClientGeometry())
            {
                if (auto snap = provider->findWalkablePoint(point, 1.5f, 0.0f))
                {
                    if (clearPoint(*snap))
                    {
                        point = *snap;
                    }
                }
            }
            else
            {
                point.z = nearestZ(area, point);
            }

            swayed.push_back(point);
        }

        walked += length;
    }

    if (swayed.size() < 3)
    {
        return skeleton;
    }

    // Round the corners off (two passes of Chaikin's cutting). A cut across a
    // corner that is not clear keeps the corner itself; the other legs lie on
    // legs already clear.
    std::vector<Vec3> circuit = swayed;

    for (int pass = 0; pass < 2; ++pass)
    {
        if (stopped())
        {
            return {};
        }

        calculation.report(80 + pass * 7);
        const std::size_t n = circuit.size();
        const auto firstCut = [&](std::size_t i) { return circuit[i] * 0.75f + circuit[(i + 1) % n] * 0.25f; };
        const auto secondCut = [&](std::size_t i) { return circuit[i] * 0.25f + circuit[(i + 1) % n] * 0.75f; };
        std::vector<Vec3> rounded;

        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec3 before = secondCut((i + n - 1) % n);
            Vec3 in = firstCut(i);
            Vec3 out = secondCut(i);
            in.z = out.z = circuit[i].z;
            bool cutClear = clearLeg(before, in);

            if (cutClear && provider && provider->usesClientGeometry())
            {
                auto win = provider->findWalkablePoint(in, 1.0f, 0.5f);
                auto wout = provider->findWalkablePoint(out, 1.0f, 0.5f);

                if (win && wout && clearPoint(*win) && clearPoint(*wout) && provider->canWalkStraight(before, *win) && provider->canWalkStraight(*win, *wout))
                {
                    in = *win;
                    out = *wout;
                }
                else
                {
                    cutClear = false;
                }
            }

            if (!cutClear)
            {
                rounded.push_back(circuit[i]);
            }

            rounded.push_back(in);
            rounded.push_back(out);
        }

        circuit = std::move(rounded);
    }

    // Two passes leave points a couple of yards apart: drop every other one
    // where the leg past it stays clear.
    calculation.report(95);
    std::vector<Vec3> thinned{circuit.front()};

    for (std::size_t i = 1; i < circuit.size(); ++i)
    {
        const bool last = i + 1 == circuit.size();
        const Vec3& next = circuit[last ? 0 : i + 1];
        const bool skippable = i % 2 == 1 && !last && clearLeg(thinned.back(), next) && (!provider || !provider->usesClientGeometry() || provider->canWalkStraight(thinned.back(), next));

        if (!skippable)
        {
            thinned.push_back(circuit[i]);
        }
    }

    if (provider && distance2D(thinned.back(), thinned.front()) > 0.01f)
    {
        thinned.push_back(thinned.front());
    }

    calculation.report(100);
    return thinned;
}

std::vector<Vec3> buildClientCircuit(const std::vector<Vec3>& area, const Vec3& from, INavigationProvider& provider, const ObstacleMemory* obstacles, std::uint64_t& state)
{
    return generateCircuit(area, from, &provider, obstacles, state, 1.0f);
}

std::vector<Vec3> buildCircuit(const std::vector<Vec3>& area, const Vec3& from, const ObstacleMemory* obstacles, std::uint64_t& state, float clearance)
{
    return generateCircuit(area, from, nullptr, obstacles, state, clearance);
}

std::vector<Vec3> detourPath(const std::vector<Vec3>& area, const Vec3& from, const std::vector<Vec3>& stops, const ObstacleMemory* obstacles, std::uint64_t& state, std::vector<std::size_t>* stopEnds)
{
    const bool bounded = outlinesArea(area);
    const CircuitSpace space(area, obstacles, 1.0f);
    const auto clearPoint = [&](const Vec3& p) { return bounded ? space.clearPoint(p) : clearOfZones(p, p, space.zones, space.clearance); };
    const auto clearLeg = [&](const Vec3& a, const Vec3& b) { return bounded ? space.clearLeg(a, b) : clearOfZones(a, b, space.zones, space.clearance); };
    constexpr float kPiece = 8.0f;
    const float amplitude = 1.5f + 1.5f * nextUnit(state);
    const float wavelength = 30.0f + 20.0f * nextUnit(state);
    const float phase = nextUnit(state) * 6.2831853f;

    std::vector<Vec3> out;

    if (stopEnds)
    {
        stopEnds->clear();
    }

    Vec3 at = from;
    float walked = 0.0f;

    for (const Vec3& stop : stops)
    {
        std::vector<Vec3> skeleton{at};
        const auto leg = circuitWay(area, at, stop, obstacles);
        skeleton.insert(skeleton.end(), leg.begin(), leg.end());
        skeleton.back() = stop;

        // Pieces pushed sideways along a wave, each only where its legs stay clear.
        std::vector<Vec3> swayed{at};

        for (std::size_t i = 0; i + 1 < skeleton.size(); ++i)
        {
            const Vec3& p = skeleton[i];
            const Vec3& q = skeleton[i + 1];
            const float length = distance2D(p, q);
            const int pieces = std::max(1, static_cast<int>(std::lround(length / kPiece)));
            const Vec3 side = length > 0.0f ? Vec3{-(q.y - p.y) / length, (q.x - p.x) / length, 0.0f} : Vec3{};

            for (int k = 1; k <= pieces; ++k)
            {
                const float t = static_cast<float>(k) / static_cast<float>(pieces);
                Vec3 point = p + (q - p) * t;

                if (k < pieces)
                {
                    const Vec3 onward = p + (q - p) * (static_cast<float>(k + 1) / static_cast<float>(pieces));
                    const float offset = amplitude * std::sin(phase + (walked + t * length) / wavelength * 6.2831853f);
                    const Vec3 bent = point + side * offset;

                    if (clearPoint(bent) && clearLeg(swayed.back(), bent) && clearLeg(bent, onward))
                    {
                        point = bent;
                    }
                }

                swayed.push_back(point);
            }

            walked += length;
        }

        // Corners rounded off once (Chaikin), the ends kept where they are.
        std::vector<Vec3> rounded{swayed.front()};

        for (std::size_t i = 1; i + 1 < swayed.size(); ++i)
        {
            const Vec3 in = swayed[i - 1] * 0.25f + swayed[i] * 0.75f;
            const Vec3 onward = swayed[i] * 0.75f + swayed[i + 1] * 0.25f;

            if (clearLeg(in, onward))
            {
                rounded.push_back(in);
                rounded.push_back(onward);
            }
            else
            {
                rounded.push_back(swayed[i]);
            }
        }

        rounded.push_back(swayed.back());

        out.insert(out.end(), rounded.begin() + 1, rounded.end());

        if (stopEnds)
        {
            stopEnds->push_back(out.size() - 1);
        }

        at = stop;
    }

    return out;
}

} // namespace og
