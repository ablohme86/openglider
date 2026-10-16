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
 *  File: PartyBehaviors.cpp
 *
 *  Desc:
 *  Implementation of the Party system's behaviors (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#include "og/core/behaviors/Behaviors.h"

#include "og/core/BotContext.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>

namespace og
{

namespace
{

// Standing at the follow distance, the leader may move this much further
// before the follower walks again: a step of the leader's is not one of ours.
constexpr float kFollowSlack = 4.0f;

float followDistance(const PartyConfig& party)
{
    return party.role == PartyConfig::Role::Healer ? party.healerFollow : party.damageFollow;
}

// On a vendor trip as a party, the ones not at the vendor stand this far off,
// out of the way of the one who is.
constexpr float kVendorStandOff = 15.0f;

float followDistance(const BotContext& ctx)
{
    const float near = followDistance(ctx.config.party);
    return ctx.partyVendorQueue.empty() ? near : std::max(near, kVendorStandOff);
}

// The leader by a vendor, its own selling done, with members still to sell:
// the first of them, to wait for.
const BotContext::PartyVendorAsk* vendorWaitFor(const GameSnapshot& s, const BotContext& ctx)
{
    constexpr float kByTheVendor = 25.0f;

    if (ctx.vendorTrip || ctx.partyVendorQueue.empty())
    {
        return nullptr;
    }

    const bool byAVendor = std::any_of(s.units.begin(), s.units.end(), [&](const UnitState& u) { return u.isVendor && !u.isPlayer && u.isAlive() && distance2D(u.position, s.player.position) <= kByTheVendor; });

    if (!byAVendor)
    {
        return nullptr;
    }

    for (const auto& ask : ctx.partyVendorQueue)
    {
        if (ask.name != s.player.name)
        {
            return &ask;
        }
    }

    return nullptr;
}

} // namespace

// --- Follow --------------------------------------------------------------------

bool FollowBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // Our turn at the vendor: the vendor trip walks, not the following.
    if (!ctx.partyFollower() || ctx.partyVendorTurn || s.isDead() || s.inCombat() || !s.attackers().empty() || s.player.isCasting || ctx.actions.isCasting())
    {
        walking_ = false;
        return false;
    }

    const PartyMember* leader = ctx.party.leader();

    if (!leader)
    {
        walking_ = false;
        return false;
    }

    // Out of sight a while: "lost" in party chat, once every ten minutes; the
    // leader answers with where it is (BotInstance::partyChat).
    constexpr Duration kLostAfter{15000};
    constexpr Duration kLostEvery{600000};
    const TimePoint now = ctx.now();

    if (!leader->inSight)
    {
        if (!outOfSightSince_)
        {
            outOfSightSince_ = now;
        }

        if (now - *outOfSightSince_ >= kLostAfter && (!lostAskedAt_ || now - *lostAskedAt_ >= kLostEvery))
        {
            lostAskedAt_ = now;
            ctx.log.info(logcat::Move, "party: {} has been out of sight a while - asking where it is", leader->name);
            ctx.partySay(PartyChat::Kind::Lost);
        }

        // To where it said it is, else to where it was last seen, and no further.
        if (ctx.partyRegroup)
        {
            return distance2D(s.player.position, *ctx.partyRegroup) > ctx.config.waypointRadius;
        }

        return leader->seenAt != TimePoint{} && distance2D(s.player.position, leader->position) > ctx.config.waypointRadius;
    }

    outOfSightSince_.reset();
    ctx.partyRegroup.reset();
    lostSaid_ = false;
    const float near = followDistance(ctx);
    walking_ = leader->distance > (walking_ ? near : near + kFollowSlack);
    return walking_;
}

BehaviorStatus FollowBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const PartyMember* leader = ctx.party.leader();

    if (!leader)
    {
        return BehaviorStatus::Failure;
    }

    if (!leader->inSight && !ctx.partyRegroup && !std::exchange(lostSaid_, true))
    {
        ctx.log.info(logcat::Move, "party: {} is out of sight - going to where it was last seen", leader->name);
    }

    const float near = leader->inSight ? followDistance(ctx) : ctx.config.waypointRadius;
    const Vec3 to = leader->inSight || !ctx.partyRegroup ? leader->position : *ctx.partyRegroup;
    const NavStatus status = ctx.navigator.moveTo(s, to, near, false);

    if (status == NavStatus::Unreachable)
    {
        ctx.log.warn(logcat::Move, "party: no way to {} from here", leader->name);
        ctx.navigator.stop();
        return BehaviorStatus::Failure;
    }

    return BehaviorStatus::Running;
}

void FollowBehavior::onExit(BotContext& ctx)
{
    ctx.navigator.stop();
}

// --- PartyHeal -----------------------------------------------------------------

namespace
{

// A healer's view with `player` being the member: its health, auras and death
// for the rules to ask about, our own mana, cooldowns and casting; the member
// as the target, so a rule's range is the member's.
GameSnapshot memberView(const GameSnapshot& s, const UnitState& member)
{
    GameSnapshot view = s;
    view.player.health = member.health;
    view.player.maxHealth = member.maxHealth;
    view.player.auras = member.auras;
    view.player.isDead = member.isDead;
    view.targetGuid = member.guid;
    return view;
}

// The heal spells' usual reach.
constexpr float kHealRange = 40.0f;
// Asking the client for the member as the target: again this often, given up after the while.
constexpr Duration kTargetAskEvery{1000};
constexpr Duration kTargetGiveUp{3000};
constexpr Duration kNotTakenFor{5000};

} // namespace

bool PartyHealBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    const PartyConfig& party = ctx.config.party;

    if (!party.enabled || party.role != PartyConfig::Role::Healer || !ctx.combatClass || s.isDead())
    {
        member_ = 0;
        return false;
    }

    // A heal on its way: let it land.
    if (healed_ && (s.player.isCasting || ctx.actions.isCasting()))
    {
        return true;
    }

    const TimePoint now = ctx.now();
    std::erase_if(notTaken_, [&](const auto& entry) { return now - entry.second > kNotTakenFor; });

    // The members in sight and in reach: the lowest health first, the leader
    // first of two alike.
    struct Candidate
    {
        const UnitState* unit;
        bool leader;
    };
    std::vector<Candidate> candidates;

    for (const PartyMember& m : ctx.party.members())
    {
        if (!m.inSight || m.dead || m.distance > kHealRange || notTaken_.contains(m.guid))
        {
            continue;
        }

        if (const UnitState* u = s.findUnit(m.guid))
        {
            candidates.push_back({u, m.leader});
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.unit->healthPercent() != b.unit->healthPercent() ? a.unit->healthPercent() < b.unit->healthPercent() : a.leader && !b.leader; });
    const std::function<bool(std::string_view)> friendly = [&ctx](std::string_view action) { return ctx.actions.isFriendly(action); };

    for (const Candidate& c : candidates)
    {
        const GameSnapshot view = memberView(s, *c.unit);
        auto cc = ctx.combatContext(view);

        if (ctx.combatClass->wantsPartyHeal(cc, friendly))
        {
            member_ = c.unit->guid;
            memberName_ = c.unit->name;
            return true;
        }
    }

    member_ = 0;
    return false;
}

BehaviorStatus PartyHealBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (s.player.isCasting || ctx.actions.isCasting())
    {
        return BehaviorStatus::Running;
    }

    const UnitState* member = s.findUnit(member_);

    if (!member)
    {
        return BehaviorStatus::Failure;
    }

    const TimePoint now = ctx.now();

    // The member as the target first, the way a player takes it.
    if (s.targetGuid != member_)
    {
        if (askedFor_ != member_)
        {
            // The target we had: gone back to once the heal is cast.
            if (!healed_ && before_ == 0)
            {
                before_ = s.targetGuid;
            }

            askedFor_ = member_;
            askedSince_ = now;
            targetAsked_.reset();
            ++targetsTaken_;
        }
        else if (now - askedSince_ >= kTargetGiveUp)
        {
            ctx.log.warn(logcat::Combat, "party: could not take {} as the target to heal it", memberName_);
            notTaken_[member_] = now;
            askedFor_ = 0;
            return BehaviorStatus::Failure;
        }

        if (!targetAsked_ || now - *targetAsked_ >= kTargetAskEvery)
        {
            targetAsked_ = now;
            ctx.actions.chat("/target " + memberName_);
        }

        return BehaviorStatus::Running;
    }

    const GameSnapshot view = memberView(s, *member);
    auto cc = ctx.combatContext(view);
    const std::function<bool(std::string_view)> friendly = [&ctx](std::string_view action) { return ctx.actions.isFriendly(action); };

    if (ctx.combatClass->onPartyHeal(cc, friendly))
    {
        ctx.log.info(logcat::Combat, "party: healing {} (hp {:.0f}%)", memberName_, member->healthPercent());
        healed_ = true;
    }

    return BehaviorStatus::Running;
}

void PartyHealBehavior::onExit(BotContext& ctx)
{
    // Back to the target it had - only one member taken since, so "the last
    // target" is still that one.
    if (healed_ && before_ != 0 && targetsTaken_ == 1)
    {
        ctx.actions.chat("/targetlasttarget");
    }

    healed_ = false;
    before_ = 0;
    askedFor_ = 0;
    targetAsked_.reset();
    targetsTaken_ = 0;
}

// --- PartyWait -----------------------------------------------------------------

bool PartyWaitBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.config.party.leads() || s.isDead() || s.inCombat() || !s.attackers().empty())
    {
        said_.clear();
        return false;
    }

    if (!ctx.party.waitFor(ctx.config.party.partyRange) && !vendorWaitFor(s, ctx))
    {
        if (!said_.empty())
        {
            ctx.log.info(logcat::Move, "party: everyone is here - going on");
            said_.clear();
        }

        return false;
    }

    return true;
}

void PartyWaitBehavior::onEnter(const GameSnapshot&, BotContext& ctx)
{
    // No pull goes on to a mob we had only chosen.
    ctx.combat.engaged = 0;
    ctx.navigator.stop();
    ctx.movement.stop();
}

BehaviorStatus PartyWaitBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (const auto* ask = vendorWaitFor(s, ctx); ask && !ctx.party.waitFor(ctx.config.party.partyRange))
    {
        if (const std::string kind = ask->name + ":vendor"; kind != said_)
        {
            ctx.log.info(logcat::Move, "party: waiting at the vendor for {}", ask->name);
            said_ = kind;
        }
    }
    else if (const auto wait = ctx.party.waitFor(ctx.config.party.partyRange))
    {
        // Once for each member and kind of reason: not again for every yard.
        std::string kind = wait->member->name + ":";

        for (const char c : wait->reason)
        {
            if (!std::isdigit(static_cast<unsigned char>(c)))
            {
                kind += c;
            }
        }

        if (kind != said_)
        {
            ctx.log.info(logcat::Move, "party: waiting for {} ({})", wait->member->name, wait->reason);
            said_ = std::move(kind);
        }
    }

    if (ctx.movement.isMoving() || s.player.isMoving)
    {
        ctx.movement.stop();
    }

    return BehaviorStatus::Running;
}

} // namespace og
