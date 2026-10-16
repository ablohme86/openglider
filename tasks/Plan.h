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
 *  File: Plan.h
 *
 *  Desc:
 *  Declarations and interface for Plan (task graph definitions,
 *  execution, and validation).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Json.h"
#include "og/tasks/PlanStep.h"

#include <cstddef>
#include <string>
#include <vector>

namespace og
{

struct PlanConstraints
{
    int maxDeaths = 3;
    int maxStuckEvents = 5;
    Duration maxDuration = Duration(3600000); // 1 hour
    int minFreeBagSlots = 2;
    float minDurabilityPercent = 25.0f;

    Json toJson() const;
    static PlanConstraints fromJson(const Json& j);
};

struct Plan
{
    std::string planId;
    std::string goal;
    std::string reason;
    std::string candidateId;
    double confidence = 1.0;
    std::vector<PlanStep> steps;
    PlanConstraints constraints;
    std::size_t currentStepIndex = 0;

    bool isComplete() const
    {
        return currentStepIndex >= steps.size();
    }

    const PlanStep* currentStep() const
    {
        return currentStepIndex < steps.size() ? &steps[currentStepIndex] : nullptr;
    }

    PlanStep* currentStep()
    {
        return currentStepIndex < steps.size() ? &steps[currentStepIndex] : nullptr;
    }

    bool advance()
    {
        if (currentStepIndex < steps.size())
        {
            ++currentStepIndex;
            return true;
        }

        return false;
    }

    std::size_t remainingSteps() const
    {
        return steps.size() > currentStepIndex ? steps.size() - currentStepIndex : 0;
    }

    Json toJson() const;
    static Plan fromJson(const Json& j);
};

} // namespace og
