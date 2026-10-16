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
 *  File: PlanningController.cpp
 *
 *  Desc:
 *  Implementation of PlanningController (strategic and tactical AI
 *  planning systems).
 *
 *
 */
#include "og/ai/PlanningController.h"

#include <algorithm>
#include <exception>
#include <atomic>

namespace og
{

PlanningController::PlanningController(IStrategicPlanner& strategic, ITacticalPlanner& tactical, const QuestDatabase& quests, TaskRegistry registry, PlanningControllerConfig config) : strategic_(strategic), tactical_(tactical), quests_(quests), config_(config), executor_(std::move(registry))
{
}

void PlanningController::start(BotGoal goal, BotContext& ctx)
{
    feedback_ = ctx.events.subscribe<PlanStepResultEvent>(
        [this](const auto& event)
        {
            if (executor_.currentPlan() && event.planId == executor_.currentPlan()->planId)
            {
                updater_.apply(world_, event, quests_);
                ++telemetry_.taskResults;
            }
        });
    stop(ctx, "Goal changed");
    goal_ = std::move(goal);
    running_ = true;
    observed_ = false;
    retries_ = 0;
    nextPlanAt_ = ctx.now();
    nextTacticalAt_ = ctx.now() + config_.tacticalInterval;
}

void PlanningController::stop(BotContext& ctx, std::string_view reason)
{
    executor_.cancelPlan(reason, &ctx);
    running_ = false;
    telemetry_.lastDecision = reason;
}

void PlanningController::suspend(BotContext& ctx)
{
    executor_.suspendStep(ctx);
}

bool PlanningController::goalReached() const
{
    if (goal_.type == BotGoalType::LevelTo || goal_.type == BotGoalType::GrindToLevel)
    {
        return goal_.targetLevel > 0 && world_.player.level >= goal_.targetLevel;
    }

    if (goal_.type == BotGoalType::CompleteQuest)
    {
        return goal_.targetQuestId > 0 && world_.quests.completedQuestIds.contains(goal_.targetQuestId);
    }

    return false;
}

bool PlanningController::loadPlan(Plan plan, BotContext& ctx)
{
    // Validate all node capabilities before executing any part of the plan.
    for (const auto& step : plan.steps)
    {
        if (world_.quests.ignoredQuestIds.contains(step.questId))
        {
            telemetry_.lastDecision = "Plan contains a user-ignored quest";
            return false;
        }
        if (!executor_.registry().find(step.type))
        {
            stop(ctx, "Plan contains an unregistered task type");
            return false;
        }
    }

    static std::atomic<std::uint64_t> nextPlanId{0};
    plan.planId = "execution_" + std::to_string(++nextPlanId);

    for (std::size_t i = 0; i < plan.steps.size(); ++i)
    {
        plan.steps[i].id = plan.planId + "_step_" + std::to_string(i);
    }

    std::string error;

    if (!executor_.loadPlan(std::move(plan), &error, &ctx.events))
    {
        stop(ctx, error);
        return false;
    }

    planStarted_ = ctx.now();
    startDeaths_ = ctx.stats.deaths;
    startStuck_ = ctx.stats.stuckEvents;
    retries_ = 0;
    nextTacticalAt_ = ctx.now() + config_.tacticalInterval;
    telemetry_.lastDecision = "Executing validated plan";
    return true;
}

void PlanningController::requestReplan(std::string reason, BotContext& ctx)
{
    executor_.cancelPlan(reason, &ctx);
    telemetry_.lastDecision = std::move(reason);
    nextPlanAt_ = std::max(nextPlanAt_, ctx.now() + config_.replanCooldown);
}

TaskResult PlanningController::reevaluate(const TaskResult& result, BotContext& ctx)
{
    if (result.isFailed() && !executor_.isFailed())
    {
        executor_.failCurrentStep(result, ctx);
    }

    ++telemetry_.tacticalCalls;
    nextTacticalAt_ = ctx.now() + config_.tacticalInterval;
    const auto adjustment = tactical_.reevaluate(world_, *executor_.currentPlan(), result);
    telemetry_.lastDecision = adjustment.reason;

    if (adjustment.type == PlanAdjustmentType::ReplacePlan && adjustment.newPlan)
    {
        executor_.cancelPlan(adjustment.reason, &ctx);

        if (!loadPlan(*adjustment.newPlan, ctx))
        {
            return TaskResult::failed(TaskFailureReason::InvalidPlan, telemetry_.lastDecision);
        }
    }
    else if (adjustment.type == PlanAdjustmentType::AbortPlan || adjustment.type == PlanAdjustmentType::AbortStep || result.isFailed())
    {
        // A later quest/vendor step may depend on the failed GoTo. Do not skip it.
        requestReplan(adjustment.reason.empty() ? result.details : adjustment.reason, ctx);
    }

    return result;
}

std::string PlanningController::currentStepError() const
{
    const auto* step = executor_.currentStep();

    if (!step)
    {
        return {};
    }

    if (step->type != PlanStepType::AcceptQuest && step->type != PlanStepType::TurnInQuest && step->type != PlanStepType::CompleteQuestObjective)
    {
        return {};
    }

    const auto* definition = quests_.find(step->questId);

    if (!definition)
    {
        return "Quest is absent from the quest database";
    }

    const auto& active = world_.quests.activeQuests;
    const auto quest = std::find_if(active.begin(), active.end(), [&](const auto& q) { return q.questId == step->questId; });

    if (step->type == PlanStepType::AcceptQuest)
    {
        if (quest != active.end() || world_.quests.completedQuestIds.contains(step->questId))
        {
            return "Quest is already active or completed";
        }

        if (world_.player.level < definition->minLevel)
        {
            return "Quest level requirement is not met";
        }

        for (int prerequisite : definition->prerequisites)
        {
            if (!world_.quests.completedQuestIds.contains(prerequisite))
            {
                return "Quest prerequisite is not completed";
            }
        }
    }
    else
    {
        if (quest == active.end() || quest->state == QuestRuntimeState::Failed || quest->state == QuestRuntimeState::Inactive)
        {
            return "Quest is no longer active";
        }

        if (step->type == PlanStepType::TurnInQuest && !quest->isComplete())
        {
            return "Quest is not ready for turn-in";
        }

        if (step->type == PlanStepType::CompleteQuestObjective && (step->objectiveIndex < 0 || static_cast<std::size_t>(step->objectiveIndex) >= definition->objectives.size()))
        {
            return "Quest objective index is invalid";
        }
    }

    return {};
}

TaskResult PlanningController::tick(const GameSnapshot& snapshot, BotContext& ctx)
{
    builder_.updateFromSnapshot(world_, snapshot);
    const std::unordered_set<int> ignored(ctx.ignoredQuests.begin(), ctx.ignoredQuests.end());
    if (world_.quests.ignoredQuestIds != ignored)
    {
        world_.quests.ignoredQuestIds = ignored;
        requestReplan("Quest ignore selection changed", ctx);
        nextPlanAt_ = ctx.now();
    }

    if (!running_)
    {
        return TaskResult::cancelled("Planning controller stopped");
    }

    if (goalReached())
    {
        stop(ctx, "Goal reached");
        return TaskResult::success("Goal reached");
    }

    const bool changed = observed_ && (lastLevel_ != world_.player.level || lastZone_ != snapshot.zoneId || (lastBagSlots_ != 0 && world_.player.freeBagSlots == 0));
    lastLevel_ = world_.player.level;
    lastZone_ = snapshot.zoneId;
    lastBagSlots_ = world_.player.freeBagSlots;
    observed_ = true;

    try
    {
        bool loadedPlan = false;

        if (!executor_.hasActivePlan())
        {
            if (ctx.now() < nextPlanAt_)
            {
                return TaskResult::inProgress("Waiting for replan cooldown");
            }

            ++telemetry_.strategicCalls;
            nextPlanAt_ = ctx.now() + config_.replanCooldown;

            if (!loadPlan(strategic_.createPlan(world_, goal_), ctx))
            {
                return TaskResult::failed(TaskFailureReason::InvalidPlan, telemetry_.lastDecision);
            }

            loadedPlan = true;
        }

        const auto& constraints = executor_.currentPlan()->constraints;

        if ((constraints.maxDuration.count() > 0 && ctx.now() - planStarted_ >= constraints.maxDuration) || (ctx.stats.deaths - startDeaths_ > 0 && ctx.stats.deaths - startDeaths_ >= constraints.maxDeaths) || (ctx.stats.stuckEvents - startStuck_ > 0 && ctx.stats.stuckEvents - startStuck_ >= constraints.maxStuckEvents))
        {
            const auto reason = ctx.stats.deaths - startDeaths_ > 0 && ctx.stats.deaths - startDeaths_ >= constraints.maxDeaths ? TaskFailureReason::TooManyDeaths : TaskFailureReason::Timeout;
            return reevaluate(TaskResult::failed(reason, "Plan execution limit reached"), ctx);
        }

        if (const auto error = currentStepError(); !error.empty())
        {
            return reevaluate(TaskResult::failed(TaskFailureReason::QuestStateMismatch, error), ctx);
        }

        const bool maintenanceNeeded = world_.player.durabilityPercent < constraints.minDurabilityPercent || (world_.player.freeBagSlots >= 0 && world_.player.freeBagSlots < constraints.minFreeBagSlots);

        if (changed || ctx.now() >= nextTacticalAt_ || (loadedPlan && maintenanceNeeded))
        {
            reevaluate(TaskResult::inProgress("World changed or tactical interval elapsed"), ctx);

            if (!running_ || !executor_.hasActivePlan())
            {
                return TaskResult::inProgress("Replanning requested");
            }
        }

        if (executor_.isSuspended())
        {
            return executor_.lastResult();
        }

        const auto result = executor_.tick(snapshot, ctx);

        if (result.isFailed())
        {
            if (result.failureReason == TaskFailureReason::PreconditionsNotMet || result.failureReason == TaskFailureReason::InvalidPlan)
            {
                stop(ctx, result.details);
                return result;
            }

            const auto* step = executor_.currentStep();
            const bool recoverable = step && step->type == PlanStepType::GoTo && (result.failureReason == TaskFailureReason::NavigationFailed || result.failureReason == TaskFailureReason::Unreachable || result.failureReason == TaskFailureReason::Timeout);

            if (recoverable && retries_ < config_.maxNavigationRetries && executor_.retryStep())
            {
                ++retries_;
                ++telemetry_.localRetries;
                telemetry_.lastDecision = "Retrying navigation locally";
                return TaskResult::inProgress(telemetry_.lastDecision);
            }

            return reevaluate(result, ctx);
        }

        if (result.status == TaskStatus::Cancelled)
        {
            running_ = false;
            telemetry_.lastDecision = result.details;
        }

        if (executor_.state() == PlanExecutorState::StartingPlan || executor_.isCompleted())
        {
            retries_ = 0;
        }

        return result;
    }
    catch (const std::exception& error)
    {
        stop(ctx, error.what());
        return TaskResult::failed(TaskFailureReason::InvalidPlan, error.what());
    }
}

} // namespace og
