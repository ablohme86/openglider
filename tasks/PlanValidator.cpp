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
 *  File: PlanValidator.cpp
 *
 *  Desc:
 *  Implementation of PlanValidator (task graph definitions, execution,
 *  and validation).
 *
 *
 */
#include "og/tasks/PlanValidator.h"

#include <format>
#include <cmath>

namespace og
{

std::string PlanValidationResult::describe() const
{
    if (valid)
    {
        return "Valid";
    }

    std::string s = "Invalid: ";

    for (std::size_t i = 0; i < errors.size(); ++i)
    {
        if (i > 0)
        {
            s += "; ";
        }

        s += errors[i];
    }

    return s;
}

PlanValidationResult PlanValidator::validate(const Plan& plan) const
{
    PlanValidationResult res;

    if (plan.steps.empty())
    {
        res.valid = false;
        res.errors.push_back("plan contains no steps");
        return res;
    }

    if (plan.steps.size() > maxSteps_)
    {
        res.valid = false;
        res.errors.push_back(std::format("plan contains {} steps, exceeding limit of {}", plan.steps.size(), maxSteps_));
    }

    for (std::size_t i = 0; i < plan.steps.size(); ++i)
    {
        const auto& step = plan.steps[i];

        if (!std::isfinite(step.position.x) || !std::isfinite(step.position.y) || !std::isfinite(step.position.z) || !std::isfinite(step.arrivalDistance) || step.arrivalDistance <= 0)
        {
            res.valid = false;
            res.errors.push_back(std::format("step {}: invalid navigation coordinates or arrival distance", i));
        }

        switch (step.type)
        {
        case PlanStepType::GoTo:
            if (step.position.x == 0.0f && step.position.y == 0.0f && step.position.z == 0.0f && step.locationName.empty())
            {
                res.valid = false;
                res.errors.push_back(std::format("step {} [GoTo]: missing position or locationName", i));
            }

            break;

        case PlanStepType::AcceptQuest:
            if (step.questId <= 0)
            {
                res.valid = false;
                res.errors.push_back(std::format("step {} [AcceptQuest]: invalid questId ({})", i, step.questId));
            }

            break;

        case PlanStepType::CompleteQuestObjective:
            if (step.questId <= 0 || step.objectiveIndex < 0)
            {
                res.valid = false;
                res.errors.push_back(std::format("step {} [CompleteQuestObjective]: invalid questId or objectiveIndex", i));
            }

            break;

        case PlanStepType::TurnInQuest:
            if (step.questId <= 0)
            {
                res.valid = false;
                res.errors.push_back(std::format("step {} [TurnInQuest]: invalid questId ({})", i, step.questId));
            }

            break;

        case PlanStepType::Wait:
            if (step.waitDuration.count() <= 0)
            {
                res.valid = false;
                res.errors.push_back(std::format("step {} [Wait]: wait duration must be positive", i));
            }

            break;

        case PlanStepType::Grind:
        case PlanStepType::GatherMining:
        case PlanStepType::GatherHerbalism:
        case PlanStepType::Fish:
        case PlanStepType::Vendor:
        case PlanStepType::Repair:
        case PlanStepType::Train:
        case PlanStepType::UseFlightPath:
        case PlanStepType::Interact:
        case PlanStepType::Stop:
            break;
        default:
            res.valid = false;
            res.errors.push_back(std::format("step {}: unknown task type", i));
            break;
        }
    }

    return res;
}

} // namespace og
