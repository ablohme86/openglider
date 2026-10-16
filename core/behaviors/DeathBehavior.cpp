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
 *  File: DeathBehavior.cpp
 *
 *  Desc:
 *  Implementation of DeathBehavior (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/actions/ActionBinding.h"
#include "og/common/MathUtil.h"
#include "og/core/behaviors/Behaviors.h"
#include "og/state/IGameStateProvider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>

namespace og
{

namespace
{

// A ghost moving this far between two snapshots was put somewhere, not walked:
// released to the graveyard.
constexpr float kGhostTeleport = 30.0f;
// How long a fresh ghost may still be reported on its body before it is
// believed to be there (it can die in a graveyard).
constexpr Duration kReleaseSettle{3000};
// The client lets go of keys pressed while it moves a fresh ghost to the
// graveyard: nothing is pressed until it has been a ghost this long.
constexpr Duration kGhostSettle{1500};
// Forward held and the ghost not a step further this long: the client has let
// go of the key, and it is pressed again. Well before the stuck check (5.5 s)
// takes the spot for an obstacle.
constexpr Duration kKeysDropped{1500};

std::size_t nearestIndex(const std::vector<Vec3>& pts, const Vec3& p)
{
    std::size_t best = 0;
    float bestDist = std::numeric_limits<float>::max();

    for (std::size_t i = 0; i < pts.size(); ++i)
    {
        const float d = distance(pts[i], p);

        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }

    return best;
}

// Releasing and retrieving are bound to plain keys by default, which only do
// anything if the user has put a /script RepopMe() macro on that bar slot - so
// on a client the bot can run Lua in, call the API instead. The keypress stays
// as the fallback for clients without Lua (1.12.1).
//
// The client also puts a dialog up for both, and the popup is what the user
// sees, so accept it as well. `match` is tested against the popup's `which` so
// nothing unrelated (a group invite, a trade) is ever clicked away.
bool runDeathLua(BotContext& ctx, std::string_view call, std::string_view match)
{
    if (!ctx.provider || !ctx.provider->supportsLua() || !ctx.provider->luaIdle())
    {
        return false;
    }

    const std::string script = std::string(call) + "() local p=StaticPopup1 if p and p:IsVisible() and p.which and " + std::string(match) + " then StaticPopup1Button1:Click() end";
    return ctx.provider->executeLua(script);
}

// Without Lua, a 64-bit client's dialog is read and its button clicked with
// the mouse, as a player would, so no addon is needed. DEATH says "Release
// Spirit" on its button; RECOVER_CORPSE (and its instance form, and a
// player's resurrection) "Resurrect" in its text.
enum class DeathDialog
{
    Release,
    Retrieve
};

bool clickDeathPopup(BotContext& ctx, DeathDialog dialog)
{
    return dialog == DeathDialog::Release ? ctx.clickDialog({}, "Release Spirit") : ctx.clickDialog("resurrect");
}

} // namespace

bool DeathBehavior::canRun(const GameSnapshot& s, BotContext&)
{
    return s.player.isDeadOrGhost();
}

void DeathBehavior::onEnter(const GameSnapshot&, BotContext& ctx)
{
    deadSince_ = ctx.now();
    lastRelease_.reset();
    lastRetrieve_.reset();
    ghostPath_.clear();
    ghostIndex_ = 0;
    approaches_.clear();
    approachIndex_ = 0;
    approachRetryAt_.reset();
    safeSpot_.reset();
    safeSpotGivenUp_ = false;
    campedSince_.reset();
    warnedNoCorpse_ = false;
    ghostSince_.reset();
    lastGhostPosition_.reset();
    stillAt_.reset();
    keysRefreshed_ = false;
    corpseRunTotalDistance_ = 0.0f;
    ctx.resurrectEscape.reset();
    ctx.movement.stop();
    ctx.combat.engaged = 0;
    ctx.pendingLoot.clear();
    ctx.objectives.push(Objective{"CorpseRun", 0, ctx.lastDeathPosition});
}

void DeathBehavior::onExit(BotContext& ctx)
{
    corpseRunTotalDistance_ = 0.0f;
    ctx.objectives.removeAll("CorpseRun");
    ctx.navigator.stop();
}

void DeathBehavior::buildGhostPath(const Vec3& from, const Vec3& corpse, BotContext& ctx)
{
    ghostPath_.clear();
    ghostIndex_ = 0;

    if (ctx.profile && ctx.profile->ghostWaypoints.size() >= 2)
    {
        const auto& gw = ctx.profile->ghostWaypoints;
        const std::size_t start = nearestIndex(gw, from);
        const std::size_t end = nearestIndex(gw, corpse);

        // Only use the ghost route if it actually brings us closer to the corpse.
        if (distance(from, corpse) > distance(gw[end], corpse))
        {
            if (start <= end)
            {
                for (std::size_t i = start; i <= end; ++i)
                {
                    ghostPath_.push_back(gw[i]);
                }
            }
            else
            {
                for (std::size_t i = start + 1; i-- > end;)
                {
                    ghostPath_.push_back(gw[i]);
                }
            }
        }
    }

    detours_.abandon();
    detours_.apply(ghostPath_, ctx);
    ghostPath_.push_back(corpse);
    ctx.log.info(logcat::Death, "corpse run: {} node(s), corpse at {} ({:.0f} yd)", ghostPath_.size(), corpse, distance(from, corpse));
}

Vec3 DeathBehavior::findSafeSpot(const Vec3& corpse, const GameSnapshot& s, BotContext& ctx) const
{
    // The whole of resurrect range, ring by ring: the corpse, then 16 ways
    // out at a third, three fifths, four fifths and nearly all of it.
    constexpr int kWays = 16;
    const float reach = std::max(2.0f, ctx.config.resurrectRange - 1.0f);
    Vec3 best = corpse;
    float bestScore = -1.0f;

    for (int i = 0; i <= 4 * kWays; ++i)
    {
        Vec3 candidate = corpse;

        if (i > 0)
        {
            const float share = std::array{0.35f, 0.6f, 0.8f, 1.0f}[static_cast<std::size_t>((i - 1) / kWays)];
            const float a = static_cast<float>((i - 1) % kWays) * kTwoPi / static_cast<float>(kWays);
            candidate = corpse + Vec3{std::cos(a) * reach * share, std::sin(a) * reach * share, 0.0f};
        }

        if (ctx.areaBlacklist.contains(candidate) || !ctx.navigator.canStandAt(candidate))
        {
            continue;
        }

        if (ctx.navigator.provider().usesClientGeometry())
        {
            if (const auto walkable = ctx.navigator.provider().findWalkablePoint(candidate, 3.0f, 0.5f))
            {
                candidate = *walkable;
            }
            else
            {
                continue;
            }
        }

        float nearest = 1000.0f;

        for (const auto& u : s.units)
        {
            if (!u.isAlive() || u.reaction != Reaction::Hostile || u.isCritter)
            {
                continue;
            }

            nearest = std::min(nearest, distance(u.position, candidate));
        }

        if (nearest > bestScore)
        {
            bestScore = nearest;
            best = candidate;
        }
    }

    return best;
}

Vec3 DeathBehavior::findEscapeSpot(const Vec3& from, const Vec3& corpse, const GameSnapshot& s, BotContext& ctx) const
{
    Vec3 best = from;
    float bestScore = -1.0f;

    // 1. Check points along ghostPath_ leading away from the corpse
    for (const Vec3& pt : ghostPath_)
    {
        const float distCorpse = distance2D(pt, corpse);
        if (distCorpse < 25.0f || distCorpse > 60.0f)
        {
            continue;
        }

        if (ctx.areaBlacklist.contains(pt) || !ctx.navigator.canStandAt(pt))
        {
            continue;
        }

        float nearestHostile = 1000.0f;
        for (const auto& u : s.units)
        {
            if (!u.isAlive() || u.reaction != Reaction::Hostile || u.isCritter)
            {
                continue;
            }
            nearestHostile = std::min(nearestHostile, distance2D(u.position, pt));
        }

        // Favor ghostPath_ nodes since they follow a known walkable path back towards safety
        const float score = nearestHostile + 5.0f;
        if (score > bestScore)
        {
            bestScore = score;
            best = pt;
        }
    }

    // 2. Radial candidates away from the centroid of nearby hostiles
    Vec3 hostileCenter{};
    int hostileCount = 0;
    for (const auto& u : s.units)
    {
        if (u.isAlive() && u.reaction == Reaction::Hostile && !u.isCritter && distance2D(u.position, corpse) <= 30.0f)
        {
            hostileCenter = hostileCenter + u.position;
            ++hostileCount;
        }
    }
    if (hostileCount > 0)
    {
        hostileCenter = hostileCenter * (1.0f / static_cast<float>(hostileCount));
    }
    else
    {
        hostileCenter = corpse;
    }

    Vec3 awayDir = from - hostileCenter;
    awayDir.z = 0.0f;
    const float awayLen = std::hypot(awayDir.x, awayDir.y);
    const Vec3 baseDir = awayLen > 0.1f ? awayDir * (1.0f / awayLen) : Vec3{1.0f, 0.0f, 0.0f};
    const float baseAngle = std::atan2(baseDir.y, baseDir.x);

    for (const float dist : {30.0f, 40.0f, 50.0f})
    {
        for (int i = -4; i <= 4; ++i)
        {
            const float a = baseAngle + static_cast<float>(i) * (kPi / 8.0f);
            Vec3 candidate = from + Vec3{std::cos(a) * dist, std::sin(a) * dist, 0.0f};

            if (distance2D(candidate, corpse) < 20.0f || ctx.areaBlacklist.contains(candidate) || !ctx.navigator.canStandAt(candidate))
            {
                continue;
            }

            float nearestHostile = 1000.0f;
            for (const auto& u : s.units)
            {
                if (!u.isAlive() || u.reaction != Reaction::Hostile || u.isCritter)
                {
                    continue;
                }
                nearestHostile = std::min(nearestHostile, distance2D(u.position, candidate));
            }

            if (nearestHostile > bestScore)
            {
                bestScore = nearestHostile;
                best = candidate;
            }
        }
    }

    // Fallback: if nothing found, move in awayDir by 35 yards
    if (bestScore < 0.0f)
    {
        best = from + baseDir * 35.0f;
    }

    return best;
}

// Where to retrieve the corpse from, nearest the ghost first. A route cannot end
// in a No Go zone or a learned obstacle, so a corpse lying in one is retrieved
// from a ring round it that is still within resurrect range.
std::vector<Vec3> DeathBehavior::corpseApproaches(const Vec3& from, const Vec3& corpse, BotContext& ctx) const
{
    std::vector<Vec3> spots;

    if (ctx.navigator.canStandAt(corpse))
    {
        spots.push_back(corpse);
    }

    for (const float share : {0.4f, 0.65f, 0.9f})
    {
        const float radius = ctx.config.resurrectRange * share;

        for (int i = 0; i < 16; ++i)
        {
            const float a = static_cast<float>(i) * kTwoPi / 16.0f;
            Vec3 spot = corpse + Vec3{std::cos(a) * radius, std::sin(a) * radius, 0.0f};

            if (!ctx.navigator.canStandAt(spot))
            {
                continue;
            }

            if (ctx.navigator.provider().usesClientGeometry())
            {
                if (const auto walkable = ctx.navigator.provider().findWalkablePoint(spot, 3.0f, 0.5f))
                {
                    spot = *walkable;
                }
                else
                {
                    continue;
                }
            }

            spots.push_back(spot);
        }
    }

    std::sort(spots.begin(), spots.end(), [&](const Vec3& a, const Vec3& b) { return distance2D(from, a) < distance2D(from, b); });

    if (spots.empty())
    {
        spots.push_back(corpse);
    }

    return spots;
}

BehaviorStatus DeathBehavior::approachCorpse(const GameSnapshot& s, const Vec3& corpse, BotContext& ctx)
{
    const TimePoint now = ctx.now();

    if (approachRetryAt_)
    {
        if (now < *approachRetryAt_)
        {
            return BehaviorStatus::Running;
        }

        approachRetryAt_.reset();
        approaches_.clear();
    }

    if (approaches_.empty())
    {
        approaches_ = corpseApproaches(s.player.position, corpse, ctx);
        approachIndex_ = 0;

        if (distance2D(approaches_.front(), corpse) > 0.1f)
        {
            ctx.log.info(logcat::Death, "corpse lies in an obstacle or a No Go zone - retrieving it from {} ({:.0f} yd off)", approaches_.front(), distance2D(approaches_.front(), corpse));
        }
    }

    const Vec3& spot = approaches_[approachIndex_];
    const float offset = distance2D(spot, corpse);
    const float radius = offset < 0.1f ? ctx.config.resurrectRange * 0.6f : std::max(1.0f, ctx.config.resurrectRange - offset - 1.0f);

    if (ctx.navigator.moveTo(s, spot, radius, true) != NavStatus::Unreachable)
    {
        return BehaviorStatus::Running;
    }

    if (++approachIndex_ < approaches_.size())
    {
        ctx.log.debug(logcat::Death, "no route to {} - trying {}", spot, approaches_[approachIndex_]);
        return BehaviorStatus::Running;
    }

    // Nothing near the corpse can be reached now; the way may clear once an
    // obstacle is forgotten, so try again later instead of every tick.
    ctx.log.warn(logcat::Death, "no route to within {:.0f} yd of the corpse - trying again in 5 seconds", ctx.config.resurrectRange);
    ctx.navigator.stop();
    approachRetryAt_ = now + Duration(5000);
    return BehaviorStatus::Running;
}

BehaviorStatus DeathBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();
    const PlayerState& p = s.player;

    // Dead body: release spirit.
    if (!p.isGhost)
    {
        // Attached to a character already dead: it lies where its body is.
        if (!ctx.lastDeathPosition)
        {
            ctx.lastDeathPosition = p.corpsePosition ? *p.corpsePosition : p.position;
        }

        ctx.movement.stop();

        if (now - deadSince_ >= ctx.config.releaseDelay && (!lastRelease_ || now - *lastRelease_ >= ctx.config.releaseRetry))
        {
            ctx.log.info(logcat::Death, "releasing spirit");

            if (!runDeathLua(ctx, "RepopMe", "p.which=='DEATH'") && !clickDeathPopup(ctx, DeathDialog::Release))
            {
                ctx.actions.execute(actions::ReleaseSpirit);
            }

            lastRelease_ = now;
        }

        return BehaviorStatus::Running;
    }

    // Ghost: run back to the corpse.
    const std::optional<Vec3> corpse = p.corpsePosition ? p.corpsePosition : ctx.lastDeathPosition;

    if (!corpse)
    {
        if (!warnedNoCorpse_)
        {
            ctx.log.error(logcat::Death, "ghost but corpse position unknown");
        }

        warnedNoCorpse_ = true;
        return BehaviorStatus::Running;
    }

    if (!ghostSince_)
    {
        ghostSince_ = now;
    }

    // The first snapshots after the release can still have the ghost on its
    // body; planning from there finds the corpse "in range", with the mobs that
    // killed it next to it. Wait for the move to the graveyard.
    if (now - *ghostSince_ < kGhostSettle || (now - *ghostSince_ < kReleaseSettle && distance2D(p.position, *corpse) < 1.0f))
    {
        ctx.movement.stop();
        return BehaviorStatus::Running;
    }

    // Running, or turning on the spot to set off (a turn key the client
    // dropped as well left the ghost facing the graveyard wall for good).
    const bool keysDown = ctx.movement.isMovingForward() || ctx.movement.turnDirection() != 0;

    if (!keysDown || !stillAt_ || distance(*stillAt_, p.position) > 0.5f || std::fabs(normalizeAngle(p.heading - stillHeading_)) > 0.05f)
    {
        stillAt_ = p.position;
        stillHeading_ = p.heading;
        stillSince_ = now;
        keysRefreshed_ = false;
    }
    else if (!keysRefreshed_ && now - stillSince_ >= kKeysDropped)
    {
        ctx.log.info(logcat::Death, "the ghost is not moving with the keys down - pressing them again");
        ctx.movement.stop();
        keysRefreshed_ = true;
        stillSince_ = now;
        return BehaviorStatus::Running;
    }

    // Moved to the graveyard after all that: everything planned from the body
    // is void, and the keys held for it are gone from the client too.
    if (lastGhostPosition_ && distance(*lastGhostPosition_, p.position) > kGhostTeleport)
    {
        ctx.log.info(logcat::Death, "ghost moved {:.0f} yd to the graveyard - planning the corpse run from here", distance(*lastGhostPosition_, p.position));
        ghostPath_.clear();
        ghostIndex_ = 0;
        corpseRunTotalDistance_ = 0.0f;
        approaches_.clear();
        approachIndex_ = 0;
        approachRetryAt_.reset();
        safeSpot_.reset();
        safeSpotGivenUp_ = false;
        campedSince_.reset();
        lastRetrieve_.reset();
        stillAt_.reset();
        detours_.abandon();
        ctx.navigator.stop();
        ctx.movement.stop();
    }

    lastGhostPosition_ = p.position;

    if (ghostPath_.empty())
    {
        buildGhostPath(p.position, *corpse, ctx);
    }

    const float dist = distance2D(p.position, *corpse);

    if (corpseRunTotalDistance_ == 0.0f)
    {
        corpseRunTotalDistance_ = !ghostPath_.empty() ? remainingPathDistance(p.position, ghostPath_, 0) : dist;
    }

    if (dist > ctx.config.resurrectRange)
    {
        if (ghostIndex_ + 1 >= ghostPath_.size())
        {
            return approachCorpse(s, *corpse, ctx);
        }

        // The ghost route is the player's own, walked point by point whatever No
        // Go zones or learned obstacles say; the corpse itself is approached
        // clear of them (approachCorpse), as it may lie in one.
        const NavStatus st = ctx.navigator.moveTo(s, ghostPath_[ghostIndex_], ctx.config.waypointRadius, false, true);
        detours_.update(s, ctx, ghostIndex_, st == NavStatus::Recovering);

        if (st == NavStatus::Unreachable)
        {
            detours_.abandon();
            ++ghostIndex_;
        }
        else if (st == NavStatus::Arrived)
        {
            // Past any way round that went in before the node just reached.
            ghostIndex_ += detours_.reached(ghostPath_, ghostIndex_, ctx) + 1;
        }

        return BehaviorStatus::Running;
    }

    // In range: never up next to hostiles. The ghost moves about within
    // resurrect range to the spot furthest from them, and again when they
    // come to it, until none is within corpseSafeRadius of it - or, camped
    // for corpseCampedTimeout, resurrects all the same and runs.
    const auto threatened = [&](const Vec3& at) { return !s.hostilesNear(at, ctx.config.corpseSafeRadius).empty(); };

    if (!safeSpotGivenUp_ && threatened(p.position))
    {
        constexpr Duration kRethink{3000};

        if (!campedSince_)
        {
            campedSince_ = now;
        }

        if (now - *campedSince_ >= ctx.config.corpseCampedTimeout)
        {
            safeSpotGivenUp_ = true;
            const Vec3 escapeSpot = findEscapeSpot(p.position, *corpse, s, ctx);
            ctx.resurrectEscape = ResurrectEscape{escapeSpot, p.position, now};
            ctx.log.info(logcat::Death, "hostiles still near the corpse after {:.0f}s - resurrecting {:.0f} yd from it and running to {}", toSeconds(now - *campedSince_), distance2D(p.position, *corpse), escapeSpot);
        }
        else
        {
            // A spot taken over by a hostile since it was chosen: another.
            if (!safeSpot_ || (threatened(*safeSpot_) && now - safeSpotThought_ >= kRethink))
            {
                const Vec3 spot = findSafeSpot(*corpse, s, ctx);
                safeSpotThought_ = now;

                if (!safeSpot_ || distance2D(spot, *safeSpot_) > 3.0f)
                {
                    ctx.log.info(logcat::Death, "hostiles near the ghost - moving to {} ({:.0f} yd from the corpse) to resurrect clear of them", spot, distance2D(spot, *corpse));
                    safeSpot_ = spot;
                    safeSpotStarted_ = now;
                }
            }

            if (distance2D(p.position, *safeSpot_) > 2.0f)
            {
                const NavStatus st = ctx.navigator.moveTo(s, *safeSpot_, 2.0f);

                // No way there: that spot is not tried again for a while.
                if (st == NavStatus::Unreachable || now - safeSpotStarted_ >= ctx.config.safeSpotTimeout)
                {
                    ctx.log.info(logcat::Death, "cannot get to {} ({}after {:.0f}s) - trying another spot", *safeSpot_, st == NavStatus::Unreachable ? "unreachable, " : "", toSeconds(now - safeSpotStarted_));
                    ctx.areaBlacklist.add(*safeSpot_, 3.0f, Duration(60000), "no way there for the ghost");
                    safeSpot_.reset();
                }
            }
            else
            {
                // There, and still not clear: wait for them to wander off.
                ctx.navigator.halt();
            }

            return BehaviorStatus::Running;
        }
    }
    else if (!threatened(p.position))
    {
        campedSince_.reset();
    }

    ctx.navigator.stop();

    if (!lastRetrieve_ || now - *lastRetrieve_ >= ctx.config.retrieveRetry)
    {
        ctx.log.info(logcat::Death, "retrieving corpse ({:.0f} yd away)", dist);

        // RECOVER_CORPSE and RECOVER_CORPSE_INSTANCE both mean "resurrect here".
        if (!runDeathLua(ctx, "RetrieveCorpse", "string.find(p.which,'RECOVER_CORPSE',1,true)") && !clickDeathPopup(ctx, DeathDialog::Retrieve))
        {
            ctx.actions.execute(actions::RetrieveCorpse);
        }

        lastRetrieve_ = now;
    }

    return BehaviorStatus::Running;
}

} // namespace og
