/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-17
 *  File: TaxiNetwork.cpp
 *
 *  Desc:
 *  Implementation of TaxiNetwork: the flight paths from the client's
 *  tables, and the choice between flying and walking.
 *
 */
#include "og/world/TaxiNetwork.h"

#include "og/common/Db2.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <queue>

namespace og
{

namespace
{

// The layouts read (WoW Forever 1.60.1, build 70450; Classic Era 1.15.9, build 70003).
// TaxiPath: ID, FromTaxiNode, ToTaxiNode, Cost. TaxiPathNode: Loc[3], ID, PathID,
// NodeIndex, ContinentID, Flags, Delay, ...
constexpr std::uint32_t kTaxiPathLayout = 0xA303DE51;
constexpr std::uint32_t kTaxiPathNodeLayout = 0xFE362E70;           // WoW Forever 1.60.1
constexpr std::uint32_t kTaxiPathNodeLayoutClassicEra = 0xBB94AA30; // Classic Era 1.15.9

float asFloat(std::uint32_t v)
{
    float f = 0;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

} // namespace

std::optional<TaxiNetwork> TaxiNetwork::fromDb2(std::span<const std::uint8_t> taxiPath, std::span<const std::uint8_t> taxiPathNode, std::string& error)
{
    const auto paths = Db2Table::parse(taxiPath, error);

    if (!paths)
    {
        error = "TaxiPath: " + error;
        return std::nullopt;
    }

    const auto points = Db2Table::parse(taxiPathNode, error);

    if (!points)
    {
        error = "TaxiPathNode: " + error;
        return std::nullopt;
    }

    const bool validPathNodeLayout = (points->layoutHash() == kTaxiPathNodeLayout || points->layoutHash() == kTaxiPathNodeLayoutClassicEra);

    if (paths->layoutHash() != kTaxiPathLayout || !validPathNodeLayout)
    {
        error = std::format("TaxiPath layout {:08X} / TaxiPathNode layout {:08X} are not ones the bot knows", paths->layoutHash(), points->layoutHash());
        return std::nullopt;
    }

    struct Path
    {
        std::uint32_t from = 0, to = 0, cost = 0;
        std::map<std::uint32_t, Vec3> points; // by NodeIndex
        std::uint32_t mapId = 0;
    };
    std::map<std::uint32_t, Path> byId;

    for (const auto& row : paths->rows())
    {
        if (row.fields.size() >= 4)
        {
            byId[row.fields[0]] = {row.fields[1], row.fields[2], row.fields[3], {}, 0};
        }
    }

    for (const auto& row : points->rows())
    {
        if (row.fields.size() < 7)
        {
            continue;
        }

        if (const auto it = byId.find(row.fields[4]); it != byId.end())
        {
            it->second.points[row.fields[5]] = {asFloat(row.fields[0]), asFloat(row.fields[1]), asFloat(row.fields[2])};
            it->second.mapId = row.fields[6];
        }
    }

    TaxiNetwork network;

    for (const auto& [id, path] : byId)
    {
        if (path.points.empty())
        {
            continue;
        }

        float length = 0.0f;
        const Vec3* last = nullptr;

        for (const auto& [index, point] : path.points)
        {
            if (last)
            {
                length += distance(*last, point);
            }

            last = &point;
        }

        if (!network.node(path.from))
        {
            network.addNode({path.from, path.mapId, path.points.begin()->second});
        }

        network.addFlight({path.from, path.to, path.cost, length});
    }

    if (network.nodes().empty())
    {
        error = "no flight point in the tables";
        return std::nullopt;
    }

    return network;
}

void TaxiNetwork::addNode(TaxiNode node)
{
    nodes_[node.id] = node;
}

void TaxiNetwork::addFlight(TaxiFlight flight)
{
    flights_.push_back(flight);
}

const TaxiNode* TaxiNetwork::node(std::uint32_t id) const
{
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

std::optional<TaxiNetwork::Route> TaxiNetwork::route(std::uint32_t from, std::uint32_t to, const std::vector<std::uint32_t>& known) const
{
    const auto isKnown = [&](std::uint32_t id) { return std::find(known.begin(), known.end(), id) != known.end(); };

    if (!isKnown(from) || !isKnown(to) || from == to)
    {
        return std::nullopt;
    }

    // The game flies on through the flight points learnt on the way.
    std::map<std::uint32_t, Route> best;
    using Item = std::pair<float, std::uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    best[from] = {};
    open.push({0.0f, from});

    while (!open.empty())
    {
        const auto [length, at] = open.top();
        open.pop();

        if (at == to)
        {
            return best[at];
        }

        if (length > best[at].length)
        {
            continue;
        }

        for (const auto& f : flights_)
        {
            if (f.from != at || !isKnown(f.to))
            {
                continue;
            }

            const Route next{length + f.length, best[at].cost + f.cost};
            const auto it = best.find(f.to);

            if (it == best.end() || next.length < it->second.length)
            {
                best[f.to] = next;
                open.push({next.length, f.to});
            }
        }
    }

    return std::nullopt;
}

std::optional<TaxiPlan> TaxiNetwork::plan(std::uint32_t mapId, const Vec3& at, const Vec3& destination, const std::vector<std::uint32_t>& known, std::int64_t money, float runSpeed, float walkFactor) const
{
    const auto walk = [&](const Vec3& a, const Vec3& b) { return distance2D(a, b) * walkFactor / runSpeed; };
    const float walkSeconds = walk(at, destination);
    std::optional<TaxiPlan> best;

    for (const auto from : known)
    {
        const auto* a = node(from);

        if (!a || a->mapId != mapId)
        {
            continue;
        }

        for (const auto to : known)
        {
            const auto* b = node(to);

            if (!b || b->mapId != mapId || to == from)
            {
                continue;
            }

            const auto flight = route(from, to, known);

            if (!flight || (money >= 0 && static_cast<std::int64_t>(flight->cost) > money))
            {
                continue;
            }

            const float seconds = walk(at, a->position) + flight->length / kFlightSpeed + kFlightOverhead + walk(b->position, destination);

            if (!best || seconds < best->seconds)
            {
                best = TaxiPlan{from, to, a->position, b->position, seconds, walkSeconds, flight->cost};
            }
        }
    }

    // Worth it: a minute saved at least, and a tenth of the time. From
    // Orgrimmar to Ratchet by the Crossroads saved two minutes of eight - a
    // quarter asked for, it walked.
    if (best && best->seconds < walkSeconds * 0.9f && walkSeconds - best->seconds >= 60.0f)
    {
        return best;
    }

    return std::nullopt;
}

} // namespace og
