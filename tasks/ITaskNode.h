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
 *  File: ITaskNode.h
 *
 *  Desc:
 *  Declarations and interface for ITaskNode (task graph definitions,
 *  execution, and validation).
 *
 *
 */
#pragma once

#include "og/core/BotContext.h"
#include "og/state/GameSnapshot.h"
#include "og/tasks/PlanStep.h"
#include "og/tasks/TaskTypes.h"

#include <string_view>

namespace og
{

class ITaskNode
{
public:
    virtual ~ITaskNode() = default;

    virtual PlanStepType type() const = 0;
    virtual std::string_view name() const = 0;

    virtual void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) = 0;
    virtual TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) = 0;
    virtual void onCancel(const PlanStep& step, BotContext& ctx) = 0;
};

} // namespace og
