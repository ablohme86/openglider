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
 *  File: QuestClusterer.cpp
 *
 *  Desc:
 *  Implementation of QuestClusterer (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/QuestClusterer.h"

#include "og/common/MathUtil.h"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace og
{

bool QuestCluster::containsQuest(int questId) const
{
    return std::find(questIds.begin(), questIds.end(), questId) != questIds.end();
}

Vec3 QuestClusterer::computeQuestCenter(const QuestDefinition& q)
{
    Vec3 sum{0.0f, 0.0f, 0.0f};
    int count = 0;

    for (const auto& obj : q.objectives)
    {
        for (const auto& loc : obj.knownLocations)
        {
            sum += loc;
            ++count;
        }
    }

    if (count > 0)
    {
        return sum / static_cast<float>(count);
    }

    if (q.endLocation.x != 0.0f || q.endLocation.y != 0.0f)
    {
        return q.endLocation;
    }

    return q.startLocation;
}

std::vector<QuestCluster> QuestClusterer::clusterQuests(const std::vector<const QuestDefinition*>& quests) const
{
    std::vector<QuestCluster> clusters;

    if (quests.empty())
    {
        return clusters;
    }

    std::unordered_set<int> visited;

    for (std::size_t i = 0; i < quests.size(); ++i)
    {
        const auto* qA = quests[i];

        if (!qA || visited.contains(qA->id))
        {
            continue;
        }

        QuestCluster cluster;
        cluster.clusterId = std::format("cluster_{}", qA->id);
        cluster.questIds.push_back(qA->id);
        visited.insert(qA->id);

        Vec3 centerA = computeQuestCenter(*qA);
        std::vector<Vec3> allPoints;
        allPoints.push_back(centerA);

        for (std::size_t j = i + 1; j < quests.size(); ++j)
        {
            const auto* qB = quests[j];

            if (!qB || visited.contains(qB->id))
            {
                continue;
            }

            Vec3 centerB = computeQuestCenter(*qB);
            const float dist = distance2D(centerA, centerB);

            // Group if geographically close or sharing turn-in NPC
            const bool sameNpc = qA->turnInNpcId > 0 && qA->turnInNpcId == qB->turnInNpcId;

            if (dist <= maxClusterDistance_ || sameNpc)
            {
                cluster.questIds.push_back(qB->id);
                visited.insert(qB->id);
                allPoints.push_back(centerB);
            }
        }

        // Recompute cluster center and radius
        Vec3 avg{0.0f, 0.0f, 0.0f};

        for (const auto& p : allPoints)
        {
            avg += p;
        }

        cluster.centerPosition = avg / static_cast<float>(allPoints.size());

        float maxR = 25.0f;

        for (const auto& p : allPoints)
        {
            const float d = distance2D(cluster.centerPosition, p);

            if (d > maxR)
            {
                maxR = d;
            }
        }

        cluster.radius = maxR;

        // Name
        if (cluster.questIds.size() == 1)
        {
            cluster.name = qA->name;
        }
        else
        {
            cluster.name = std::format("{} + {} quests", qA->name, cluster.questIds.size() - 1);
        }

        cluster.hub = qA->turnInNpcName.empty() ? qA->questGiverName : qA->turnInNpcName;

        clusters.push_back(std::move(cluster));
    }

    return clusters;
}

} // namespace og
