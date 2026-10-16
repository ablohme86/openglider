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
 *  File: TacticalPlanner.cpp
 *
 *  Desc:
 *  Implementation of TacticalPlanner (strategic and tactical AI
 *  planning systems).
 *
 *
 */
#include "og/ai/TacticalPlanner.h"

namespace og
{

namespace
{
bool addressesMaintenance(const Plan& plan, PlanStepType needed)
{
    for (std::size_t i = plan.currentStepIndex; i < plan.steps.size(); ++i)
    {
        const auto type = plan.steps[i].type;

        if (type == needed)
        {
            return true;
        }

        if (type != PlanStepType::GoTo && type != PlanStepType::Vendor && type != PlanStepType::Repair && type != PlanStepType::Wait)
        {
            return false;
        }
    }

    return false;
}

} // namespace

PlanAdjustment TacticalPlanner::reevaluate(const WorldModel& world, const Plan& currentPlan, const TaskResult& lastResult)
{
    PlanAdjustment adj;

    // 1. Durability emergency
    if (!lastResult.isFailed() && world.player.durabilityPercent < currentPlan.constraints.minDurabilityPercent && !addressesMaintenance(currentPlan, PlanStepType::Repair))
    {
        adj.type = PlanAdjustmentType::AbortPlan;
        adj.reason = "Low durability, aborting plan for repair";
        return adj;
    }

    // 2. Bags completely full
    if (!lastResult.isFailed() && world.player.freeBagSlots >= 0 && (world.player.freeBagSlots == 0 || world.player.freeBagSlots < currentPlan.constraints.minFreeBagSlots) && !addressesMaintenance(currentPlan, PlanStepType::Vendor))
    {
        adj.type = PlanAdjustmentType::AbortPlan;
        adj.reason = "Inventory full or below plan bag-space threshold, vendor visit required";
        return adj;
    }

    // 3. Task failure evaluation
    if (lastResult.isFailed())
    {
        if (lastResult.failureReason == TaskFailureReason::TooManyDeaths)
        {
            adj.type = PlanAdjustmentType::AbortPlan;
            adj.reason = "Too many deaths encountered during task execution, aborting plan";
            return adj;
        }

        if (lastResult.failureReason == TaskFailureReason::Unreachable)
        {
            adj.type = PlanAdjustmentType::AbortPlan;
            adj.reason = "Target location unreachable; remaining steps may depend on reaching it";
            return adj;
        }

        if (lastResult.failureReason == TaskFailureReason::InventoryFull)
        {
            adj.type = PlanAdjustmentType::AbortPlan;
            adj.reason = "Cannot loot/collect: inventory is full";
            return adj;
        }

        adj.type = PlanAdjustmentType::AbortPlan;
        adj.reason = "Task permanently failed: " + lastResult.details;
        return adj;
    }

    // Normal progression
    adj.type = PlanAdjustmentType::Continue;
    return adj;
}

} // namespace og
