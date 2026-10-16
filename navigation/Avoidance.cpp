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
 *  File: Avoidance.cpp
 *
 *  Desc:
 *  Implementation of Avoidance (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/Avoidance.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <limits>
#include <unordered_map>

namespace og
{
bool clearOfZones(const Vec3& from, const Vec3& to, std::span<const AvoidZone> zones, float margin)
{
    const Vec3 delta = to - from;
    const float length2 = delta.x * delta.x + delta.y * delta.y;

    for (const auto& zone : zones)
    {
        const float radius = zone.radius + margin;

        if (zone.apart(from.z, to.z))
        {
            continue;
        }

        if (zone.center.x < std::min(from.x, to.x) - radius || zone.center.x > std::max(from.x, to.x) + radius || zone.center.y < std::min(from.y, to.y) - radius || zone.center.y > std::max(from.y, to.y) + radius)
        {
            continue;
        }

        const float t = length2 > 0 ? std::clamp(((zone.center.x - from.x) * delta.x + (zone.center.y - from.y) * delta.y) / length2, 0.0f, 1.0f) : 0.0f;

        if (distance2D(from + delta * t, zone.center) <= zone.radius + margin)
        {
            return false;
        }
    }

    return true;
}

std::optional<Path> pathAroundZones(const Vec3& from, const Vec3& to, std::span<const AvoidZone> zones, float margin, float step, std::stop_token stop, const std::function<void(int)>& progress)
{
    if (stop.stop_requested())
    {
        return std::nullopt;
    }

    if (clearOfZones(from, to, zones, margin))
    {
        return Path{{to}};
    }

    if (!clearOfZones(from, from, zones) || !clearOfZones(to, to, zones))
    {
        return std::nullopt;
    }

    // Index the painted circles once per search. Each candidate leg only
    // checks circles in the grid cells it crosses, rather than every brush
    // stamp in the profile for each of the thousands of A* steps.
    constexpr double kCell = 32.0;
    const auto cellOf = [](double coordinate) { return static_cast<int>(std::floor(coordinate / kCell)); };
    const auto keyOf = [](int x, int y) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y); };
    std::unordered_map<std::uint64_t, std::vector<const AvoidZone*>> bins;
    std::vector<const AvoidZone*> large;

    for (const auto& z : zones)
    {
        if (stop.stop_requested())
        {
            return std::nullopt;
        }

        const double radius = z.radius + std::max(0.0f, margin);
        const int minX = cellOf(z.center.x - radius), maxX = cellOf(z.center.x + radius);
        const int minY = cellOf(z.center.y - radius), maxY = cellOf(z.center.y + radius);

        // Very large zones remain a direct check, keeping index memory bounded.
        if ((static_cast<double>(maxX) - minX + 1) * (static_cast<double>(maxY) - minY + 1) > 256)
        {
            large.push_back(&z);
            continue;
        }

        for (int x = minX; x <= maxX; ++x)
        {
            for (int y = minY; y <= maxY; ++y)
            {
                bins[keyOf(x, y)].push_back(&z);
            }
        }
    }

    // A player just outside an obstacle may leave the clearance buffer, but
    // must never move closer to that obstacle or enter its actual area.
    const auto clear = [&](const Vec3& a, const Vec3& b)
    {
        const auto zoneClear = [&](const AvoidZone& z)
        {
            const float radius = z.radius + margin;

            if (z.apart(a.z, b.z))
            {
                return true;
            }

            if (z.center.x < std::min(a.x, b.x) - radius || z.center.x > std::max(a.x, b.x) + radius || z.center.y < std::min(a.y, b.y) - radius || z.center.y > std::max(a.y, b.y) + radius)
            {
                return true;
            }

            // A destination just outside a zone is reached along its edge.
            float padding = distance2D(to, z.center) <= z.radius + margin ? 0.0f : margin;
            const float d = distance2D(a, z.center);

            if (d <= z.radius + margin)
            {
                const Vec3 direction = b - a;
                const Vec3 outward = a - z.center;

                if (direction.x * outward.x + direction.y * outward.y < 0)
                {
                    return false;
                }

                padding = 0;
            }

            return clearOfZones(a, b, std::span(&z, 1), padding);
        };

        for (const auto* z : large)
        {
            if (!zoneClear(*z))
            {
                return false;
            }
        }

        int x = cellOf(a.x), y = cellOf(a.y);
        const int endX = cellOf(b.x), endY = cellOf(b.y);
        const double dx = static_cast<double>(b.x) - a.x, dy = static_cast<double>(b.y) - a.y;
        const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
        const double infinity = std::numeric_limits<double>::infinity();
        const double stepX = dx == 0 ? infinity : kCell / std::fabs(dx);
        const double stepY = dy == 0 ? infinity : kCell / std::fabs(dy);
        double nextX = dx == 0 ? infinity : ((x + (sx > 0 ? 1.0 : 0.0)) * kCell - a.x) / dx;
        double nextY = dy == 0 ? infinity : ((y + (sy > 0 ? 1.0 : 0.0)) * kCell - a.y) / dy;

        for (;;)
        {
            if (stop.stop_requested())
            {
                return false;
            }

            if (const auto bin = bins.find(keyOf(x, y)); bin != bins.end())
            {
                for (const auto* z : bin->second)
                {
                    if (!zoneClear(*z))
                    {
                        return false;
                    }
                }
            }

            if (x == endX && y == endY)
            {
                break;
            }

            // Walk the segment through the grid, including both cells when it
            // crosses a corner. Clamp axes already at the destination cell.
            if (x != endX && (y == endY || nextX <= nextY))
            {
                x += sx;
                nextX += stepX;
            }
            else
            {
                y += sy;
                nextY += stepY;
            }
        }

        return true;
    };
    using Cell = std::pair<int, int>;
    struct Node
    {
        float cost;
        Cell parent;
    };
    using Entry = std::pair<float, Cell>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    std::map<Cell, Node> nodes;
    const auto position = [&](Cell c)
    {
        Vec3 p{from.x + c.first * step, from.y + c.second * step, from.z};
        const float total = distance2D(from, to);

        if (total > 0)
        {
            p.z += (to.z - from.z) * std::clamp(distance2D(from, p) / total, 0.0f, 1.0f);
        }

        return p;
    };
    const Cell start{0, 0};
    nodes[start] = {0, start};
    open.push({distance2D(from, to), start});
    // The budget grows with the trip: going round a large painted area on the
    // way to somewhere a few hundred yards off takes tens of thousands of cells
    // (about 50000 for 480 yd in Elwynn), and a fixed 8192 found no way at all.
    const float cells = distance2D(from, to) / step;
    const int budget = static_cast<int>(std::clamp(cells * cells * 2.0f, 8192.0f, 120000.0f));

    for (int expanded = 0; !open.empty() && expanded < budget; ++expanded)
    {
        if (stop.stop_requested())
        {
            return std::nullopt;
        }

        if (progress && expanded % 256 == 0)
        {
            progress(std::min(99, expanded * 100 / budget));
        }

        const auto [estimate, cell] = open.top();
        open.pop();
        const Vec3 at = position(cell);
        const float cost = nodes.at(cell).cost;

        if (estimate > cost + distance2D(at, to) + 0.01f)
        {
            continue;
        }

        if (clear(at, to))
        {
            std::vector<Vec3> reverse{to};

            for (Cell c = cell; c != start; c = nodes.at(c).parent)
            {
                reverse.push_back(position(c));
            }

            std::reverse(reverse.begin(), reverse.end());
            Path result;
            Vec3 previous = from;

            for (std::size_t i = 0; i < reverse.size();)
            {
                std::size_t next = reverse.size() - 1;

                while (next > i && !clear(previous, reverse[next]))
                {
                    --next;
                }

                result.points.push_back(reverse[next]);
                previous = reverse[next];
                i = next + 1;
            }

            return result;
        }

        for (int x = -1; x <= 1; ++x)
        {
            for (int y = -1; y <= 1; ++y)
            {
                if (x == 0 && y == 0)
                {
                    continue;
                }

                const Cell next{cell.first + x, cell.second + y};
                const Vec3 p = position(next);
                const float candidate = cost + distance2D(at, p);
                auto found = nodes.find(next);

                if (found != nodes.end() && found->second.cost <= candidate)
                {
                    continue;
                }

                if (!clear(at, p))
                {
                    continue;
                }

                nodes[next] = {candidate, cell};
                open.push({candidate + distance2D(p, to), next});
            }
        }
    }

    return std::nullopt;
}

} // namespace og
