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
 *  File: RuleBasedStrategicPlanner.cpp
 *
 *  Desc:
 *  Implementation of RuleBasedStrategicPlanner (strategic and tactical
 *  AI planning systems).
 *
 *
 */
#include "og/ai/RuleBasedStrategicPlanner.h"

namespace og
{

Plan RuleBasedStrategicPlanner::createPlan(const WorldModel& world, const BotGoal& goal)
{
    auto candidates = candidateGen_.generateCandidates(world, questDb_, travelGraph_, goal);

    if (candidates.empty())
    {
        // Fallback safety grind plan
        Plan fallback;
        fallback.planId = "fallback_grind";
        fallback.goal = "Grind (safe fallback)";
        fallback.reason = "No candidates could be generated";
        PlanStep s;
        s.type = PlanStepType::Grind;
        s.targetLevel = world.player.level + 1;
        s.timeout = Duration(1800000);
        fallback.steps.push_back(s);
        return fallback;
    }

    // Top scoring candidate
    const auto& top = candidates.front();
    Plan chosen = top.plan;
    chosen.candidateId = top.id;
    chosen.confidence = 1.0;
    return chosen;
}

} // namespace og
