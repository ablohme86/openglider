/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-30
 *  File: BuffTracker.h
 *
 *  Desc:
 *  Declarations and interface for BuffTracker (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/state/GameSnapshot.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace og
{

struct BotContext;
class IBotBehavior;

class BuffTracker
{
public:
    void update(const GameSnapshot& snapshot, BotContext& ctx);
    bool tryApplyInstantBuff(const GameSnapshot& snapshot, BotContext& ctx, const IBotBehavior* current);
    bool applyBuff(const GameSnapshot& snapshot, BotContext& ctx);
    bool applyInstantBuff(const GameSnapshot& snapshot, BotContext& ctx);
    void reset();

    bool hasPendingBuff() const
    {
        return pressedBuff_.has_value();
    }

private:
    struct PressedBuff
    {
        std::string action;
        TimePoint settledAt;
        std::set<std::uint32_t> before;
        bool hadPet = false; // a summon (Summon Imp) gives a pet, not a buff
    };
    struct BlockedBuff
    {
        std::string action;
        std::set<std::uint32_t> blockers;
    };
    std::optional<PressedBuff> pressedBuff_;
    std::vector<BlockedBuff> blockedBuffs_;
};

} // namespace og
