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
 *  File: ActionBinding.cpp
 *
 *  Desc:
 *  Implementation of ActionBinding (semantic game actions, action
 *  configuration, and execution).
 *
 *
 */
#include "og/actions/ActionBinding.h"

namespace og
{

void ActionMap::add(ActionBinding binding)
{
    auto name = binding.name;
    bindings_.insert_or_assign(std::move(name), std::move(binding));
}

void ActionMap::addInfo(ActionBinding info)
{
    std::string name = info.name;
    info_.insert_or_assign(std::move(name), std::move(info));
}

const ActionBinding* ActionMap::settingsFor(std::string_view name) const
{
    if (const ActionBinding* bound = find(name))
    {
        return bound;
    }

    const auto it = info_.find(name);
    return it == info_.end() ? nullptr : &it->second;
}

bool ActionMap::isFriendly(std::string_view name) const
{
    if (const ActionBinding* bound = find(name); bound && bound->friendly)
    {
        return true;
    }

    const auto it = info_.find(name);
    return it != info_.end() && it->second.friendly;
}

bool ActionMap::remove(std::string_view name)
{
    auto it = bindings_.find(name);

    if (it == bindings_.end())
    {
        return false;
    }

    bindings_.erase(it);
    return true;
}

const ActionBinding* ActionMap::find(std::string_view name) const
{
    auto it = bindings_.find(name);
    return it == bindings_.end() ? nullptr : &it->second;
}

ActionBinding* ActionMap::find(std::string_view name)
{
    auto it = bindings_.find(name);
    return it == bindings_.end() ? nullptr : &it->second;
}

std::optional<std::string> ActionMap::actionForChord(const KeyChord& chord, std::string_view preferredPrefix) const
{
    if (!preferredPrefix.empty())
    {
        for (const auto& [name, b] : bindings_)
        {
            if (b.chord == chord && (name.starts_with(preferredPrefix) || name.starts_with("Move.") || name.starts_with("Target.") || name.starts_with("Player.")))
            {
                return name;
            }
        }
    }

    for (const auto& [name, b] : bindings_)
    {
        if (b.chord == chord)
        {
            return name;
        }
    }

    return std::nullopt;
}

std::vector<std::string> ActionMap::actionsForKey(Key key) const
{
    std::vector<std::string> out;

    for (const auto& [name, b] : bindings_)
    {
        if (b.chord.key == key)
        {
            out.push_back(name);
        }
    }

    return out;
}

} // namespace og
