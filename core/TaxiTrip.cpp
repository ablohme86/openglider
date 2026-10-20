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
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>

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
        return Continent{4910.0f, -7150.0f, 5840.0f, -17240.0f};
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
// The flight map just up: its buttons laid out a moment later - read at once
// the first time after attaching, none was where its flight point is.
const Duration kMapSettles{1000};
const Duration kMapLooked{4000};    // the flight point looked for this long
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

std::vector<UiFrame> flightGossipLines(IGameStateProvider& provider)
{
    const std::initializer_list<std::string_view> flying{"fly", "flight", "flieg", "flug", "voler", "volar", "voar", "полет", "лететь", "날아", "비행", "飛", "飞"};
    const std::initializer_list<std::string_view> riding{"ride", "travel", "reise", "viaj"};
    std::vector<UiFrame> found;

    for (const auto& words : {flying, riding})
    {
        for (const std::string_view word : words)
        {
            for (const UiFrame& line : provider.uiFramesWithText("GossipFrame", word))
            {
                // The same line under two of the words: one line.
                const bool known = std::any_of(found.begin(), found.end(), [&](const UiFrame& f) { return f.top == line.top && f.left == line.left; });

                if (line.visible && line.bottom - line.top < 0.03f && !known)
                {
                    found.push_back(line);
                }
            }
        }

        if (!found.empty())
        {
            break;
        }
    }

    return found;
}

std::optional<UiFrame> flightGossipChoice(IGameStateProvider& provider)
{
    std::vector<UiFrame> gossipButtons;
    for (int i = 1; i <= 16; ++i)
    {
        if (const auto btn = provider.uiFrame(std::format("GossipTitleButton{}", i)); btn && btn->visible)
        {
            gossipButtons.push_back(*btn);
        }
    }

    auto known = flightGossipLines(provider);

    if (!gossipButtons.empty())
    {
        std::erase_if(known, [&](const UiFrame& line) {
            return std::none_of(gossipButtons.begin(), gossipButtons.end(), [&](const UiFrame& btn) {
                return std::abs(line.centerY() - btn.centerY()) < 0.02f || (line.top >= btn.top - 0.01f && line.bottom <= btn.bottom + 0.01f);
            });
        });
    }

    if (known.size() == 1)
    {
        return known.front();
    }

    if (known.size() == 2)
    {
        const auto topMost = std::min_element(known.begin(), known.end(), [](const UiFrame& a, const UiFrame& b) { return a.top < b.top; });
        const auto bottomMost = std::max_element(known.begin(), known.end(), [](const UiFrame& a, const UiFrame& b) { return a.top < b.top; });
        if (bottomMost->top > topMost->bottom)
        {
            return *bottomMost;
        }
    }

    if (gossipButtons.size() == 1)
    {
        return gossipButtons.front();
    }

    return std::nullopt;
}

std::optional<int> taxiButtonFor(std::uint32_t target, std::uint32_t mapId, const std::vector<std::pair<int, UiFrame>>& buttons, const UiFrame& map, const TaxiNetwork& network, const std::vector<std::uint32_t>& known, std::optional<std::uint32_t> from)
{
    const auto c = continent(mapId);
    const float width = map.right - map.left, height = map.bottom - map.top;

    if (!c || width <= 0 || height <= 0 || buttons.empty() || (from && target == *from))
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

    std::vector<std::uint32_t> candidates = known;
    if (std::find(candidates.begin(), candidates.end(), target) == candidates.end())
    {
        candidates.push_back(target);
    }
    if (from && std::find(candidates.begin(), candidates.end(), *from) == candidates.end())
    {
        candidates.push_back(*from);
    }

    struct Edge
    {
        float dist;
        int buttonIndex;
        std::uint32_t nodeId;
    };
    std::vector<Edge> edges;

    constexpr float kMaxApart = 0.14f;

    for (const auto& [bIdx, bFrame] : buttons)
    {
        const auto bPos = onMap(bFrame);
        for (const auto id : candidates)
        {
            if (const auto* n = network.node(id); n && n->mapId == mapId)
            {
                const float d = apart(bPos, placed(*n));
                if (d < kMaxApart)
                {
                    edges.push_back({d, bIdx, id});
                }
            }
        }
    }

    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        return a.dist < b.dist;
    });

    std::unordered_set<int> matchedButtons;
    std::unordered_set<std::uint32_t> matchedNodes;
    std::unordered_map<std::uint32_t, int> nodeToButton;

    for (const auto& e : edges)
    {
        if (!matchedButtons.contains(e.buttonIndex) && !matchedNodes.contains(e.nodeId))
        {
            matchedButtons.insert(e.buttonIndex);
            matchedNodes.insert(e.nodeId);
            nodeToButton[e.nodeId] = e.buttonIndex;
        }
    }

    if (const auto it = nodeToButton.find(target); it != nodeToButton.end())
    {
        if (from)
        {
            if (const auto fromIt = nodeToButton.find(*from); fromIt != nodeToButton.end() && fromIt->second == it->second)
            {
                return std::nullopt;
            }
        }
        return it->second;
    }

    return std::nullopt;
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
        if (mapSeenAt_ == TimePoint{})
        {
            mapSeenAt_ = now;
        }

        if (now - clickedAt_ < kAnswer || now - mapSeenAt_ < kMapSettles)
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
        const auto button = map && ctx.network() ? taxiButtonFor(plan_.to, mapId_, buttons, *map, *ctx.network(), known, plan_.from) : std::nullopt;

        if (button)
        {
            if (ctx.provider && ctx.provider->supportsLua())
            {
                ctx.runScript(std::format("TakeTaxiNode({})", *button));
            }
            else
            {
                const auto frame = std::find_if(buttons.begin(), buttons.end(), [&](const auto& b) { return b.first == *button; })->second;
                if (!clickAt(ctx, frame))
                {
                    return Status::Running;
                }
            }

            ++clicks_;
            clickedAt_ = now;
            stage_ = Stage::Flying;
            lastPosition_ = pos;
            ctx.log.info(logcat::Move, "flight: flying from flight point {} to {} (button TaxiButton{})", plan_.from, plan_.to, *button);
            return Status::Running;
        }

        if (buttons.empty() && ctx.provider && ctx.provider->supportsLua())
        {
            const auto c = continent(mapId_);
            const float tx = c ? (c->left - plan_.toPosition.y) / (c->left - c->right) : -1.0f;
            const float ty = c ? (c->top - plan_.toPosition.x) / (c->top - c->bottom) : -1.0f;
            if (tx < 0.0f || tx > 1.0f || ty < 0.0f || ty > 1.0f)
            {
                return fail(ctx, "flight destination outside continent map bounds");
            }

            const std::string script = std::format(
                "local tx,ty={:.4f},{:.4f};"
                "local cost={};"
                "local num=NumTaxiNodes();"
                "local best,bestDist=nil,999999;"
                "for i=1,num do "
                "if TaxiNodeGetType(i)=='REACHABLE' then "
                "local x,y=TaxiNodePosition(i);"
                "local d=(x-tx)*(x-tx)+(y-ty)*(y-ty);"
                "if d<0.04*0.04 then "
                "if d<bestDist-0.0005 then bestDist=d;best=i "
                "elseif math.abs(d-bestDist)<=0.0005 and cost>0 and TaxiNodeCost(i)==cost then best=i end end end end "
                "if best then TakeTaxiNode(best) end",
                tx, ty, plan_.cost);
            ctx.runScript(script);
            ++clicks_;
            clickedAt_ = now;
            stage_ = Stage::Flying;
            lastPosition_ = pos;
            ctx.log.info(logcat::Move, "flight: flying from flight point {} to {} ({} copper)", plan_.from, plan_.to, plan_.cost);
            return Status::Running;
        }

        // Looked for again a while, the map still settling.
        if (now - mapSeenAt_ < kMapLooked)
        {
            return Status::Running;
        }

        return fail(ctx, std::format("flight point {} not found on the flight map ({} shown)", plan_.to, buttons.size()));
    }

    // The flight map shut: looked at anew when it opens again.
    mapSeenAt_ = TimePoint{};

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
        if (ctx.provider->supportsLua())
        {
            ctx.runScript(kChooseFlightLua);
            gossipChosen_ = true;
            talkedAt_ = now;
            return Status::Running;
        }

        if (const auto choice = flightGossipChoice(*ctx.provider))
        {
            if (clickAt(ctx, *choice))
            {
                gossipChosen_ = true;
                talkedAt_ = now;
                return Status::Running;
            }
        }

        // None of those words, or more than one line with one: a flight
        // master's gossip with its greeting and one line under it has that
        // line to fly by, whatever it says.
        std::vector<UiFrame> lines;

        for (const UiFrame& line : ctx.provider->uiFramesWithText("GossipFrame", ""))
        {
            if (line.visible && line.bottom - line.top < 0.03f)
            {
                lines.push_back(line);
            }
        }

        if (!lines.empty() && lines.size() <= 2)
        {
            const auto lowest = std::max_element(lines.begin(), lines.end(), [](const UiFrame& a, const UiFrame& b) { return a.top < b.top; });

            if (clickAt(ctx, *lowest))
            {
                ctx.log.info(logcat::Move, "flight: the flight master's gossip line known by no word - its only line clicked");
                gossipChosen_ = true;
                talkedAt_ = now;
                return Status::Running;
            }
        }
        else if (!lines.empty())
        {
            ctx.log.info(logcat::Move, "flight: the flight master's gossip has {} lines and none known to fly by", lines.size());
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

    if (!ctx.turningRound(master_) && !ctx.movement.face(s.player, master->position, 0.4f))
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

        const auto seek = mouseTried_ ? BotContext::MouseSeek::Unsupported : ctx.seekNpcTurning(s.player, master_, master->position);

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
