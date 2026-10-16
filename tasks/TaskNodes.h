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
 *  File: TaskNodes.h
 *
 *  Desc:
 *  Declarations and interface for TaskNodes (task graph definitions,
 *  execution, and validation).
 *
 *
 */
#pragma once

#include "og/tasks/ITaskNode.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace og
{

class GoToTaskNode final : public ITaskNode
{
public:
    PlanStepType type() const override
    {
        return PlanStepType::GoTo;
    }

    std::string_view name() const override
    {
        return "GoTo";
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    TimePoint startTime_{};
};

class WaitTaskNode final : public ITaskNode
{
public:
    PlanStepType type() const override
    {
        return PlanStepType::Wait;
    }

    std::string_view name() const override
    {
        return "Wait";
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    TimePoint startTime_{};
};

class GrindTaskNode final : public ITaskNode
{
public:
    PlanStepType type() const override
    {
        return PlanStepType::Grind;
    }

    std::string_view name() const override
    {
        return "Grind";
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    TimePoint startTime_{};
    std::uint32_t startKills_ = 0;
};

class QuestObjectiveTaskNode final : public ITaskNode
{
public:
    PlanStepType type() const override
    {
        return PlanStepType::CompleteQuestObjective;
    }

    std::string_view name() const override
    {
        return "CompleteQuestObjective";
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    TimePoint startTime_{};
};

class InteractTaskNode final : public ITaskNode
{
public:
    PlanStepType type() const override
    {
        return PlanStepType::Interact;
    }

    std::string_view name() const override
    {
        return "Interact";
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    TimePoint startTime_{};
};

class VendorTaskNode final : public ITaskNode
{
public:
    PlanStepType type() const override
    {
        return PlanStepType::Vendor;
    }

    std::string_view name() const override
    {
        return "Vendor";
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    TimePoint startTime_{};
};

class RepairTaskNode final : public ITaskNode
{
public:
    PlanStepType type() const override
    {
        return PlanStepType::Repair;
    }

    std::string_view name() const override
    {
        return "Repair";
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    TimePoint startTime_{};
};

class GenericQuestTaskNode final : public ITaskNode
{
public:
    explicit GenericQuestTaskNode(PlanStepType type, std::string_view name) : type_(type), name_(name)
    {
    }

    PlanStepType type() const override
    {
        return type_;
    }

    std::string_view name() const override
    {
        return name_;
    }

    void onStart(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    TaskResult tick(const PlanStep& step, const GameSnapshot& s, BotContext& ctx) override;
    void onCancel(const PlanStep& step, BotContext& ctx) override;

private:
    PlanStepType type_;
    std::string name_;
    TimePoint startTime_{};
};

class TaskRegistry
{
public:
    TaskRegistry();

    void registerNode(std::unique_ptr<ITaskNode> node);
    ITaskNode* find(PlanStepType type) const;

    static TaskRegistry createDefault();

private:
    std::unordered_map<PlanStepType, std::unique_ptr<ITaskNode>> nodes_;
};

} // namespace og
