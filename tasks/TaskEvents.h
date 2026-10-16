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
 *  File: TaskEvents.h
 *
 *  Desc:
 *  Declarations and interface for TaskEvents (task graph definitions,
 *  execution, and validation).
 *
 *
 */
#pragma once

#include "og/tasks/PlanStep.h"
#include "og/tasks/TaskTypes.h"

#include <string>

namespace og
{

struct PlanStartedEvent
{
    std::string planId;
    std::string goal;
    std::size_t totalSteps = 0;
};

struct PlanStepStartedEvent
{
    std::string planId;
    std::size_t stepIndex = 0;
    PlanStep step;
};

struct PlanStepCompletedEvent
{
    std::string planId;
    std::size_t stepIndex = 0;
    PlanStep step;
    TaskResult result;
};

struct PlanStepFailedEvent
{
    std::string planId;
    std::size_t stepIndex = 0;
    PlanStep step;
    TaskResult result;
};

struct PlanStepInterruptedEvent
{
    std::string planId;
    std::size_t stepIndex = 0;
    PlanStep step;
    std::string reason;
    TaskResult result;
};

// Exactly once per terminal task attempt, including partial progress and cancellation.
// Consumers must not also count the status-specific events as separate results.
struct PlanStepResultEvent
{
    std::string planId;
    std::size_t stepIndex = 0;
    PlanStep step;
    TaskResult result;
};

struct PlanCompletedEvent
{
    std::string planId;
    std::string goal;
    std::size_t stepsExecuted = 0;
};

struct PlanAbortedEvent
{
    std::string planId;
    std::string reason;
};

} // namespace og
