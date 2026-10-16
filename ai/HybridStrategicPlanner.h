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
 *  File: HybridStrategicPlanner.h
 *
 *  Desc:
 *  Declarations and interface for HybridStrategicPlanner (strategic and
 *  tactical AI planning systems).
 *
 *
 */
#pragma once

#include "og/ai/CandidateGenerator.h"
#include "og/ai/ILlmClient.h"
#include "og/ai/IStrategicPlanner.h"
#include "og/ai/RuleBasedStrategicPlanner.h"
#include "og/tasks/PlanValidator.h"
#include "og/world/QuestDatabase.h"
#include "og/world/TravelGraph.h"

#include <memory>

namespace og
{

class HybridStrategicPlanner final : public IStrategicPlanner
{
public:
    HybridStrategicPlanner(std::shared_ptr<ILlmClient> llmClient, const QuestDatabase& questDb, const TravelGraph& travelGraph, CandidateGenerator candidateGen = CandidateGenerator{}, PlanValidator validator = PlanValidator{}) : llmClient_(std::move(llmClient)), questDb_(questDb), travelGraph_(travelGraph), candidateGen_(candidateGen), validator_(validator), fallbackPlanner_(questDb, travelGraph, candidateGen)
    {
    }

    Plan createPlan(const WorldModel& world, const BotGoal& goal) override;

    bool lastUsedFallback() const
    {
        return lastUsedFallback_;
    }

    const std::string& lastLlmReason() const
    {
        return lastLlmReason_;
    }

    const std::string& lastFallbackReason() const
    {
        return lastFallbackReason_;
    }

private:
    std::string buildPrompt(const WorldModel& world, const BotGoal& goal, const std::vector<CandidatePlan>& candidates) const;

    std::shared_ptr<ILlmClient> llmClient_;
    const QuestDatabase& questDb_;
    const TravelGraph& travelGraph_;
    CandidateGenerator candidateGen_;
    PlanValidator validator_;
    RuleBasedStrategicPlanner fallbackPlanner_;

    bool lastUsedFallback_ = false;
    std::string lastLlmReason_;
    std::string lastFallbackReason_;
};

} // namespace og
