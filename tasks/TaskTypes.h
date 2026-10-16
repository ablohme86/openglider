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
 *  File: TaskTypes.h
 *
 *  Desc:
 *  Declarations and interface for TaskTypes (task graph definitions,
 *  execution, and validation).
 *
 *
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace og
{

enum class TaskStatus
{
    InProgress,
    Success,
    Failed,
    Cancelled,
    Interrupted,
    PartiallyCompleted
};

std::string_view taskStatusToString(TaskStatus s);
TaskStatus taskStatusFromString(std::string_view s);

enum class TaskFailureReason
{
    None,
    Unreachable,
    TooManyDeaths,
    Timeout,
    MissingNpc,
    MissingObject,
    InventoryFull,
    NavigationFailed,
    CombatTooDifficult,
    QuestStateMismatch,
    PreconditionsNotMet,
    InvalidPlan
};

std::string_view failureReasonToString(TaskFailureReason r);
TaskFailureReason failureReasonFromString(std::string_view s);

struct TaskResult
{
    TaskStatus status = TaskStatus::Success;
    TaskFailureReason failureReason = TaskFailureReason::None;
    std::string details;
    std::unordered_map<std::string, double> metrics;

    bool isDone() const
    {
        return status != TaskStatus::InProgress;
    }

    bool isSuccess() const
    {
        return status == TaskStatus::Success;
    }

    bool isFailed() const
    {
        return status == TaskStatus::Failed;
    }

    bool isInterrupted() const
    {
        return status == TaskStatus::Interrupted;
    }

    static TaskResult inProgress(std::string details = {})
    {
        return TaskResult{TaskStatus::InProgress, TaskFailureReason::None, std::move(details), {}};
    }

    static TaskResult success(std::string details = {}, std::unordered_map<std::string, double> metrics = {})
    {
        return TaskResult{TaskStatus::Success, TaskFailureReason::None, std::move(details), std::move(metrics)};
    }

    static TaskResult failed(TaskFailureReason reason, std::string details = {}, std::unordered_map<std::string, double> metrics = {})
    {
        return TaskResult{TaskStatus::Failed, reason, std::move(details), std::move(metrics)};
    }

    static TaskResult cancelled(std::string details = {})
    {
        return TaskResult{TaskStatus::Cancelled, TaskFailureReason::None, std::move(details), {}};
    }

    static TaskResult interrupted(std::string details = {})
    {
        return TaskResult{TaskStatus::Interrupted, TaskFailureReason::None, std::move(details), {}};
    }

    static TaskResult partiallyCompleted(std::string details = {}, std::unordered_map<std::string, double> metrics = {})
    {
        return TaskResult{TaskStatus::PartiallyCompleted, TaskFailureReason::None, std::move(details), std::move(metrics)};
    }
};

} // namespace og
