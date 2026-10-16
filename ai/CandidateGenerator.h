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
 *  File: CandidateGenerator.h
 *
 *  Desc:
 *  Declarations and interface for CandidateGenerator (strategic and
 *  tactical AI planning systems).
 *
 *
 */
#pragma once

#include "og/ai/PlannerTypes.h"
#include "og/world/QuestClusterer.h"
#include "og/world/QuestDatabase.h"
#include "og/world/QuestScorer.h"
#include "og/world/TravelGraph.h"
#include "og/world/WorldModel.h"

#include <vector>

namespace og
{

class CandidateGenerator
{
public:
    CandidateGenerator(QuestClusterer clusterer = QuestClusterer{}, QuestScorer scorer = QuestScorer{}) : clusterer_(clusterer), scorer_(scorer)
    {
    }

    std::vector<CandidatePlan> generateCandidates(const WorldModel& world, const QuestDatabase& questDb, const TravelGraph& travelGraph, const BotGoal& goal) const;

private:
    QuestClusterer clusterer_;
    QuestScorer scorer_;
};

} // namespace og
