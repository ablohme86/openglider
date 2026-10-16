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
 *  File: PlanningBehavior.h
 *
 *  Desc:
 *  Declarations and interface for PlanningBehavior (strategic and
 *  tactical AI planning systems).
 *
 *
 */
#pragma once

#include "og/ai/PlanningController.h"
#include "og/core/behaviors/Behaviors.h"

namespace og
{

// Gates profile movement/pulls behind the current plan. Higher priority reactive
// behaviors still own combat, looting, rest, death and emergencies.
class PlanningBehavior final : public IBotBehavior
{
public:
    explicit PlanningBehavior(PlanningController& controller) : controller_(controller)
    {
    }

    std::string_view name() const override
    {
        return "Planning";
    }

    int priority() const override
    {
        return 300;
    }

    bool canRun(const GameSnapshot&, BotContext&) override
    {
        return true;
    }

    BehaviorStatus tick(const GameSnapshot& snapshot, BotContext& ctx) override;
    void onEnter(const GameSnapshot&, BotContext&) override
    {
        controller_.resume();
    }

    void onExit(BotContext& ctx) override;

private:
    PlanningController& controller_;
    PullBehavior pull_;
    NavigationBehavior navigation_;
    IBotBehavior* active_ = nullptr;
};

} // namespace og
