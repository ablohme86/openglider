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
 *  File: PlanningBehavior.cpp
 *
 *  Desc:
 *  Implementation of PlanningBehavior (strategic and tactical AI
 *  planning systems).
 *
 *
 */
#include "og/ai/PlanningBehavior.h"

namespace og
{

BehaviorStatus PlanningBehavior::tick(const GameSnapshot& snapshot, BotContext& ctx)
{
    controller_.tick(snapshot, ctx);
    const auto* step = controller_.executor().currentStep();
    IBotBehavior* next = nullptr;

    if (controller_.running() && controller_.executor().hasActivePlan() && !controller_.executor().isSuspended() && step && step->type == PlanStepType::Grind)
    {
        if (pull_.canRun(snapshot, ctx))
        {
            next = &pull_;
        }
        else if (navigation_.canRun(snapshot, ctx))
        {
            next = &navigation_;
        }
    }

    if (next != active_)
    {
        if (active_)
        {
            active_->onExit(ctx);
        }

        active_ = next;

        if (active_)
        {
            active_->onEnter(snapshot, ctx);
        }
    }

    if (active_)
    {
        active_->tick(snapshot, ctx);
    }
    else if (!controller_.running() || !step || step->type == PlanStepType::Wait || controller_.executor().isSuspended())
    {
        ctx.navigator.stop();
    }

    return BehaviorStatus::Running;
}

void PlanningBehavior::onExit(BotContext& ctx)
{
    if (active_)
    {
        active_->onExit(ctx);
    }

    active_ = nullptr;
    controller_.suspend(ctx);
}

} // namespace og
