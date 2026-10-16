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
 *  File: ObjectiveStack.h
 *
 *  Desc:
 *  Declarations and interface for ObjectiveStack (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/EventBus.h"
#include "og/common/Log.h"
#include "og/common/Vec3.h"
#include "og/state/Types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

struct Objective
{
    std::string kind; // "GrindRoute", "LootCorpse", "CorpseRun", "GatherNode", ...
    Guid guid = 0;    // subject of the objective, if any
    std::optional<Vec3> position;

    std::string describe() const;
};

// Main objective at the bottom, temporary objectives pushed on top and popped
// when done, so the bot naturally resumes what it was doing:
//
//   GrindRoute                -> push LootCorpse -> pop -> GrindRoute
class ObjectiveStack
{
public:
    ObjectiveStack(Logger& log, EventBus* events = nullptr) : log_(log), events_(events)
    {
    }

    void setMain(Objective objective);
    void push(Objective objective);
    // Pops the top objective if it has the given kind (any kind if empty).
    bool pop(std::string_view kind = {});
    // Removes every objective of `kind` (e.g. when a behavior is interrupted).
    void removeAll(std::string_view kind);

    const Objective* top() const
    {
        return items_.empty() ? nullptr : &items_.back();
    }

    bool isTop(std::string_view kind, Guid guid = 0) const;
    bool contains(std::string_view kind) const;
    bool empty() const
    {
        return items_.empty();
    }

    std::size_t size() const
    {
        return items_.size();
    }

    const std::vector<Objective>& items() const
    {
        return items_;
    }

    std::string describe() const;

private:
    void changed();

    Logger& log_;
    EventBus* events_;
    std::vector<Objective> items_;
};

} // namespace og
