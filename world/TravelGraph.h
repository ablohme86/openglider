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
 *  File: TravelGraph.h
 *
 *  Desc:
 *  Declarations and interface for TravelGraph (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Vec3.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace og
{

enum class TravelEdgeType
{
    Walk,
    Flight,
    Boat,
    Zeppelin,
    Portal,
    Hearth
};

std::string_view travelEdgeTypeToString(TravelEdgeType t);
TravelEdgeType travelEdgeTypeFromString(std::string_view s);

struct TravelNode
{
    std::string id;
    std::string name;
    Vec3 position{};
    std::string zone;
    int flightMasterId = 0;
    bool hasInnkeeper = false;
    bool hasVendor = false;
    bool hasRepair = false;
};

struct TravelEdge
{
    std::string fromNodeId;
    std::string toNodeId;
    TravelEdgeType type = TravelEdgeType::Walk;
    Duration duration = Duration(0);
    int flightPathId = 0;
    int minLevel = 1;
    int moneyCost = 0;
};

struct TravelRouteResult
{
    bool found = false;
    Duration totalDuration = Duration(0);
    std::vector<std::string> nodePath;
    std::vector<TravelEdge> edges;
};

class TravelGraph
{
public:
    TravelGraph() = default;

    void addNode(TravelNode node);
    void addEdge(TravelEdge edge);

    const TravelNode* findNode(std::string_view id) const;
    const TravelNode* findClosestNode(const Vec3& pos, std::string_view zone = {}) const;

    TravelRouteResult findShortestRoute(std::string_view startNodeId, std::string_view endNodeId, const std::unordered_set<int>& knownFlightPaths = {}, int playerLevel = 80) const;

    const std::unordered_map<std::string, TravelNode>& nodes() const
    {
        return nodes_;
    }

    const std::vector<TravelEdge>& edges() const
    {
        return edges_;
    }

private:
    std::unordered_map<std::string, TravelNode> nodes_;
    std::vector<TravelEdge> edges_;
    std::unordered_map<std::string, std::vector<std::size_t>> outgoingEdges_;
};

} // namespace og
