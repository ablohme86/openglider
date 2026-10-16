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
 *  File: PlanningController.h
 *
 *  Desc:
 *  Declarations and interface for PlanningController (strategic and
 *  tactical AI planning systems).
 *
 *
 */
#pragma once

#include "og/ai/IStrategicPlanner.h"
#include "og/ai/TacticalPlanner.h"
#include "og/tasks/PlanExecutor.h"
#include "og/world/QuestDatabase.h"

namespace og
{

struct PlanningControllerConfig
{
    Duration tacticalInterval = seconds(15);
    Duration replanCooldown = seconds(10);
    int maxNavigationRetries = 2;
};

struct PlannerTelemetry
{
    std::size_t strategicCalls = 0;
    std::size_t tacticalCalls = 0;
    std::size_t localRetries = 0;
    std::size_t taskResults = 0;
    std::string lastDecision;
};

// Single-threaded coordinator. The owner supplies snapshots and confirmed quest
// knowledge, schedules reactive behaviors, and explicitly resumes interrupted work.
// Synchronous planners should run on the bot worker, never on the Qt UI thread.
class PlanningController
{
public:
    PlanningController(IStrategicPlanner& strategic, ITacticalPlanner& tactical, const QuestDatabase& quests, TaskRegistry registry = TaskRegistry::createDefault(), PlanningControllerConfig config = {});

    void start(BotGoal goal, BotContext& ctx);
    void stop(BotContext& ctx, std::string_view reason = "Stopped by user");
    bool resume()
    {
        return running_ && executor_.resumeStep();
    }

    void suspend(BotContext& ctx);
    TaskResult tick(const GameSnapshot& snapshot, BotContext& ctx);

    bool running() const
    {
        return running_;
    }

    WorldModel& world()
    {
        return world_;
    }

    const WorldModel& world() const
    {
        return world_;
    }

    const PlanExecutor& executor() const
    {
        return executor_;
    }

    const PlannerTelemetry& telemetry() const
    {
        return telemetry_;
    }

private:
    bool goalReached() const;
    bool loadPlan(Plan plan, BotContext& ctx);
    TaskResult reevaluate(const TaskResult& result, BotContext& ctx);
    void requestReplan(std::string reason, BotContext& ctx);
    std::string currentStepError() const;

    IStrategicPlanner& strategic_;
    ITacticalPlanner& tactical_;
    const QuestDatabase& quests_;
    PlanningControllerConfig config_;
    PlanExecutor executor_;
    WorldModel world_;
    WorldModelBuilder builder_;
    WorldModelTaskResultUpdater updater_;
    BotGoal goal_;
    PlannerTelemetry telemetry_;
    bool running_ = false;
    bool observed_ = false;
    int retries_ = 0;
    int lastLevel_ = 0;
    std::uint32_t lastZone_ = 0;
    int lastBagSlots_ = -1;
    TimePoint nextPlanAt_{};
    TimePoint nextTacticalAt_{};
    TimePoint planStarted_{};
    int startDeaths_ = 0;
    int startStuck_ = 0;
    EventBus::Subscription feedback_;
};

} // namespace og
