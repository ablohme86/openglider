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
 *  File: CombatTracker.h
 *
 *  Desc:
 *  Declarations and interface for CombatTracker (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/core/BotContext.h"

#include <set>

namespace og
{

// Runs every tick before behavior selection. Keeps shared combat state up to
// date independently of which behavior is active:
//   - remembers every unit we fought and detects when it dies (kill stats,
//     TargetKilledEvent, pending loot)
//   - tracks fight start/end
//   - drops an engaged target that vanished or got blacklisted
//   - prunes pending loot that can no longer be looted
class CombatTracker
{
public:
    void update(const GameSnapshot& snapshot, BotContext& ctx);
    void reset()
    {
        fought_.clear();
    }

    const std::set<Guid>& fought() const
    {
        return fought_;
    }

private:
    std::set<Guid> fought_;
};

} // namespace og
