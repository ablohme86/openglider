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
 *  File: IBotBehavior.h
 *
 *  Desc:
 *  Declarations and interface for IBotBehavior (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/core/BotContext.h"
#include "og/state/GameSnapshot.h"

#include <string_view>

namespace og
{

enum class BehaviorStatus
{
    Running,
    Success,
    Failure
};

// A behavior is a self-contained piece of the bot (Combat, Loot, Rest, ...).
// Every tick the engine runs the highest-priority behavior whose canRun()
// returns true; behaviors keep their own sub-state between ticks.
//
// Default priorities:
//   1000 Emergency   900 Death   800 Combat   700 Loot
//    650 Gather      600 Rest    200 Pull    100 Navigation
class IBotBehavior
{
public:
    virtual ~IBotBehavior() = default;

    virtual std::string_view name() const = 0;
    virtual int priority() const = 0;

    virtual bool canRun(const GameSnapshot& snapshot, BotContext& ctx) = 0;
    virtual BehaviorStatus tick(const GameSnapshot& snapshot, BotContext& ctx) = 0;

    // Called when the behavior becomes / stops being the active one.
    virtual void onEnter(const GameSnapshot& snapshot, BotContext& ctx)
    {
    }

    virtual void onExit(BotContext& ctx)
    {
    }

    // A moving activity can expose its next destination before taking over.
    virtual std::optional<Vec3> approachDestination(const GameSnapshot&, BotContext&) const
    {
        return std::nullopt;
    }

    virtual void onYield(BotContext& ctx, std::string_view next)
    {
        onExit(ctx);
    }
};

namespace priority
{
// Up for air before the breath runs out, whatever else is going on.
inline constexpr int Surface = 1050;
// Out of a campfire the bot has come to stand in, resting or fighting.
inline constexpr int LeaveFire = 1040;
inline constexpr int Emergency = 1000;
// Party system: a healer heals a party member before it fights on.
inline constexpr int PartyHeal = 950;
inline constexpr int Death = 900;
inline constexpr int Combat = 800;
inline constexpr int ResurrectEscape = 780;
inline constexpr int Hold = 750;
inline constexpr int Loot = 700;
// Out of where a mob comes back at once, once its loot is taken (FastRespawn).
inline constexpr int LeaveRespawn = 690;
inline constexpr int Gather = 650;
// Sit down at campfires (WoW Forever): a minute by one for Camp Benefits.
inline constexpr int Campfire = 610;
inline constexpr int Rest = 600;
// Party system: a follower walks after its leader; the leader waits for a
// member behind. Above everything that walks somewhere on the bot's own.
inline constexpr int PartyFollow = 590;
inline constexpr int PartyWait = 580;
inline constexpr int ClearPath = 560;
inline constexpr int Vendor = 500;
inline constexpr int InventoryQuest = 470;
// Learn a flight path: a flight master with the green "!" close by.
inline constexpr int LearnFlightPath = 455;
inline constexpr int QuestHandIn = 450;
// Train Abilities: to the class trainer after an even level, finished
// quests handed in first.
inline constexpr int Train = 440;
// Auto Equip: new gear put on, once nothing above it has the bot busy.
inline constexpr int AutoEquip = 430;
inline constexpr int Fish = 300;
inline constexpr int Pull = 200;
inline constexpr int Navigation = 100;
} // namespace priority

} // namespace og
