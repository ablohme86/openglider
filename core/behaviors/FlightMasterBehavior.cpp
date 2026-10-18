/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-17
 *  File: FlightMasterBehavior.cpp
 *
 *  Desc:
 *  Learn Flight Paths: a flight master in sight with the green "!" over
 *  its head talked to, which learns its flight path.
 *
 */
#include "og/core/BotContext.h"
#include "og/core/TaxiTrip.h"
#include "og/core/behaviors/Behaviors.h"
#include <format>

namespace og
{

namespace
{
// A flight master this close, its path not learnt: worth the steps - as far
// as the bot looks for anything (the scan range).
constexpr float kNear = 250.0f;
// Walking there and talking: a flight master in sight is two minutes off.
const Duration kTimeout{120000};
// After a try that came to nothing, that one not again for this long.
const Duration kRetryAfter{600000};
// Between talking and the flight map, and between one try and the next.
const Duration kAnswer{2500};
constexpr int kTalks = 4;
constexpr float kCloseIn = 1.5f;

const UnitState* flightMaster(const GameSnapshot& s, Guid guid)
{
    for (const auto& u : s.units)
    {
        if (u.guid == guid)
        {
            return &u;
        }
    }

    return nullptr;
}

bool clickAt(BotContext& ctx, const UiFrame& frame)
{
    const auto size = ctx.actions.windowSize();

    if (!size || !ctx.actions.mouseFree())
    {
        return false;
    }

    ctx.actions.mouseMove(static_cast<int>(frame.centerX() * static_cast<float>(size->first) + 0.5f), static_cast<int>(frame.centerY() * static_cast<float>(size->second) + 0.5f));
    ctx.actions.click(MouseButton::Left);
    return true;
}

// The flight master's gossip, where it has one: the line that shows the
// flight map ("Show me where I can fly.").
constexpr std::string_view kChooseFlight = "if C_GossipInfo and C_GossipInfo.GetOptions then "
                                           "for i,v in ipairs(C_GossipInfo.GetOptions()) do "
                                           "local t=string.lower(v.name or '') local k=string.lower(v.type or '') "
                                           "if k=='taxi' or string.find(t,'fly') or string.find(t,'flight') then "
                                           "if C_GossipInfo.SelectOptionByIndex then C_GossipInfo.SelectOptionByIndex(i) "
                                           "else C_GossipInfo.SelectOption(v.gossipOptionID) end break end end "
                                           "elseif GetGossipOptions then "
                                           "local o={GetGossipOptions()} "
                                           "for i=1,#o,2 do "
                                           "if string.lower(o[i+1] or '')=='taxi' then SelectGossipOption(math.floor((i+1)/2)) break end end end";
constexpr std::string_view kCloseMap = "if CloseTaxiMap then CloseTaxiMap() end; if C_GossipInfo and C_GossipInfo.CloseGossip then C_GossipInfo.CloseGossip() elseif CloseGossip then CloseGossip() end";
} // namespace

bool FlightMasterBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.config.learnFlightPaths || s.isDead() || ctx.partyFollower() || s.player.isOnTaxi)
    {
        npc_ = 0;
        return false;
    }

    if (npc_)
    {
        return true;
    }

    if (s.inCombat() || !s.attackers().empty())
    {
        return false;
    }

    const TimePoint now = ctx.now();

    for (const auto& u : s.units)
    {
        if (u.isFlightMaster && u.newFlightPath && u.isAlive() && !u.isPlayer && distance(s.player.position, u.position) <= kNear)
        {
            const auto it = notBefore_.find(u.guid);

            if (it == notBefore_.end() || now >= it->second)
            {
                return true;
            }
        }
    }

    return false;
}

void FlightMasterBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    if (npc_)
    {
        // Back from a fight: talked to again.
        talkedAt_ = TimePoint{};
        gossipChosen_ = false;
        return;
    }

    const UnitState* best = nullptr;
    const TimePoint now = ctx.now();

    for (const auto& u : s.units)
    {
        const auto it = notBefore_.find(u.guid);

        if (u.isFlightMaster && u.newFlightPath && u.isAlive() && !u.isPlayer && distance(s.player.position, u.position) <= kNear && (it == notBefore_.end() || now >= it->second) &&
            (!best || distance(s.player.position, u.position) < distance(s.player.position, best->position)))
        {
            best = &u;
        }
    }

    if (!best)
    {
        return;
    }

    npc_ = best->guid;
    npcName_ = best->name;
    started_ = now;
    talkedAt_ = TimePoint{};
    talks_ = rightClicks_ = 0;
    gossipChosen_ = mouseTried_ = closeIn_ = false;
    ctx.log.info(logcat::Profile, "learn flight path: {} has a flight path not learnt ({:.0f} yd) - going to talk to them", npcName_, distance(s.player.position, best->position));
    ctx.objectives.push(Objective{"Learn Flight Path", 0, best->position});
    ctx.combat.defendOnly = true;
}

void FlightMasterBehavior::onExit(BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.cancelInteraction();
    ctx.stopMouseSeek();
    ctx.restoreCamera();
    ctx.combat.defendOnly = false;
}

BehaviorStatus FlightMasterBehavior::finish(BotContext& ctx, const std::string& reason, bool failed)
{
    ctx.navigator.stop();
    ctx.movement.stop();
    ctx.objectives.pop("Learn Flight Path");
    ctx.combat.defendOnly = false;
    ctx.stopMouseSeek();
    ctx.restoreCamera();

    // The flight map, or a gossip, that talking opened: shut.
    if (ctx.provider && ctx.provider->supportsLua())
    {
        ctx.runScript(kCloseMap);
    }
    else
    {
        for (const auto* close : {"TaxiFrameCloseButton", "FlightMapFrameCloseButton", "GossipFrameCloseButton"})
        {
            if (ctx.uiFrameVisible(close))
            {
                ctx.clickUiFrame(close);
            }
        }
    }

    if (failed)
    {
        notBefore_[npc_] = ctx.now() + kRetryAfter;
        ctx.log.warn(logcat::Profile, "learn flight path: {} at {} - trying again in {:.0f} minutes", reason, npcName_, toSeconds(kRetryAfter) / 60.0);
    }
    else
    {
        ctx.log.info(logcat::Profile, "learn flight path: {} at {}", reason, npcName_);
    }

    npc_ = 0;
    return failed ? BehaviorStatus::Failure : BehaviorStatus::Success;
}

BehaviorStatus FlightMasterBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (!npc_)
    {
        onEnter(s, ctx);

        if (!npc_)
        {
            return BehaviorStatus::Success;
        }
    }

    const TimePoint now = ctx.now();
    const UnitState* npc = flightMaster(s, npc_);

    if (!npc)
    {
        return finish(ctx, "out of sight", true);
    }

    // The "!" gone, or the flight map up: learnt.
    if (!npc->newFlightPath)
    {
        return finish(ctx, "flight path learnt", false);
    }

    if (ctx.uiFrameVisible("TaxiFrame") || ctx.uiFrameVisible("FlightMapFrame"))
    {
        return finish(ctx, "flight map opened, the flight path learnt", false);
    }

    if (now - started_ > kTimeout)
    {
        return finish(ctx, "it took too long", true);
    }

    if (s.player.isCasting || ctx.actions.isCasting())
    {
        ctx.navigator.stop();
        return BehaviorStatus::Running;
    }

    const float reach = closeIn_ ? kCloseIn : ctx.humanReach(ctx.config.interactDistance, npc->guid);

    if (distance(s.player.position, npc->position) > reach)
    {
        if (ctx.navigator.moveTo(s, npc->position, reach - 0.5f, true) == NavStatus::Unreachable)
        {
            return finish(ctx, "could not be reached", true);
        }

        return BehaviorStatus::Running;
    }

    ctx.navigator.stop();
    ctx.movement.stop();
    const bool lua = ctx.provider && ctx.provider->supportsLua();

    // A gossip first, where it has one: the flight line chosen once.
    if (ctx.uiFrameVisible("GossipFrame") && !gossipChosen_)
    {
        if (lua)
        {
            ctx.runScript(kChooseFlight);
            gossipChosen_ = true;
            talkedAt_ = now;
            return BehaviorStatus::Running;
        }

        if (ctx.provider)
        {
            if (const auto known = flightGossipLines(*ctx.provider); known.size() == 1 && clickAt(ctx, known.front()))
            {
                gossipChosen_ = true;
                talkedAt_ = now;
                return BehaviorStatus::Running;
            }
        }
    }

    // Talked to, and the "!" still there: once more - after the first, from
    // right up to it.
    if (talkedAt_ != TimePoint{} && now - talkedAt_ < kAnswer)
    {
        return BehaviorStatus::Running;
    }

    if (talks_ >= kTalks)
    {
        return finish(ctx, "talking learnt nothing", true);
    }

    if (talks_ >= 1 && !closeIn_ && distance(s.player.position, npc->position) > kCloseIn + 0.5f)
    {
        closeIn_ = true;
        return BehaviorStatus::Running;
    }

    if (!ctx.movement.face(s.player, npc->position, 0.4f))
    {
        return BehaviorStatus::Running;
    }

    if (lua)
    {
        ctx.interactNpc(npc->guid, npc->position);
    }
    else
    {
        // No Lua (1.60.1): a right-click on it under the mouse - which learns
        // the flight path; where the mouse finds nothing, the interact key.
        if (!mouseTried_ && ctx.zoomInForClick(rightClicks_))
        {
            return BehaviorStatus::Running;
        }

        const auto seek = mouseTried_ ? BotContext::MouseSeek::Unsupported : ctx.seekUnderMouse(npc_, s.player, npc->position, 0.9f);

        if (seek == BotContext::MouseSeek::Searching)
        {
            return BehaviorStatus::Running;
        }

        if (seek == BotContext::MouseSeek::Found)
        {
            ctx.actions.click(MouseButton::Right);
            ctx.stopMouseSeek();
            ++rightClicks_;
        }
        else
        {
            if (seek == BotContext::MouseSeek::NotFound)
            {
                ctx.stopMouseSeek();
            }

            mouseTried_ = true;

            if (ctx.actions.execute(actions::Interact) != ActionResult::Executed)
            {
                return BehaviorStatus::Running;
            }
        }
    }

    ++talks_;
    talkedAt_ = now;
    gossipChosen_ = false;
    ctx.log.info(logcat::Profile, "learn flight path: talking to {}", npcName_);
    return BehaviorStatus::Running;
}

} // namespace og
