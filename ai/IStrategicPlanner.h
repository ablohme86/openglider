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
 *  File: IStrategicPlanner.h
 *
 *  Desc:
 *  Declarations and interface for IStrategicPlanner (strategic and
 *  tactical AI planning systems).
 *
 *
 */
#pragma once

#include "og/ai/PlannerTypes.h"
#include "og/tasks/Plan.h"
#include "og/world/WorldModel.h"

namespace og
{

class IStrategicPlanner
{
public:
    virtual ~IStrategicPlanner() = default;

    virtual Plan createPlan(const WorldModel& world, const BotGoal& goal) = 0;
};

} // namespace og
