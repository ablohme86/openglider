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
 *  File: TacticalPlanner.h
 *
 *  Desc:
 *  Declarations and interface for TacticalPlanner (strategic and
 *  tactical AI planning systems).
 *
 *
 */
#pragma once

#include "og/ai/PlannerTypes.h"
#include "og/tasks/Plan.h"
#include "og/tasks/TaskTypes.h"
#include "og/world/WorldModel.h"

namespace og
{

class ITacticalPlanner
{
public:
    virtual ~ITacticalPlanner() = default;

    virtual PlanAdjustment reevaluate(const WorldModel& world, const Plan& currentPlan, const TaskResult& lastResult) = 0;
};

class TacticalPlanner final : public ITacticalPlanner
{
public:
    TacticalPlanner() = default;

    PlanAdjustment reevaluate(const WorldModel& world, const Plan& currentPlan, const TaskResult& lastResult) override;
};

} // namespace og
