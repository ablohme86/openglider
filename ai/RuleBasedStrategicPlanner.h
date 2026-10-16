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
 *  File: RuleBasedStrategicPlanner.h
 *
 *  Desc:
 *  Declarations and interface for RuleBasedStrategicPlanner (strategic
 *  and tactical AI planning systems).
 *
 *
 */
#pragma once

#include "og/ai/CandidateGenerator.h"
#include "og/ai/IStrategicPlanner.h"
#include "og/world/QuestDatabase.h"
#include "og/world/TravelGraph.h"

namespace og
{

class RuleBasedStrategicPlanner final : public IStrategicPlanner
{
public:
    RuleBasedStrategicPlanner(const QuestDatabase& questDb, const TravelGraph& travelGraph, CandidateGenerator candidateGen = CandidateGenerator{}) : questDb_(questDb), travelGraph_(travelGraph), candidateGen_(candidateGen)
    {
    }

    Plan createPlan(const WorldModel& world, const BotGoal& goal) override;

private:
    const QuestDatabase& questDb_;
    const TravelGraph& travelGraph_;
    CandidateGenerator candidateGen_;
};

} // namespace og
