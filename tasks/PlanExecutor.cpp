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
 *  File: PlanExecutor.cpp
 *
 *  Desc:
 *  Implementation of PlanExecutor (task graph definitions, execution,
 *  and validation).
 *
 *
 */
#include "og/tasks/PlanExecutor.h"

#include <format>
#include <algorithm>
#include <cmath>
#include <limits>

namespace og
{

std::string_view planExecutorStateToString(PlanExecutorState s)
{
    switch (s)
    {
    case PlanExecutorState::Idle:
        return "Idle";
    case PlanExecutorState::StartingPlan:
        return "StartingPlan";
    case PlanExecutorState::ExecutingStep:
        return "ExecutingStep";
    case PlanExecutorState::Suspended:
        return "Suspended";
    case PlanExecutorState::PlanCompleted:
        return "PlanCompleted";
    case PlanExecutorState::Failed:
        return "Failed";
    }

    return "Unknown";
}

PlanExecutor::PlanExecutor(TaskRegistry registry, PlanValidator validator) : registry_(std::move(registry)), validator_(validator)
{
}

bool PlanExecutor::loadPlan(Plan plan, std::string* error, EventBus* events)
{
    if (hasActivePlan())
    {
        if (error)
        {
            *error = "Cancel the active plan before replacing it";
        }

        return false;
    }

    PlanValidationResult v = validator_.validate(plan);

    if (!v.valid)
    {
        if (error)
        {
            *error = v.describe();
        }

        return false;
    }

    plan_ = std::move(plan);
    plan_->currentStepIndex = 0;
    stepsExecuted_ = 0;
    state_ = PlanExecutorState::StartingPlan;
    currentNode_ = nullptr;
    lastResult_ = TaskResult::inProgress("Plan loaded");

    if (events)
    {
        events->publish(PlanStartedEvent{plan_->planId, plan_->goal, plan_->steps.size()});
    }

    return true;
}

void PlanExecutor::cancelPlan(std::string_view reason, BotContext* ctx)
{
    if (hasActivePlan() && currentNode_ && plan_ && ctx)
    {
        if (const PlanStep* step = plan_->currentStep())
        {
            currentNode_->onCancel(*step, *ctx);
            ctx->events.publish(PlanStepResultEvent{plan_->planId, plan_->currentStepIndex, *step, TaskResult::cancelled(std::string(reason))});
        }
    }

    if (hasActivePlan() && plan_ && ctx)
    {
        ctx->events.publish(PlanAbortedEvent{plan_->planId, std::string(reason)});
    }

    state_ = PlanExecutorState::Idle;
    currentNode_ = nullptr;
    lastResult_ = TaskResult::cancelled(std::string(reason));
}

bool PlanExecutor::resumeStep()
{
    if (!isSuspended())
    {
        return false;
    }

    state_ = PlanExecutorState::StartingPlan;
    return true;
}

bool PlanExecutor::retryStep()
{
    if (!isFailed() || !plan_ || !plan_->currentStep())
    {
        return false;
    }

    state_ = PlanExecutorState::StartingPlan;
    return true;
}

void PlanExecutor::suspendStep(BotContext& ctx, std::string_view reason)
{
    if (state_ != PlanExecutorState::ExecutingStep || !currentNode_ || !currentStep())
    {
        return;
    }

    currentNode_->onCancel(*currentStep(), ctx);
    currentNode_ = nullptr;
    state_ = PlanExecutorState::Suspended;
    lastResult_ = TaskResult::interrupted(std::string(reason));
    lastResult_.metrics = {{"currentCount", currentStep()->currentCount}, {"elapsedTime", toSeconds(ctx.now() - stepStarted_)}};
    ctx.events.publish(PlanStepResultEvent{plan_->planId, plan_->currentStepIndex, *currentStep(), lastResult_});
    ctx.events.publish(PlanStepInterruptedEvent{plan_->planId, plan_->currentStepIndex, *currentStep(), lastResult_.details, lastResult_});
}

void PlanExecutor::failCurrentStep(TaskResult result, BotContext& ctx)
{
    if (!hasActivePlan() || !currentStep())
    {
        return;
    }

    if (currentNode_)
    {
        currentNode_->onCancel(*currentStep(), ctx);
    }

    currentNode_ = nullptr;
    state_ = PlanExecutorState::Failed;
    lastResult_ = std::move(result);
    ctx.events.publish(PlanStepResultEvent{plan_->planId, plan_->currentStepIndex, *currentStep(), lastResult_});
    ctx.events.publish(PlanStepFailedEvent{plan_->planId, plan_->currentStepIndex, *currentStep(), lastResult_});
}

TaskResult PlanExecutor::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (state_ == PlanExecutorState::Idle)
    {
        return TaskResult::success("Idle");
    }

    if (state_ == PlanExecutorState::PlanCompleted)
    {
        return TaskResult::success("PlanCompleted");
    }

    if (state_ == PlanExecutorState::Failed || state_ == PlanExecutorState::Suspended)
    {
        return lastResult_;
    }

    if (state_ == PlanExecutorState::StartingPlan)
    {
        if (!plan_ || plan_->isComplete())
        {
            state_ = PlanExecutorState::PlanCompleted;
            lastResult_ = TaskResult::success("Plan completed");

            if (plan_)
            {
                ctx.events.publish(PlanCompletedEvent{plan_->planId, plan_->goal, stepsExecuted_});
            }

            return lastResult_;
        }

        const PlanStep* step = plan_->currentStep();
        currentNode_ = registry_.find(step->type);

        if (!currentNode_)
        {
            state_ = PlanExecutorState::Failed;
            lastResult_ = TaskResult::failed(TaskFailureReason::InvalidPlan, std::format("No task node registered for step type {}", planStepTypeToString(step->type)));
            ctx.events.publish(PlanStepFailedEvent{plan_->planId, plan_->currentStepIndex, *step, lastResult_});
            ctx.events.publish(PlanStepResultEvent{plan_->planId, plan_->currentStepIndex, *step, lastResult_});
            return lastResult_;
        }

        ctx.events.publish(PlanStepStartedEvent{plan_->planId, plan_->currentStepIndex, *step});
        currentNode_->onStart(*step, s, ctx);
        stepStarted_ = ctx.now();
        state_ = PlanExecutorState::ExecutingStep;
    }

    if (state_ == PlanExecutorState::ExecutingStep)
    {
        if (!plan_ || !currentNode_)
        {
            state_ = PlanExecutorState::Failed;
            lastResult_ = TaskResult::failed(TaskFailureReason::InvalidPlan, "Missing plan or current node");
            return lastResult_;
        }

        PlanStep* step = plan_->currentStep();
        TaskResult res = currentNode_->tick(*step, s, ctx);

        if (!res.isDone())
        {
            lastResult_ = res;
            return res; // InProgress
        }

        res.metrics.try_emplace("elapsedTime", toSeconds(ctx.now() - stepStarted_));

        // Absolute progress survives a restart after interruption or local recovery.
        if (auto it = res.metrics.find("currentCount"); it != res.metrics.end() && std::isfinite(it->second) && it->second >= 0 && it->second <= std::numeric_limits<int>::max())
        {
            step->currentCount = std::max(step->currentCount, static_cast<int>(it->second));
        }

        if (!res.isSuccess())
        {
            currentNode_->onCancel(*step, ctx);
        }

        currentNode_ = nullptr;
        lastResult_ = res;
        ctx.events.publish(PlanStepResultEvent{plan_->planId, plan_->currentStepIndex, *step, res});

        if (res.isSuccess())
        {
            ctx.events.publish(PlanStepCompletedEvent{plan_->planId, plan_->currentStepIndex, *step, res});
            ++stepsExecuted_;
            plan_->advance();

            if (plan_->isComplete())
            {
                state_ = PlanExecutorState::PlanCompleted;
                lastResult_ = TaskResult::success("Plan completed", {{"stepsExecuted", static_cast<double>(stepsExecuted_)}});
                ctx.events.publish(PlanCompletedEvent{plan_->planId, plan_->goal, stepsExecuted_});
                return lastResult_;
            }

            state_ = PlanExecutorState::StartingPlan;
            return TaskResult::inProgress("Advancing to next step");
        }

        if (res.isFailed())
        {
            state_ = PlanExecutorState::Failed;
            ctx.events.publish(PlanStepFailedEvent{plan_->planId, plan_->currentStepIndex, *step, res});
            return res;
        }

        if (res.status == TaskStatus::Interrupted || res.status == TaskStatus::PartiallyCompleted)
        {
            state_ = PlanExecutorState::Suspended;
            ctx.events.publish(PlanStepInterruptedEvent{plan_->planId, plan_->currentStepIndex, *step, res.details, res});
            return res;
        }

        if (res.status == TaskStatus::Cancelled)
        {
            state_ = PlanExecutorState::Idle;
            ctx.events.publish(PlanAbortedEvent{plan_->planId, res.details});
            return res;
        }
    }

    return TaskResult::inProgress();
}

} // namespace og
