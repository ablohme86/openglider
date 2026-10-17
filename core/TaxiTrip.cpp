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
 *  File: TaxiTrip.cpp
 *
 *  Desc:
 *  Implementation of TaxiTrip: a flight on the way.
 *
 */
#include "og/core/TaxiTrip.h"

#include "og/core/BotContext.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace og
{

namespace
{

// Where the continent's map has the world (its WorldMapArea): left and right
// the world's y, top and bottom its x. Kalimdor's was matched to the flight
// map of Freewind Post, Thunder Bluff and Camp Taurajo shown.
struct Continent
{
    float left, right, top, bottom;
};

std::optional<Continent> continent(std::uint32_t mapId)
{
    switch (mapId)
    {
    case 0:
        return Continent{18171.97f, -22569.21f, 11176.34f, -15973.34f};
    case 1:
        return Continent{17066.6f, -19733.2f, 12799.9f, -11733.3f};
    default:
        return std::nullopt;
    }
}

constexpr float kNearMaster = 25.0f; // the flight master within this of its flight point
constexpr float kCloseIn = 1.5f;
const Duration kTimeout{240000};    // to the flight master and talking
const Duration kAnswer{2500};       // a talk or a click and what it opens
const Duration kTakeOff{6000};      // a flight point clicked and still on the ground
const Duration kLanded{3000};       // standing this long after flying: landed
constexpr int kTalks = 4;
constexpr int kClicks = 2;

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

} // namespace

std::optional<int> taxiButtonFor(std::uint32_t target, std::uint32_t mapId, const std::vector<std::pair<int, UiFrame>>& buttons, const UiFrame& map, const TaxiNetwork& network, const std::vector<std::uint32_t>& known)
{
    const auto c = continent(mapId);
    const float width = map.right - map.left, height = map.bottom - map.top;

    if (!c || width <= 0 || height <= 0 || buttons.empty())
    {
        return std::nullopt;
    }

    // Where on the flight map a flight point is, as a share of it.
    const auto placed = [&](const TaxiNode& n) { return std::pair{(c->left - n.position.y) / (c->left - c->right), (c->top - n.position.x) / (c->top - c->bottom)}; };
    const auto onMap = [&](const UiFrame& b) { return std::pair{(b.centerX() - map.left) / width, (b.centerY() - map.top) / height}; };
    const auto apart = [](std::pair<float, float> a, std::pair<float, float> b) { return std::hypot(a.first - b.first, a.second - b.second); };
    const auto* wanted = network.node(target);

    if (!wanted || wanted->mapId != mapId)
    {
        return std::nullopt;
    }

    // The button nearest where the flight point is...
    const auto nearest = std::min_element(buttons.begin(), buttons.end(), [&](const auto& a, const auto& b) { return apart(onMap(a.second), placed(*wanted)) < apart(onMap(b.second), placed(*wanted)); });

    // ...is it, if no other flight point learnt is nearer that button.
    for (const auto id : known)
    {
        const auto* other = network.node(id);

        if (other && other->mapId == mapId && id != target && apart(onMap(nearest->second), placed(*other)) < apart(onMap(nearest->second), placed(*wanted)))
        {
            return std::nullopt;
        }
    }

    return apart(onMap(nearest->second), placed(*wanted)) < 0.05f ? std::optional<int>{nearest->first} : std::nullopt;
}

TaxiTrip::Status TaxiTrip::fail(BotContext& ctx, std::string why)
{
    why_ = std::move(why);
    ctx.navigator.stop();
    ctx.movement.stop();
    ctx.stopMouseSeek();
    ctx.restoreCamera();

    for (const auto* close : {"TaxiFrameCloseButton", "GossipFrameCloseButton"})
    {
        if (ctx.uiFrameVisible(close))
        {
            ctx.clickUiFrame(close);
        }
    }

    return Status::Failed;
}

TaxiTrip::Status TaxiTrip::tick(const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();

    if (started_ == TimePoint{})
    {
        started_ = stageAt_ = now;
    }

    const Vec3& pos = s.player.position;

    if (stage_ == Stage::Flying)
    {
        // Up and away: on the taxi, as the client says; where it does not,
        // faster than any run.
        const bool fast = lastPosition_ && distance(*lastPosition_, pos) > 2.5f;
        lastPosition_ = pos;

        if (s.player.isOnTaxi || fast)
        {
            takenOff_ = true;
            stillSince_ = TimePoint{};
            return Status::Running;
        }

        if (takenOff_)
        {
            if (stillSince_ == TimePoint{})
            {
                stillSince_ = now;
            }

            if (s.player.isOnTaxi || now - stillSince_ < kLanded)
            {
                return Status::Running;
            }

            ctx.log.info(logcat::Move, "flight: landed at flight point {} - walking on", plan_.to);
            return Status::Landed;
        }

        if (now - clickedAt_ < kTakeOff)
        {
            return Status::Running;
        }

        if (clicks_ >= kClicks || !ctx.uiFrameVisible("TaxiFrame"))
        {
            return fail(ctx, "did not take off");
        }

        stage_ = Stage::Talking; // the flight point clicked again
    }

    if (now - started_ > kTimeout)
    {
        return fail(ctx, "it took too long");
    }

    // The flight map up: the flight point clicked.
    if (ctx.uiFrameVisible("TaxiFrame"))
    {
        if (now - clickedAt_ < kAnswer)
        {
            return Status::Running;
        }

        const auto map = ctx.provider ? ctx.provider->uiFrame("TaxiRouteMap") : std::nullopt;
        std::vector<std::pair<int, UiFrame>> buttons;

        for (int n = 1; n <= 64 && ctx.provider; ++n)
        {
            const auto b = ctx.provider->uiFrame(std::format("TaxiButton{}", n));

            if (!b)
            {
                break;
            }

            if (b->visible)
            {
                buttons.emplace_back(n, *b);
            }
        }

        const auto known = s.player.knownTaxiNodes.value_or(std::vector<std::uint32_t>{});
        const auto button = map && ctx.network() ? taxiButtonFor(plan_.to, mapId_, buttons, *map, *ctx.network(), known) : std::nullopt;

        if (!button)
        {
            return fail(ctx, std::format("flight point {} not found on the flight map ({} shown)", plan_.to, buttons.size()));
        }

        const auto frame = std::find_if(buttons.begin(), buttons.end(), [&](const auto& b) { return b.first == *button; })->second;

        if (!clickAt(ctx, frame))
        {
            return Status::Running;
        }

        ++clicks_;
        clickedAt_ = now;
        stage_ = Stage::Flying;
        lastPosition_ = pos;
        ctx.log.info(logcat::Move, "flight: flying from flight point {} to {} ({} copper)", plan_.from, plan_.to, plan_.cost);
        return Status::Running;
    }

    // The flight master: by its flight point.
    const UnitState* master = nullptr;

    for (const auto& u : s.units)
    {
        if (u.isFlightMaster && u.isAlive() && distance2D(u.position, plan_.fromPosition) <= kNearMaster && (!master || distance(pos, u.position) < distance(pos, master->position)))
        {
            master = &u;
        }
    }

    if (!master)
    {
        if (ctx.navigator.moveTo(s, plan_.fromPosition, 3.0f, true) == NavStatus::Unreachable)
        {
            return fail(ctx, "the flight point could not be reached");
        }

        if (distance2D(pos, plan_.fromPosition) < 10.0f && now - stageAt_ > Duration(20000))
        {
            return fail(ctx, "no flight master at the flight point");
        }

        return Status::Running;
    }

    master_ = master->guid;
    const float reach = closeIn_ ? kCloseIn : ctx.humanReach(ctx.config.interactDistance, master->guid);

    if (distance(pos, master->position) > reach)
    {
        stage_ = Stage::ToMaster;

        if (ctx.navigator.moveTo(s, master->position, reach - 0.5f, true) == NavStatus::Unreachable)
        {
            return fail(ctx, "the flight master could not be reached");
        }

        return Status::Running;
    }

    ctx.navigator.stop();
    ctx.movement.stop();

    if (stage_ == Stage::ToMaster)
    {
        stage_ = Stage::Talking;
        stageAt_ = now;
    }

    // A gossip first, where it has one: the line to fly by.
    if (ctx.uiFrameVisible("GossipFrame") && !gossipChosen_ && ctx.provider)
    {
        for (const std::string_view phrase : {"fly", "Fly", "flight"})
        {
            for (const UiFrame& line : ctx.provider->uiFramesWithText("GossipFrame", phrase))
            {
                if (line.visible && line.bottom - line.top < 0.03f && clickAt(ctx, line))
                {
                    gossipChosen_ = true;
                    talkedAt_ = now;
                    return Status::Running;
                }
            }
        }
    }

    if (talkedAt_ != TimePoint{} && now - talkedAt_ < kAnswer)
    {
        return Status::Running;
    }

    if (talks_ >= kTalks)
    {
        return fail(ctx, "talking to the flight master opened no flight map");
    }

    if (talks_ >= 1 && !closeIn_ && distance(pos, master->position) > kCloseIn + 0.5f)
    {
        closeIn_ = true;
        return Status::Running;
    }

    if (!ctx.movement.face(s.player, master->position, 0.4f))
    {
        return Status::Running;
    }

    if (ctx.provider && ctx.provider->supportsLua())
    {
        ctx.interactNpc(master->guid, master->position);
    }
    else
    {
        // No Lua (1.60.1): a right-click on it under the mouse; where the
        // mouse finds nothing, the interact key.
        if (!mouseTried_ && ctx.zoomInForClick(rightClicks_))
        {
            return Status::Running;
        }

        const auto seek = mouseTried_ ? BotContext::MouseSeek::Unsupported : ctx.seekUnderMouse(master_, s.player, master->position, 0.9f);

        if (seek == BotContext::MouseSeek::Searching)
        {
            return Status::Running;
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
                return Status::Running;
            }
        }
    }

    ++talks_;
    talkedAt_ = now;
    gossipChosen_ = false;
    ctx.log.info(logcat::Move, "flight: talking to {} for a flight to flight point {}", master->name, plan_.to);
    return Status::Running;
}

} // namespace og
