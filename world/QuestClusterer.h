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
 *  File: QuestClusterer.h
 *
 *  Desc:
 *  Declarations and interface for QuestClusterer (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/world/QuestTypes.h"

#include <string>
#include <vector>

namespace og
{

struct QuestCluster
{
    std::string clusterId;
    std::string name;
    std::vector<int> questIds;
    Vec3 centerPosition{};
    float radius = 50.0f;
    std::string hub;
    double score = 0.0;

    bool containsQuest(int questId) const;
};

class QuestClusterer
{
public:
    explicit QuestClusterer(float maxClusterDistance = 250.0f) : maxClusterDistance_(maxClusterDistance)
    {
    }

    std::vector<QuestCluster> clusterQuests(const std::vector<const QuestDefinition*>& quests) const;

    void setMaxClusterDistance(float d)
    {
        maxClusterDistance_ = d;
    }

    float maxClusterDistance() const
    {
        return maxClusterDistance_;
    }

private:
    float maxClusterDistance_ = 250.0f;

    static Vec3 computeQuestCenter(const QuestDefinition& q);
};

} // namespace og
