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
 *  File: TravelGraph.cpp
 *
 *  Desc:
 *  Implementation of TravelGraph (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/TravelGraph.h"

#include "og/common/MathUtil.h"

#include <algorithm>
#include <limits>
#include <queue>

namespace og
{

std::string_view travelEdgeTypeToString(TravelEdgeType t)
{
    switch (t)
    {
    case TravelEdgeType::Walk:
        return "Walk";
    case TravelEdgeType::Flight:
        return "Flight";
    case TravelEdgeType::Boat:
        return "Boat";
    case TravelEdgeType::Zeppelin:
        return "Zeppelin";
    case TravelEdgeType::Portal:
        return "Portal";
    case TravelEdgeType::Hearth:
        return "Hearth";
    }

    return "Unknown";
}

TravelEdgeType travelEdgeTypeFromString(std::string_view s)
{
    if (s == "Flight")
    {
        return TravelEdgeType::Flight;
    }

    if (s == "Boat")
    {
        return TravelEdgeType::Boat;
    }

    if (s == "Zeppelin")
    {
        return TravelEdgeType::Zeppelin;
    }

    if (s == "Portal")
    {
        return TravelEdgeType::Portal;
    }

    if (s == "Hearth")
    {
        return TravelEdgeType::Hearth;
    }

    return TravelEdgeType::Walk;
}

void TravelGraph::addNode(TravelNode node)
{
    std::string id = node.id;
    nodes_[id] = std::move(node);
}

void TravelGraph::addEdge(TravelEdge edge)
{
    const std::size_t idx = edges_.size();
    outgoingEdges_[edge.fromNodeId].push_back(idx);
    edges_.push_back(std::move(edge));
}

const TravelNode* TravelGraph::findNode(std::string_view id) const
{
    auto it = nodes_.find(std::string(id));
    return it != nodes_.end() ? &it->second : nullptr;
}

const TravelNode* TravelGraph::findClosestNode(const Vec3& pos, std::string_view zone) const
{
    const TravelNode* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();

    for (const auto& [_, node] : nodes_)
    {
        if (!zone.empty() && node.zone != zone)
        {
            continue;
        }

        const float d = distance2D(pos, node.position);

        if (d < bestDist)
        {
            bestDist = d;
            best = &node;
        }
    }

    return best;
}

TravelRouteResult TravelGraph::findShortestRoute(std::string_view startNodeId, std::string_view endNodeId, const std::unordered_set<int>& knownFlightPaths, int playerLevel) const
{
    TravelRouteResult result;

    if (startNodeId == endNodeId)
    {
        result.found = true;
        result.nodePath.push_back(std::string(startNodeId));
        return result;
    }

    if (!findNode(startNodeId) || !findNode(endNodeId))
    {
        return result;
    }

    using QueueItem = std::pair<int64_t, std::string>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> pq;

    std::unordered_map<std::string, int64_t> distances;
    std::unordered_map<std::string, std::pair<std::string, std::size_t>> previous; // node -> (prevNode, edgeIndex)

    distances[std::string(startNodeId)] = 0;
    pq.push({0, std::string(startNodeId)});

    while (!pq.empty())
    {
        auto [currentDist, currentId] = pq.top();
        pq.pop();

        if (currentDist > distances[currentId])
        {
            continue;
        }

        if (currentId == endNodeId)
        {
            break;
        }

        auto it = outgoingEdges_.find(currentId);

        if (it == outgoingEdges_.end())
        {
            continue;
        }

        for (std::size_t edgeIdx : it->second)
        {
            const auto& edge = edges_[edgeIdx];

            // Flight check
            if (edge.type == TravelEdgeType::Flight && edge.flightPathId > 0 && !knownFlightPaths.contains(edge.flightPathId))
            {
                continue;
            }

            // Level check
            if (playerLevel < edge.minLevel)
            {
                continue;
            }

            const int64_t edgeWeight = edge.duration.count();
            const int64_t newDist = currentDist + edgeWeight;

            auto distIt = distances.find(edge.toNodeId);

            if (distIt == distances.end() || newDist < distIt->second)
            {
                distances[edge.toNodeId] = newDist;
                previous[edge.toNodeId] = {currentId, edgeIdx};
                pq.push({newDist, edge.toNodeId});
            }
        }
    }

    std::string target(endNodeId);

    if (!previous.contains(target))
    {
        return result; // No route found
    }

    // Reconstruct path
    std::vector<std::string> pathNodes;
    std::vector<TravelEdge> pathEdges;

    std::string curr = target;

    while (curr != startNodeId)
    {
        pathNodes.push_back(curr);
        const auto& [prevNode, edgeIdx] = previous[curr];
        pathEdges.push_back(edges_[edgeIdx]);
        curr = prevNode;
    }

    pathNodes.push_back(std::string(startNodeId));

    std::reverse(pathNodes.begin(), pathNodes.end());
    std::reverse(pathEdges.begin(), pathEdges.end());

    result.found = true;
    result.totalDuration = Duration(distances[target]);
    result.nodePath = std::move(pathNodes);
    result.edges = std::move(pathEdges);
    return result;
}

} // namespace og
