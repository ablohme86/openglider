/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-04
 *  File: PartyWatch.cpp
 *
 *  Desc:
 *  Implementation of PartyWatch (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/PartyWatch.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace og
{

namespace
{

bool sameName(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

} // namespace

void PartyWatch::update(const GameSnapshot& s, const PartyConfig& config, TimePoint now)
{
    if (!config.enabled)
    {
        members_.clear();
        return;
    }

    // The names configured, the leader among them for a follower; each once.
    std::vector<std::pair<std::string, bool>> names;
    const auto add = [&](const std::string& name, bool leader)
    {
        if (name.empty() || sameName(name, s.player.name))
        {
            return;
        }

        for (auto& [known, isLeader] : names)
        {
            if (sameName(known, name))
            {
                isLeader = isLeader || leader;
                return;
            }
        }

        names.emplace_back(name, leader);
    };

    if (!config.leader)
    {
        add(config.leaderName, true);
    }

    for (const auto& name : config.members)
    {
        add(name, false);
    }

    std::vector<PartyMember> next;
    next.reserve(names.size());

    for (const auto& [name, leader] : names)
    {
        PartyMember member;

        // What was known of it: where it was last seen.
        if (const auto was = std::find_if(members_.begin(), members_.end(), [&](const PartyMember& m) { return sameName(m.name, name); }); was != members_.end())
        {
            member = *was;
        }

        member.name = name;
        member.leader = leader;
        member.inSight = false;
        member.guid = 0;

        for (const UnitState& u : s.units)
        {
            if (!u.isPlayer || !sameName(u.name, name))
            {
                continue;
            }

            member.inSight = true;
            member.guid = u.guid;
            member.position = u.position;
            member.seenAt = now;
            member.health = u.healthPercent();
            member.power = u.powerPercent();
            member.dead = !u.isAlive();
            member.resting = u.isSitting && (u.hasBuff("Food") || u.hasBuff("Drink"));
            member.inCombat = u.inCombat;
            member.target = u.targetGuid;
            break;
        }

        if (member.seenAt != TimePoint{})
        {
            member.distance = distance2D(s.player.position, member.position);
        }

        next.push_back(std::move(member));
    }

    members_ = std::move(next);
}

const PartyMember* PartyWatch::named(std::string_view name) const
{
    const auto it = std::find_if(members_.begin(), members_.end(), [&](const PartyMember& m) { return sameName(m.name, name); });
    return it == members_.end() ? nullptr : &*it;
}

const PartyMember* PartyWatch::leader() const
{
    const auto it = std::find_if(members_.begin(), members_.end(), [](const PartyMember& m) { return m.leader; });
    return it == members_.end() ? nullptr : &*it;
}

std::optional<PartyWatch::Wait> PartyWatch::waitFor(float partyRange) const
{
    for (const PartyMember& m : members_)
    {
        if (!m.inSight)
        {
            return Wait{&m, m.seenAt == TimePoint{} ? std::string("not seen yet") : std::format("out of sight, last seen {:.0f} yd off", m.distance)};
        }

        if (m.dead)
        {
            return Wait{&m, "dead"};
        }

        if (m.distance > partyRange)
        {
            return Wait{&m, std::format("{:.0f} yd behind", m.distance)};
        }

        if (m.resting)
        {
            return Wait{&m, "resting"};
        }
    }

    return std::nullopt;
}

} // namespace og
