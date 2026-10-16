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
 *  File: PlanExecutor.h
 *
 *  Desc:
 *  Declarations and interface for PlanExecutor (task graph definitions,
 *  execution, and validation).
 *
 *
 */
#pragma once

#include "og/core/BotContext.h"
#include "og/state/GameSnapshot.h"
#include "og/tasks/Plan.h"
#include "og/tasks/PlanValidator.h"
#include "og/tasks/TaskEvents.h"
#include "og/tasks/TaskNodes.h"
#include "og/tasks/TaskTypes.h"

#include <optional>
#include <string>
#include <string_view>

namespace og
{

enum class PlanExecutorState
{
    Idle,
    StartingPlan,
    ExecutingStep,
    Suspended,
    PlanCompleted,
    Failed
};

std::string_view planExecutorStateToString(PlanExecutorState s);

class PlanExecutor
{
public:
    explicit PlanExecutor(TaskRegistry registry = TaskRegistry::createDefault(), PlanValidator validator = PlanValidator{});

    bool loadPlan(Plan plan, std::string* error = nullptr, EventBus* events = nullptr);
    void cancelPlan(std::string_view reason = {}, BotContext* ctx = nullptr);
    bool resumeStep();
    bool retryStep();
    void suspendStep(BotContext& ctx, std::string_view reason = "Reactive behavior took control");
    void failCurrentStep(TaskResult result, BotContext& ctx);

    TaskResult tick(const GameSnapshot& s, BotContext& ctx);

    bool hasActivePlan() const
    {
        return state_ == PlanExecutorState::StartingPlan || state_ == PlanExecutorState::ExecutingStep || state_ == PlanExecutorState::Suspended;
    }

    bool isCompleted() const
    {
        return state_ == PlanExecutorState::PlanCompleted;
    }

    bool isFailed() const
    {
        return state_ == PlanExecutorState::Failed;
    }

    bool isIdle() const
    {
        return state_ == PlanExecutorState::Idle;
    }

    bool isSuspended() const
    {
        return state_ == PlanExecutorState::Suspended;
    }

    const Plan* currentPlan() const
    {
        return plan_ ? &*plan_ : nullptr;
    }

    const PlanStep* currentStep() const
    {
        return plan_ ? plan_->currentStep() : nullptr;
    }

    PlanExecutorState state() const
    {
        return state_;
    }

    std::string_view stateName() const
    {
        return planExecutorStateToString(state_);
    }

    std::size_t stepsExecuted() const
    {
        return stepsExecuted_;
    }

    const TaskResult& lastResult() const
    {
        return lastResult_;
    }

    TaskRegistry& registry()
    {
        return registry_;
    }

    const TaskRegistry& registry() const
    {
        return registry_;
    }

    PlanValidator& validator()
    {
        return validator_;
    }

    const PlanValidator& validator() const
    {
        return validator_;
    }

private:
    TaskRegistry registry_;
    PlanValidator validator_;
    std::optional<Plan> plan_;
    PlanExecutorState state_ = PlanExecutorState::Idle;
    ITaskNode* currentNode_ = nullptr;
    std::size_t stepsExecuted_ = 0;
    TaskResult lastResult_;
    TimePoint stepStarted_{};
};

} // namespace og
