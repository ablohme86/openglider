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
 *  File: ObjectiveStack.cpp
 *
 *  Desc:
 *  Implementation of ObjectiveStack (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/ObjectiveStack.h"

#include "og/core/Events.h"

#include <algorithm>

namespace og
{

std::string Objective::describe() const
{
    if (guid != 0)
    {
        return kind + "(" + formatGuid(guid) + ")";
    }

    return kind;
}

void ObjectiveStack::setMain(Objective objective)
{
    if (items_.empty())
    {
        items_.push_back(std::move(objective));
    }
    else
    {
        items_.front() = std::move(objective);
    }

    changed();
}

void ObjectiveStack::push(Objective objective)
{
    log_.info(logcat::Objective, "push {}", objective.describe());
    items_.push_back(std::move(objective));
    changed();
}

bool ObjectiveStack::pop(std::string_view kind)
{
    if (items_.size() <= 1 && !items_.empty() && kind.empty())
    {
        return false; // never pop the main objective blindly
    }

    if (items_.empty())
    {
        return false;
    }

    if (!kind.empty() && items_.back().kind != kind)
    {
        return false;
    }

    log_.info(logcat::Objective, "pop {}", items_.back().describe());
    items_.pop_back();
    changed();
    return true;
}

void ObjectiveStack::removeAll(std::string_view kind)
{
    const auto before = items_.size();
    std::erase_if(items_, [&](const Objective& o) { return o.kind == kind; });

    if (items_.size() != before)
    {
        changed();
    }
}

bool ObjectiveStack::isTop(std::string_view kind, Guid guid) const
{
    const Objective* t = top();
    return t && t->kind == kind && (guid == 0 || t->guid == guid);
}

bool ObjectiveStack::contains(std::string_view kind) const
{
    return std::any_of(items_.begin(), items_.end(), [&](const Objective& o) { return o.kind == kind; });
}

std::string ObjectiveStack::describe() const
{
    std::string out;

    for (const auto& o : items_)
    {
        if (!out.empty())
        {
            out += " > ";
        }

        out += o.describe();
    }

    return out.empty() ? "(none)" : out;
}

void ObjectiveStack::changed()
{
    if (events_)
    {
        events_->publish(ObjectiveChangedEvent{describe()});
    }
}

} // namespace og
