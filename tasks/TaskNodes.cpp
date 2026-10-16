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
 *  File: TaskNodes.cpp
 *
 *  Desc:
 *  Implementation of TaskNodes (task graph definitions, execution, and
 *  validation).
 *
 *
 */
#include "og/tasks/TaskNodes.h"

#include "og/common/MathUtil.h"

#include <format>

namespace og
{

// GoToTaskNode
void GoToTaskNode::onStart(const PlanStep& step, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
    ctx.objectives.push(Objective{"GoTo", 0, step.position});
}

TaskResult GoToTaskNode::tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx)
{
    const float d = distance2D(s.player.position, step.position);

    if (d <= step.arrivalDistance)
    {
        ctx.navigator.stop();
        ctx.objectives.pop("GoTo");
        return TaskResult::success(std::format("Arrived at destination (dist={:.1f})", d), {{"travelTime", toSeconds(ctx.now() - startTime_)}, {"distance", d}});
    }

    if (step.timeout.count() > 0 && ctx.now() - startTime_ > step.timeout)
    {
        ctx.navigator.stop();
        ctx.objectives.pop("GoTo");
        return TaskResult::failed(TaskFailureReason::Timeout, "GoTo timeout exceeded");
    }

    const NavStatus st = ctx.navigator.moveTo(s, step.position, step.arrivalDistance);

    if (st == NavStatus::Unreachable)
    {
        ctx.navigator.stop();
        ctx.objectives.pop("GoTo");
        return TaskResult::failed(TaskFailureReason::Unreachable, "Destination unreachable");
    }

    return TaskResult::inProgress();
}

void GoToTaskNode::onCancel(const PlanStep&, BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.objectives.pop("GoTo");
}

// WaitTaskNode
void WaitTaskNode::onStart(const PlanStep&, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
}

TaskResult WaitTaskNode::tick(const PlanStep& step, const GameSnapshot&, BotContext& ctx)
{
    if (ctx.now() - startTime_ >= step.waitDuration)
    {
        return TaskResult::success("Wait completed");
    }

    return TaskResult::inProgress();
}

void WaitTaskNode::onCancel(const PlanStep&, BotContext&)
{
}

// GrindTaskNode
void GrindTaskNode::onStart(const PlanStep&, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
    startKills_ = ctx.stats.kills;
    ctx.objectives.push(Objective{"Grind"});
}

TaskResult GrindTaskNode::tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx)
{
    if (step.targetLevel > 0 && s.player.level >= step.targetLevel)
    {
        ctx.objectives.pop("Grind");
        return TaskResult::success("Target level reached", {{"kills", static_cast<double>(ctx.stats.kills - startKills_)}});
    }

    if (step.timeout.count() > 0 && ctx.now() - startTime_ > step.timeout)
    {
        ctx.objectives.pop("Grind");
        return TaskResult::success("Grind duration completed", {{"kills", static_cast<double>(ctx.stats.kills - startKills_)}});
    }

    return TaskResult::inProgress();
}

void GrindTaskNode::onCancel(const PlanStep&, BotContext& ctx)
{
    ctx.objectives.pop("Grind");
}

// QuestObjectiveTaskNode
void QuestObjectiveTaskNode::onStart(const PlanStep& step, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
    ctx.objectives.push(Objective{"QuestObjective", step.targetGuid, step.position});
}

TaskResult QuestObjectiveTaskNode::tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx)
{
    if (step.requiredCount > 0 && step.currentCount >= step.requiredCount)
    {
        ctx.objectives.pop("QuestObjective");
        ctx.navigator.stop();
        return TaskResult::success("Quest objective completed", {{"currentCount", step.currentCount}});
    }

    if (step.timeout.count() > 0 && ctx.now() - startTime_ > step.timeout)
    {
        ctx.navigator.stop();
        ctx.objectives.pop("QuestObjective");
        return TaskResult::failed(TaskFailureReason::Timeout, "Quest objective timeout", {{"currentCount", step.currentCount}});
    }

    if (step.position.x != 0.0f || step.position.y != 0.0f)
    {
        const float d = distance2D(s.player.position, step.position);

        if (d > 45.0f)
        {
            ctx.navigator.moveTo(s, step.position, 35.0f);
            return TaskResult::inProgress("Moving to quest area");
        }
    }

    return TaskResult::inProgress("Executing quest objective");
}

void QuestObjectiveTaskNode::onCancel(const PlanStep&, BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.objectives.pop("QuestObjective");
}

// InteractTaskNode
void InteractTaskNode::onStart(const PlanStep&, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
}

TaskResult InteractTaskNode::tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx)
{
    if (step.targetGuid != 0)
    {
        const UnitState* u = s.findUnit(step.targetGuid);

        if (u)
        {
            const float d = distance(s.player.position, u->position);

            if (d <= 5.0f)
            {
                ctx.interactUnit(step.targetGuid, u->position);
                return TaskResult::success("Interacted with unit");
            }

            ctx.navigator.moveTo(s, u->position, 4.0f);
            return TaskResult::inProgress("Approaching interact unit");
        }

        if (step.timeout.count() > 0 && ctx.now() - startTime_ > step.timeout)
        {
            return TaskResult::failed(TaskFailureReason::MissingNpc, "Target unit not found");
        }

        return TaskResult::inProgress("Searching for target unit");
    }

    if (step.position.x != 0.0f || step.position.y != 0.0f)
    {
        const float d = distance2D(s.player.position, step.position);

        if (d <= 4.0f)
        {
            return TaskResult::success("Arrived at interaction point");
        }

        ctx.navigator.moveTo(s, step.position, 3.5f);
        return TaskResult::inProgress("Moving to interaction point");
    }

    return TaskResult::failed(TaskFailureReason::MissingObject, "No target or position specified");
}

void InteractTaskNode::onCancel(const PlanStep&, BotContext& ctx)
{
    ctx.navigator.stop();
}

// VendorTaskNode
void VendorTaskNode::onStart(const PlanStep& step, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
    ctx.objectives.push(Objective{"Vendor", step.targetGuid, step.position});
}

TaskResult VendorTaskNode::tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx)
{
    if (step.position.x != 0.0f || step.position.y != 0.0f)
    {
        const float d = distance2D(s.player.position, step.position);

        if (d > 4.5f)
        {
            ctx.navigator.moveTo(s, step.position, 4.0f);
            return TaskResult::inProgress("Approaching vendor");
        }
    }

    ctx.navigator.stop();

    if (step.targetGuid != 0)
    {
        const UnitState* u = s.findUnit(step.targetGuid);

        if (u)
        {
            ctx.interactUnit(step.targetGuid, u->position);
        }
    }

    if (ctx.now() - startTime_ > Duration(10000))
    {
        ctx.objectives.pop("Vendor");
        return TaskResult::failed(TaskFailureReason::PreconditionsNotMet, "Vendor transaction verification is not implemented");
    }

    return TaskResult::inProgress("Vendoring items");
}

void VendorTaskNode::onCancel(const PlanStep&, BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.objectives.pop("Vendor");
}

// RepairTaskNode
void RepairTaskNode::onStart(const PlanStep& step, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
    ctx.objectives.push(Objective{"Repair", step.targetGuid, step.position});
}

TaskResult RepairTaskNode::tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx)
{
    if (step.position.x != 0.0f || step.position.y != 0.0f)
    {
        const float d = distance2D(s.player.position, step.position);

        if (d > 4.5f)
        {
            ctx.navigator.moveTo(s, step.position, 4.0f);
            return TaskResult::inProgress("Approaching repair NPC");
        }
    }

    ctx.navigator.stop();

    if (step.targetGuid != 0)
    {
        const UnitState* u = s.findUnit(step.targetGuid);

        if (u)
        {
            ctx.interactUnit(step.targetGuid, u->position);
        }
    }

    if (ctx.now() - startTime_ > Duration(5000))
    {
        ctx.objectives.pop("Repair");
        return TaskResult::failed(TaskFailureReason::PreconditionsNotMet, "Repair transaction verification is not implemented");
    }

    return TaskResult::inProgress("Repairing equipment");
}

void RepairTaskNode::onCancel(const PlanStep&, BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.objectives.pop("Repair");
}

// GenericQuestTaskNode
void GenericQuestTaskNode::onStart(const PlanStep& step, const GameSnapshot&, BotContext& ctx)
{
    startTime_ = ctx.now();
    ctx.objectives.push(Objective{name_, step.targetGuid, step.position});
}

TaskResult GenericQuestTaskNode::tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx)
{
    if (step.position.x != 0.0f || step.position.y != 0.0f)
    {
        const float d = distance2D(s.player.position, step.position);

        if (d > 4.5f)
        {
            ctx.navigator.moveTo(s, step.position, 4.0f);
            return TaskResult::inProgress("Approaching target");
        }
    }

    ctx.navigator.stop();

    if (step.targetGuid != 0)
    {
        const UnitState* u = s.findUnit(step.targetGuid);

        if (u)
        {
            ctx.interactUnit(step.targetGuid, u->position);
        }
    }

    if (ctx.now() - startTime_ > Duration(5000))
    {
        ctx.objectives.pop(name_);
        return TaskResult::failed(TaskFailureReason::PreconditionsNotMet, std::format("{} completion verification is not implemented", name_));
    }

    return TaskResult::inProgress();
}

void GenericQuestTaskNode::onCancel(const PlanStep&, BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.objectives.pop(name_);
}

// TaskRegistry
TaskRegistry::TaskRegistry() = default;

void TaskRegistry::registerNode(std::unique_ptr<ITaskNode> node)
{
    nodes_[node->type()] = std::move(node);
}

ITaskNode* TaskRegistry::find(PlanStepType type) const
{
    auto it = nodes_.find(type);
    return it != nodes_.end() ? it->second.get() : nullptr;
}

TaskRegistry TaskRegistry::createDefault()
{
    TaskRegistry r;
    r.registerNode(std::make_unique<GoToTaskNode>());
    r.registerNode(std::make_unique<WaitTaskNode>());
    r.registerNode(std::make_unique<GrindTaskNode>());
    r.registerNode(std::make_unique<QuestObjectiveTaskNode>());
    r.registerNode(std::make_unique<InteractTaskNode>());
    r.registerNode(std::make_unique<VendorTaskNode>());
    r.registerNode(std::make_unique<RepairTaskNode>());
    r.registerNode(std::make_unique<GenericQuestTaskNode>(PlanStepType::AcceptQuest, "AcceptQuest"));
    r.registerNode(std::make_unique<GenericQuestTaskNode>(PlanStepType::TurnInQuest, "TurnInQuest"));
    r.registerNode(std::make_unique<GenericQuestTaskNode>(PlanStepType::Train, "Train"));
    r.registerNode(std::make_unique<GenericQuestTaskNode>(PlanStepType::UseFlightPath, "UseFlightPath"));
    r.registerNode(std::make_unique<GenericQuestTaskNode>(PlanStepType::GatherMining, "GatherMining"));
    r.registerNode(std::make_unique<GenericQuestTaskNode>(PlanStepType::GatherHerbalism, "GatherHerbalism"));
    r.registerNode(std::make_unique<GenericQuestTaskNode>(PlanStepType::Fish, "Fish"));
    return r;
}

} // namespace og
