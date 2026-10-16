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
 *  File: QuestScorer.h
 *
 *  Desc:
 *  Declarations and interface for QuestScorer (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/world/QuestClusterer.h"
#include "og/world/QuestDatabase.h"

namespace og
{

struct HistoricalQuestStats
{
    int attempts = 0;
    int completions = 0;
    int deaths = 0;
    double avgMinutes = 10.0;
    double failureRate = 0.0;
};

class QuestScorer
{
public:
    QuestScorer() = default;

    double scoreQuest(const QuestDefinition& quest, const Vec3& playerPos, int playerLevel, const HistoricalQuestStats* stats = nullptr) const;

    double scoreCluster(const QuestCluster& cluster, const QuestDatabase& db, const Vec3& playerPos, int playerLevel) const;
};

} // namespace og
