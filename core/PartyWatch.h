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
 *  File: PartyWatch.h
 *
 *  Desc:
 *  Declarations and interface for PartyWatch (bot engine, state
 *  machines, and automated behaviors).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/core/BotConfig.h"
#include "og/state/GameSnapshot.h"

#include <optional>
#include <string>
#include <vector>

namespace og
{

// A party member, as the client shows it: in sight (in the object list,
// about 250 yards round us), or where it was last seen.
struct PartyMember
{
    std::string name;
    bool leader = false;
    bool inSight = false;
    Guid guid = 0;    // while in sight
    Vec3 position;    // last seen
    TimePoint seenAt; // when last seen; TimePoint{} never
    float distance = 0; // from us, on the map (2D), when last seen
    float health = 0;   // percent
    float power = 0;    // percent of its mana, rage or energy
    bool dead = false;
    bool resting = false; // eating or drinking
    bool inCombat = false;
    Guid target = 0;
};

// Where the party members (PartyConfig) are, from the players in the
// snapshot: what the radar draws. Updated every tick before the behaviors.
class PartyWatch
{
public:
    void update(const GameSnapshot& s, const PartyConfig& config, TimePoint now);

    const std::vector<PartyMember>& members() const
    {
        return members_;
    }

    // A follower's leader; nullptr for the leader itself, or with no party.
    const PartyMember* leader() const;
    // The member of that name (any case), or nullptr: nobody else is listened to.
    const PartyMember* named(std::string_view name) const;

    // The first member the leader waits for (out of sight, beyond the party
    // range, dead or resting), and why; nothing when all are there.
    struct Wait
    {
        const PartyMember* member = nullptr;
        std::string reason;
    };
    std::optional<Wait> waitFor(float partyRange) const;

private:
    std::vector<PartyMember> members_;
};

} // namespace og
