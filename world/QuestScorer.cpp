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
 *  File: QuestScorer.cpp
 *
 *  Desc:
 *  Implementation of QuestScorer (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/QuestScorer.h"

#include "og/common/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace og
{

double QuestScorer::scoreQuest(const QuestDefinition& quest, const Vec3& playerPos, int playerLevel, const HistoricalQuestStats* stats) const
{
    double score = 50.0;

    // XP value
    const double xpValue = std::min(35.0, static_cast<double>(quest.rewardXp) / 100.0);
    score += xpValue;

    // Follow-up chain bonus
    if (!quest.followUps.empty())
    {
        score += 15.0;
    }

    // Level difficulty scaling
    const int levelDiff = quest.recommendedLevel - playerLevel;

    if (levelDiff > 2)
    {
        score -= (levelDiff - 2) * 15.0; // too high level
    }
    else if (levelDiff < -4)
    {
        score -= (-levelDiff - 4) * 8.0; // grey/low level quest
    }
    else
    {
        score += 10.0; // sweet spot
    }

    // Travel / distance cost
    const float dist = distance2D(playerPos, quest.startLocation);
    const double travelCost = std::min(35.0, dist * 0.04);
    score -= travelCost;

    // Historical failure & death penalty
    if (stats)
    {
        score -= stats->failureRate * 30.0;
        score -= std::min(25.0, stats->deaths * 6.0);
    }

    return std::clamp(score, 0.0, 100.0);
}

double QuestScorer::scoreCluster(const QuestCluster& cluster, const QuestDatabase& db, const Vec3& playerPos, int playerLevel) const
{
    if (cluster.questIds.empty())
    {
        return 0.0;
    }

    double sumScore = 0.0;

    for (int qid : cluster.questIds)
    {
        const auto* q = db.find(qid);

        if (q)
        {
            sumScore += scoreQuest(*q, playerPos, playerLevel);
        }
    }

    double avgScore = sumScore / static_cast<double>(cluster.questIds.size());

    // Clustering overlap bonus (doing multiple quests together in same area is huge!)
    if (cluster.questIds.size() > 1)
    {
        const double overlapBonus = (cluster.questIds.size() - 1) * 15.0;
        avgScore += overlapBonus;
    }

    // Distance to cluster center
    const float dist = distance2D(playerPos, cluster.centerPosition);
    avgScore -= std::min(20.0, dist * 0.02);

    return std::clamp(avgScore, 0.0, 100.0);
}

} // namespace og
