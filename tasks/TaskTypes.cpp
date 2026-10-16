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
 *  File: TaskTypes.cpp
 *
 *  Desc:
 *  Implementation of TaskTypes (task graph definitions, execution, and
 *  validation).
 *
 *
 */
#include "og/tasks/TaskTypes.h"

namespace og
{

std::string_view taskStatusToString(TaskStatus s)
{
    switch (s)
    {
    case TaskStatus::InProgress:
        return "InProgress";
    case TaskStatus::Success:
        return "Success";
    case TaskStatus::Failed:
        return "Failed";
    case TaskStatus::Cancelled:
        return "Cancelled";
    case TaskStatus::Interrupted:
        return "Interrupted";
    case TaskStatus::PartiallyCompleted:
        return "PartiallyCompleted";
    }

    return "Unknown";
}

TaskStatus taskStatusFromString(std::string_view s)
{
    if (s == "InProgress")
    {
        return TaskStatus::InProgress;
    }

    if (s == "Success")
    {
        return TaskStatus::Success;
    }

    if (s == "Failed")
    {
        return TaskStatus::Failed;
    }

    if (s == "Cancelled")
    {
        return TaskStatus::Cancelled;
    }

    if (s == "Interrupted")
    {
        return TaskStatus::Interrupted;
    }

    if (s == "PartiallyCompleted")
    {
        return TaskStatus::PartiallyCompleted;
    }

    return TaskStatus::Failed;
}

std::string_view failureReasonToString(TaskFailureReason r)
{
    switch (r)
    {
    case TaskFailureReason::None:
        return "None";
    case TaskFailureReason::Unreachable:
        return "Unreachable";
    case TaskFailureReason::TooManyDeaths:
        return "TooManyDeaths";
    case TaskFailureReason::Timeout:
        return "Timeout";
    case TaskFailureReason::MissingNpc:
        return "MissingNpc";
    case TaskFailureReason::MissingObject:
        return "MissingObject";
    case TaskFailureReason::InventoryFull:
        return "InventoryFull";
    case TaskFailureReason::NavigationFailed:
        return "NavigationFailed";
    case TaskFailureReason::CombatTooDifficult:
        return "CombatTooDifficult";
    case TaskFailureReason::QuestStateMismatch:
        return "QuestStateMismatch";
    case TaskFailureReason::PreconditionsNotMet:
        return "PreconditionsNotMet";
    case TaskFailureReason::InvalidPlan:
        return "InvalidPlan";
    }

    return "Unknown";
}

TaskFailureReason failureReasonFromString(std::string_view s)
{
    if (s == "None")
    {
        return TaskFailureReason::None;
    }

    if (s == "Unreachable")
    {
        return TaskFailureReason::Unreachable;
    }

    if (s == "TooManyDeaths")
    {
        return TaskFailureReason::TooManyDeaths;
    }

    if (s == "Timeout")
    {
        return TaskFailureReason::Timeout;
    }

    if (s == "MissingNpc")
    {
        return TaskFailureReason::MissingNpc;
    }

    if (s == "MissingObject")
    {
        return TaskFailureReason::MissingObject;
    }

    if (s == "InventoryFull")
    {
        return TaskFailureReason::InventoryFull;
    }

    if (s == "NavigationFailed")
    {
        return TaskFailureReason::NavigationFailed;
    }

    if (s == "CombatTooDifficult")
    {
        return TaskFailureReason::CombatTooDifficult;
    }

    if (s == "QuestStateMismatch")
    {
        return TaskFailureReason::QuestStateMismatch;
    }

    if (s == "PreconditionsNotMet")
    {
        return TaskFailureReason::PreconditionsNotMet;
    }

    if (s == "InvalidPlan")
    {
        return TaskFailureReason::InvalidPlan;
    }

    return TaskFailureReason::None;
}

} // namespace og
