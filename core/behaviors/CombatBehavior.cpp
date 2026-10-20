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
 *  File: CombatBehavior.cpp
 *
 *  Desc:
 *  Implementation of CombatBehavior (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/actions/ActionBinding.h"
#include "og/common/MathUtil.h"
#include "og/core/behaviors/Behaviors.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace og
{

namespace
{
// Closest the pull approach walks in: melee reach.
constexpr float kPullFloor = 1.5f;
// An auto attack swings within this.
constexpr float kMeleeReach = 5.0f;

// Turning does not interrupt a cast, but combat used to stop steering while it
// waited for one: a turn key down when the cast started stayed down for the
// whole cast and the character spun on the spot. Keep facing the target
// instead, which also follows it when it moves. True while casting.
bool faceWhileCasting(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float tolerance)
{
    if (!s.player.isCasting && !ctx.actions.isCasting())
    {
        return false;
    }

    ctx.movement.face(s.player, unit.position, tolerance);
    return true;
}

// Walking in, the bot goes this much past the distance a pull rule starts to
// fire at. Stopped right on it, a mob wandering off took the caster out of
// range again at once, and it stepped after it, stop-start.
constexpr float kPullOvershoot = 0.9f;

// A target out of reach is chased in to this share of the reach - 15 %
// inside it - so one that keeps walking off stays in range a while before it
// has to be chased again.
constexpr float kChaseMargin = 0.85f;

inline float cfgPullFloor(const BotContext& ctx) { return ctx.config.pullFloor > 0.0f ? ctx.config.pullFloor : kPullFloor; }
inline float cfgMeleeReach(const BotContext& ctx) { return ctx.config.meleeReach > 0.0f ? ctx.config.meleeReach : kMeleeReach; }
inline float cfgPullOvershoot(const BotContext& ctx) { return (ctx.config.pullOvershoot > 0.0f && ctx.config.pullOvershoot < 1.0f) ? ctx.config.pullOvershoot : kPullOvershoot; }
inline float cfgChaseMargin(const BotContext& ctx) { return (ctx.config.chaseMargin > 0.0f && ctx.config.chaseMargin < 1.0f) ? ctx.config.chaseMargin : kChaseMargin; }

// A pull that has not drawn the mob by now did not go off: a shot or spell
// lands well within this, and a mob it hit is in combat at once.
constexpr Duration kPullNoEffect{3000};
// A pull action that drew nothing in this long (Hunter's Mark from 40 yd, the
// mob out of Auto Shot's reach) no longer holds the walk in to a rule that
// fires closer: the hunter stood there until the mob wandered in.
constexpr Duration kPullDrewNothing{1500};

// A mob we are hitting is left as another player's only when it shows their
// tag this long.
constexpr Duration kTapSettle{3000};

// A mob in melee reach this far round from the target we face is hitting us
// from behind: past a right angle, with a margin so one at our shoulder does
// not count. Backed past, it turns up in front, as the others following us do.
constexpr float kBehindAngle = kPi / 2.0f + 0.25f;
// Backing up goes on until the mobs are well in front, not just out of the
// back: let go the moment one came round to our side, the key went up after
// a fraction of a yard, the bot stepped in again, and backed again later -
// stop-start.
constexpr float kWellInFront = kPi / 3.0f;
// A little step back: at most this far and this long, then a rest before the
// next one, so a mob that keeps coming round does not walk the bot off.
constexpr float kBackStep = 4.0f;
constexpr Duration kBackLongest{1500};
constexpr Duration kBackRest{4000};
// A mob above the maximum mob level this near where it would notice us is
// stepped away from: from kDodgeMargin outside its aggro range until
// kDodgeClear farther, for kDodgeLongest at the most, then the fight goes on
// where it is for kDodgeRest (it may not be possible to get clear).
constexpr float kDodgeMargin = 3.0f;
constexpr float kDodgeClear = 6.0f;
constexpr float kDodgeStep = 4.0f;
constexpr Duration kDodgeLongest{8000};
constexpr Duration kDodgeRest{4000};
constexpr Duration kDodgeSteer{500};

// Can the character see the mob? On the client's map, a straight walk to it
// stands in for a line of sight: a wall of the building in between, or the
// floor above, and Tab took nothing and Auto Shot was "not in line of sight".
// A hunter stopped at 32 yards outside the barracks and tabbed at the wall.
bool inSight(BotContext& ctx, const Vec3& from, const Vec3& to)
{
    INavigationProvider& nav = ctx.navigator.provider();
    return !nav.usesClientGeometry() || nav.canWalkStraight(from, to);
}

// Would a pull rule fire with the mob `d` yards away, on the line it is on now?
bool pullFiresAt(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float d)
{
    const float dist = distance(s.player.position, unit.position);

    if (!ctx.combatClass || dist <= 0.0f)
    {
        return false;
    }

    GameSnapshot moved = s;
    const auto it = std::find_if(moved.units.begin(), moved.units.end(), [&](const UnitState& u) { return u.guid == unit.guid; });

    if (it == moved.units.end())
    {
        return false;
    }

    it->position = s.player.position + (unit.position - s.player.position) * (d / dist);
    auto cc = ctx.combatContext(moved);
    return ctx.combatClass->wantsPull(cc);
}

// Would a combat rule fire with the mob `d` yards away, on the line it is on
// now? Auto Shot or the wand's Shoot counts as firing where it reaches, on or
// not: the client shoots again by itself once the mob is back in its reach.
bool combatFiresAt(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float d)
{
    const float dist = distance(s.player.position, unit.position);

    if (!ctx.combatClass || dist <= 0.0f)
    {
        return false;
    }

    GameSnapshot moved = s;
    moved.player.isShooting = false;
    moved.player.isAutoAttacking = false;
    const auto it = std::find_if(moved.units.begin(), moved.units.end(), [&](const UnitState& u) { return u.guid == unit.guid; });

    if (it == moved.units.end())
    {
        return false;
    }

    it->position = s.player.position + (unit.position - s.player.position) * (d / dist);
    auto cc = ctx.combatContext(moved);
    return ctx.combatClass->wantsCombat(cc);
}

// Would a pull rule fire with the mob closer? Only then does walking in help.
// Mind Blast on its cooldown or held back after "Out of range", or too little
// mana, fires nowhere: walking in anyway took a caster onto the mob, and a mob
// wandering off turned that into a stop-start chase at its heels. A few
// distances, since a ranged pull can have a minimum range (Auto Shot's 8).
bool pullFiresCloser(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float dist)
{
    const float floor = cfgPullFloor(ctx);
    if (dist <= floor)
    {
        return false;
    }

    const bool rangedClass = ctx.combatClass && ctx.combatClass->settings().combatRange > 5.0f;

    for (const float d : {floor, 5.0f, 10.0f, 20.0f})
    {
        if (d >= dist)
        {
            break;
        }

        // A ranged class (combatRange > 5.0) should not walk into melee (<= 5 yards)
        // from afar (dist >= 8.0) just to satisfy a melee fallback rule.
        if (rangedClass && dist >= 8.0f && d <= 5.0f)
        {
            continue;
        }

        if (pullFiresAt(unit, s, ctx, d))
        {
            return true;
        }
    }

    return false;
}

} // namespace

bool CombatBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (s.isDead())
    {
        return false;
    }

    if (!s.attackers().empty())
    {
        // When a quest mob was killed and needs looting for a quest item,
        // yield to LootBehavior first even if adds/attackers are hitting us.
        if (ctx.hasQuestLootNearby(s))
        {
            return false;
        }

        return true; // always defend
    }

    // Followed: finish a fight under way, but start none.
    if (ctx.followerHold != 0 && !s.inCombat())
    {
        return false;
    }

    // Travelling somewhere on purpose: an engaged target that is not actually
    // attacking us must not pull us off the route - unless it is a mob in the
    // way we chose to fight before it came for us (ClearPathBehavior).
    if (ctx.combat.engaged == 0)
    {
        return false;
    }

    if (ctx.combat.defendOnly && ctx.combat.engaged != ctx.combat.clearing)
    {
        return false;
    }

    const UnitState* u = s.findUnit(ctx.combat.engaged);
    return u && u->isAlive() && u->reaction != Reaction::Friendly && !ctx.targetBlacklist.contains(u->guid);
}

void CombatBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    pullGuid_ = 0;
    pullChase_ = false;
    fightChase_ = false;
    pullActionExecuted_ = false;
    autoRepeatStartedAt_.reset();
    wandShooting_ = false;
    wandStoppedByStep_ = false;
    lastFightPosition_.reset();
    const bool defending = s.inCombat();
    pullFinished_ = defending;
    inPullRange_ = false;
    tabTarget_ = 0;
    tabAttempts_ = 0;

    if (!defending && ctx.combatClass)
    {
        ctx.combatClass->resetPull();
    }
}

void CombatBehavior::onExit(BotContext& ctx)
{
    pullFinished_ = false;
    pullActionExecuted_ = false;
    autoRepeatStartedAt_.reset();
    deadZoneIn_.reset();
    deadZoneBackFrom_.reset();

    if (ctx.combatClass)
    {
        ctx.combatClass->resetPull();
    }

    ctx.navigator.stop();
    stopBacking(ctx);
    stopDodging(ctx);
    stopScriptMove(ctx);
    ctx.scriptMove.reset();
    // A look for a mob under the mouse left half way starts over: taken up
    // later for the same mob, it went on from where it was and gave up early.
    ctx.stopMouseSeek();
    clickTried_ = false;
    clickSince_ = TimePoint{};
}

Guid CombatBehavior::chooseTarget(const GameSnapshot& s, BotContext& ctx) const
{
    const auto attackers = s.attackers();

    if (!attackers.empty())
    {
        // One fighting us, not running from us: while the target flees, a mob
        // hitting us is fought rather than the runner chased.
        const auto fighting = [&](const UnitState* u) { return u && u->isAlive() && s.isAttacker(*u) && !u->isFleeing; };

        // The one that fled is back: finished first, then the rest.
        if (const UnitState* back = s.findUnit(fled_); fighting(back))
        {
            return back->guid;
        }

        // Keep hitting what we are hitting, then what we planned to hit, then the best attacker.
        if (const UnitState* t = s.target(); fighting(t))
        {
            return t->guid;
        }

        if (const UnitState* e = s.findUnit(ctx.combat.engaged); fighting(e))
        {
            return e->guid;
        }

        if (const UnitState* best = ctx.targets.selectAttacker(s); fighting(best))
        {
            return best->guid;
        }

        for (const UnitState* u : attackers)
        {
            if (fighting(u))
            {
                return u->guid;
            }
        }

        // Only the runner: after it.
        if (const UnitState* t = s.target(); t && t->isAlive() && s.isAttacker(*t))
        {
            return t->guid;
        }

        if (const UnitState* best = ctx.targets.selectAttacker(s))
        {
            return best->guid;
        }
    }

    if (const UnitState* e = s.findUnit(ctx.combat.engaged); e && e->isAlive() && e->reaction != Reaction::Friendly)
    {
        return e->guid;
    }

    if (!attackers.empty())
    {
        return attackers.front()->guid;
    }

    return 0;
}

BehaviorStatus CombatBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (const UnitState* e = s.findUnit(ctx.combat.engaged); e && e->isAlive() && e->isFleeing)
    {
        if (const UnitState* f = s.findUnit(fled_); !f || !f->isAlive())
        {
            fled_ = e->guid;
        }
    }
    else if (const UnitState* f = s.findUnit(fled_); !f || !f->isAlive())
    {
        fled_ = 0;
    }

    const Guid desired = chooseTarget(s, ctx);
    const UnitState* unit = s.findUnit(desired);

    if (!unit || unit->reaction == Reaction::Friendly)
    {
        ctx.navigator.stop();
        ctx.combat.engaged = 0;
        return BehaviorStatus::Failure;
    }

    if (desired != ctx.combat.engaged)
    {
        ctx.log.info(logcat::Target, "engaging {} {}", unit->name, formatGuid(unit->guid));
        stopBacking(ctx);
        ctx.combat.engaged = desired;
        pullGuid_ = 0;
        pullActionExecuted_ = false;
        autoRepeatStartedAt_.reset();
        const bool defending = s.inCombat() || s.isAttacker(*unit);
        pullFinished_ = defending;

        if (!defending && ctx.combatClass)
        {
            ctx.combatClass->resetPull();
        }
    }

    // Someone else tagged it after we chose it: no loot and no experience for
    // us. Leave it for good, unless it is fighting us. Once we are hitting it,
    // only when that holds a while: the tag the client shows can come after
    // the first swing, and a mob that turned on another player meanwhile was
    // left - ours after all - and came after the bot while it went looting.
    if (unit->isTappedByOther && !s.isAttacker(*unit))
    {
        if (tappedGuid_ != unit->guid)
        {
            tappedGuid_ = unit->guid;
            tappedSince_ = ctx.now();
        }

        const bool fightingIt = s.inCombat() && (pullActionExecuted_ || pullFinished_);

        if (!fightingIt || ctx.now() - tappedSince_ >= kTapSettle)
        {
            ctx.navigator.stop();
            ctx.blacklistTarget(unit->guid, unit->name, "tagged by another player");
            return BehaviorStatus::Failure;
        }
    }
    else if (tappedGuid_ == unit->guid)
    {
        tappedGuid_ = 0;
    }

    // Closing in on a mob without getting nearer in height: it is under the
    // ground, in a mine below or on another floor. Arrival is judged on the
    // level, so the bot would stand on top of it until the approach timed out.
    // Something attacking us is fought wherever it is.
    if (!pullActionExecuted_ && !s.isAttacker(*unit) && ctx.targets.tooSteep(*unit, s))
    {
        ctx.navigator.stop();
        ctx.blacklistTarget(unit->guid, unit->name, "under the ground");
        ctx.combat.engaged = 0;
        return BehaviorStatus::Failure;
    }

    // On the way to it, or about to tag it: mobs may have wandered in since it
    // was chosen. Not once the fight is on.
    if (!s.inCombat() && !s.isAttacker(*unit) && !unit->inCombat && ctx.avoidPacked(s, *unit))
    {
        ctx.navigator.stop();
        ctx.combat.engaged = 0;
        return BehaviorStatus::Failure;
    }

    // The only way to it goes through a pack's reach, or a strong mob's: a
    // Defias Looter in a house whose door a pack stood by. Left alone for a
    // while rather than walked to through them.
    if (!s.inCombat() && !s.isAttacker(*unit) && !unit->inCombat && !pullActionExecuted_)
    {
        if (const auto zone = ctx.wayThroughDanger(s, *unit))
        {
            ctx.navigator.stop();
            ctx.blacklistTarget(unit->guid, unit->name, std::format("the way to it goes through the reach of {}", zone->name.empty() ? std::string("mobs kept clear of") : zone->name), Duration(300000));
            ctx.combat.engaged = 0;
            return BehaviorStatus::Failure;
        }
    }

    // Still on the way to it, nothing pulled yet: a mob that has wandered in
    // between is fought first, rather than walked into.
    if (!s.inCombat() && !s.isAttacker(*unit) && !unit->inCombat && !pullActionExecuted_ && !pullFinished_ && s.attackers().empty())
    {
        if (const UnitState* nearer = ctx.targets.standingInTheWay(s, *unit); nearer && !ctx.avoidPacked(s, *nearer))
        {
            ctx.log.info(logcat::Target, "{} {} (level {}) stands in the way to {}, {:.0f} yd off - fighting it first", nearer->name, formatGuid(nearer->guid), nearer->level, unit->name, distance(s.player.position, nearer->position));
            ctx.navigator.stop();
            ctx.combat.engaged = nearer->guid;
            ctx.combat.clearing = nearer->guid;
            return BehaviorStatus::Running;
        }
    }

    if (s.targetGuid != desired)
    {
        return acquire(*unit, s, ctx);
    }

    tabAttempts_ = 0;

    auto cc = ctx.combatContext(s);
    const bool inCombat = s.inCombat() || unit->inCombat || s.isAttacker(*unit);
    const bool busy = s.player.isCasting || ctx.actions.isCasting() || ctx.actions.onGlobalCooldown();
    const bool wantsMorePull = ctx.combatClass && ctx.combatClass->wantsPull(cc);

    constexpr Duration kPullToCombatDelay{100};
    const bool pullDelayPassed = !pullActionExecuted_ || (ctx.now() - pullActionAt_ >= kPullToCombatDelay);

    // If combat has begun, all matching pull conditions have executed, and 100ms has passed since the pull action:
    if (inCombat && !wantsMorePull && !busy && pullDelayPassed && ctx.combat.engaged == unit->guid)
    {
        pullFinished_ = true;
    }

    // A runner can drop both its target and combat flag. Keep fighting our
    // engaged runner instead of switching to the pull rules (e.g. Charge).
    // If pull is finished, unit is fleeing, or we never engaged this unit to pull (e.g. defending add):
    const bool mustFight = pullDelayPassed && (unit->isFleeing || pullFinished_ || (ctx.combat.engaged != unit->guid));

    if (!mustFight)
    {
        return pull(*unit, s, ctx);
    }

    pullGuid_ = 0;
    return fight(*unit, s, ctx);
}

BehaviorStatus CombatBehavior::acquire(const UnitState& unit, const GameSnapshot& s, BotContext& ctx)
{
    if (unit.reaction == Reaction::Friendly)
    {
        ctx.combat.engaged = 0;
        return BehaviorStatus::Failure;
    }

    // We cannot select a specific GUID with keys; if the game already has a
    // live attacker targeted, fight that one instead - not one running from
    // us, which chooseTarget left for one hitting us: kept, the bot stood by
    // while that one hit it until the runner came back. Nor when the runner
    // is back: it is Tabbed to, and finished first.
    if (const UnitState* t = s.target(); t && t->isAlive() && s.isAttacker(*t) && !t->isFleeing && unit.guid != fled_)
    {
        ctx.combat.engaged = t->guid;
        return BehaviorStatus::Running;
    }

    // If the client has a stray live target that is not attacking us (e.g. auto-targeted
    // after a kill or from an earlier tab), clear it first. Leaving it targeted makes
    // Target.Next (Tab) cycle distant mobs in front of the camera instead of targeting
    // the nearest attacker hitting us.
    if (const UnitState* t = s.target(); t && t->isAlive() && !s.isAttacker(*t) && t->guid != unit.guid)
    {
        ctx.log.info(logcat::Target, "clearing stray target {} {} before acquiring attacker {}", t->name, formatGuid(t->guid), unit.name);
        ctx.actions.execute(actions::TargetClear);
        return BehaviorStatus::Running;
    }

    // A party follower takes its leader's target the way a player does:
    // /assist <leader>. Tab could take another mob of the group fought.
    if (ctx.partyFollower())
    {
        if (const PartyMember* leader = ctx.party.leader(); leader && leader->inSight && leader->target == unit.guid)
        {
            constexpr Duration kAssistEvery{1500};
            const TimePoint now = ctx.now();

            if (!lastAssist_ || now - *lastAssist_ >= kAssistEvery)
            {
                lastAssist_ = now;
                ctx.actions.chat("/assist " + leader->name);
            }

            return BehaviorStatus::Running;
        }
    }

    const bool targetRequested = ctx.setTarget(unit.guid);

    // Keep steering the prepared approach while the client applies selection.
    // Returning here left the old route's keys held without following the new one.
    if (tabTarget_ != unit.guid)
    {
        ctx.stopMouseSeek();
        tabTarget_ = unit.guid;
        tabAttempts_ = 0;
        acquireWalking_ = false;
        clickTried_ = false;
        clickSince_ = TimePoint{};
    }

    const float dist = distance(s.player.position, unit.position);
    // Tab first; where Tab did not take it, a click on it - on a client that
    // says what is under the mouse - before the mob is given up.
    const bool clicking = ctx.config.clickToSelect && !clickTried_ && !targetRequested && tabAttempts_ >= ctx.config.tabAttempts && ctx.provider && ctx.provider->mouseoverGuid().has_value();
    const CombatClassSettings settings = ctx.combatClass ? ctx.combatClass->settings() : CombatClassSettings{};
    // Target.Next (Tab) only reaches units in front of us up to the client's
    // reach: some 20 yards on the old clients, 40 on the 64-bit ones - where
    // a hunter walked in to 16 yd to Tab a mob its Auto Shot reached at 35.
    auto cc = ctx.combatContext(s);
    const float fightingRange = ctx.combatClass ? ctx.combatClass->combatRange(cc) : settings.combatRange;
    const float pullReach = ctx.combatClass ? ctx.combatClass->pullReach(cc) : 5.0f;
    const float tabReach = ctx.provider ? ctx.provider->tabTargetReach() : 20.0f;
    const float maxAcquireDist = std::min(tabReach, unit.isFleeing ? fightingRange : pullReach);
    const bool hasAggro = !unit.isFleeing && (s.inCombat() || s.isAttacker(unit) || unit.inCombat || (unit.targetGuid == s.player.guid));
    // When having aggro or when Tab repeatedly fails (e.g. mob behind an obstacle/dummy doll),
    // close in towards melee floor (1.5 yd) so line of sight clears and Tab can select the mob.
    // Short of Tab's reach by a fifth, its edge being uncertain - not of the
    // pull's: a mob at 33 yd is Tabbed from about 32, not walked in to 28.
    const float tabFrom = std::min(maxAcquireDist, tabReach * 0.8f);
    // One already at us is in melee reach as it is: turned to and tabbed, not
    // walked in to 1.5 yards - it hit from 2.5 and stepped about, and the bot
    // ran round it in circles, never in front of it to Tab, until it died.
    // In to the floor only when Tab has failed a couple of times.
    const float floor = cfgPullFloor(ctx);
    const float melee = cfgMeleeReach(ctx);
    const float chase = cfgChaseMargin(ctx);
    const float aggroApproach = tabAttempts_ >= 2 ? floor : melee - 1.0f;
    const float approachTarget = hasAggro ? aggroApproach : std::max(floor, unit.isFleeing ? maxAcquireDist * 0.8f : tabFrom * std::max(0.25f, 1.0f - tabAttempts_ * 0.19f));

    if (faceWhileCasting(unit, s, ctx, 0.3f))
    {
        return BehaviorStatus::Running;
    }

    // Out of sight - behind a wall, upstairs - it is walked to until it is in
    // sight, however near: Tab does not take what the character cannot see.
    const bool outOfSight = !hasAggro && dist > melee && walkForSight(unit, s, ctx, dist);
    // Once walking, on in to the chase margin before stopping: a whelp
    // wandering off at the edge of Tab's reach was followed a step at a time,
    // Forward pressed and let go every few tenths of a second.
    // A runner is caught at melee reach as it is, without the margin.
    const float margin = unit.isFleeing ? approachTarget : std::max(floor, approachTarget * chase);

    if (dist > maxAcquireDist || outOfSight)
    {
        acquireWalking_ = true;

        // Once a second, and at once for a new target: every tick filled the log.
        if (unit.guid != takingSaidOf_ || ctx.now() - takingSaidAt_ >= Duration(1000))
        {
            takingSaidOf_ = unit.guid;
            takingSaidAt_ = ctx.now();
            ctx.log.trace(logcat::Move, "taking the target: distance={:.1f} past Tab's reach {:.1f}{}", dist, maxAcquireDist, outOfSight ? ", out of sight" : "");
        }

        // In to the margin, not to the edge of the reach: the way is judged
        // on the map (2D) and the reach by the slope (3D), and on a hillside
        // the bot stopped 30.2 yd off for a reach of 30, stepping and
        // stopping as the mob walked off.
        if (ctx.navigator.moveTo(s, unit.position, outOfSight ? floor : margin) == NavStatus::Unreachable)
        {
            ctx.navigator.stop();
            ctx.blacklistTarget(unit.guid, unit.name, "unreachable", seconds(300));
            return BehaviorStatus::Failure;
        }

        return BehaviorStatus::Running;
    }

    // Still short of where the approach stops: walk on in while the target is
    // taken. Halting at the edge of Tab's reach, then walking on once it was
    // taken, stopped the bot for a moment at every pull.
    const bool walkOn = dist > (acquireWalking_ ? margin : approachTarget);
    acquireWalking_ = walkOn;
    ctx.log.trace(logcat::Move, "taking the target: distance={:.1f} walking on={} to {:.1f} (stops at {:.1f}) tabbed {} times", dist, walkOn, margin, approachTarget, tabAttempts_);

    if (walkOn)
    {
        if (ctx.navigator.moveTo(s, unit.position, margin) == NavStatus::Unreachable)
        {
            ctx.navigator.stop();
            ctx.blacklistTarget(unit.guid, unit.name, "unreachable", seconds(300));
            return BehaviorStatus::Failure;
        }
    }
    else
    {
        ctx.navigator.halt();
    }

    if (targetRequested)
    {
        if (!walkOn)
        {
            ctx.movement.stopTurning();
        }

        return BehaviorStatus::Running;
    }

    // Tab takes what is in front: walking, the navigator does the turning.
    if (walkOn ? std::abs(headingDelta(s.player.heading, s.player.position, unit.position)) > 0.3f : !ctx.movement.face(s.player, unit.position, 0.3f))
    {
        return BehaviorStatus::Running;
    }

    const TimePoint now = ctx.now();

    if (clicking)
    {
        // A few seconds of looking for it under the mouse, from the middle of
        // the screen out: what faces us is found first.
        constexpr Duration kClickLooking{3000};

        if (clickSince_ == TimePoint{})
        {
            clickSince_ = now;
        }

        const auto seek = now - clickSince_ < kClickLooking ? ctx.seekUnderMouse(unit.guid, s.player, unit.position, 1.0f) : BotContext::MouseSeek::NotFound;

        if (seek == BotContext::MouseSeek::Searching)
        {
            return BehaviorStatus::Running;
        }

        ctx.stopMouseSeek();
        clickTried_ = true;

        if (seek == BotContext::MouseSeek::Found)
        {
            ctx.actions.click(MouseButton::Left);
            ctx.log.debug(logcat::Target, "clicked {} {} to take it ({:.0f} yd)", unit.name, formatGuid(unit.guid), dist);
            lastTab_ = now; // given up, if it did not take it, after as long as a Tab
            return BehaviorStatus::Running;
        }

        ctx.log.debug(logcat::Target, "{} not found under the mouse either", unit.name);
        return BehaviorStatus::Running;
    }

    if (lastTab_ && now - *lastTab_ < ctx.config.tabInterval)
    {
        return BehaviorStatus::Running;
    }

    if (!walkOn && !hasAggro && tabAttempts_ >= ctx.config.tabAttempts)
    {
        ctx.blacklistTarget(unit.guid, unit.name, "cannot select it with Target.Next");
        tabAttempts_ = 0;
        return BehaviorStatus::Failure;
    }

    if (tabAttempts_ >= ctx.config.tabAttempts)
    {
        tabAttempts_ = 0;
    }

    ctx.actions.execute(actions::TargetNext);
    ++tabAttempts_;
    lastTab_ = now;
    return BehaviorStatus::Running;
}

BehaviorStatus CombatBehavior::pull(const UnitState& unit, const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();
    const float dist = distance(s.player.position, unit.position);

    if (pullGuid_ != unit.guid)
    {
        pullGuid_ = unit.guid;
        pullFinished_ = false;
        pullStarted_ = now;
        inPullRange_ = false;
        pullWalking_ = false;
        pullChase_ = false;
        pullCloseTo_.reset();
        pullActionExecuted_ = false;
        autoRepeatStartedAt_.reset();

        if (ctx.combatClass)
        {
            ctx.combatClass->resetPull();
        }

        ctx.log.info(logcat::Combat, "pull {} {} distance={:.1f}", unit.name, formatGuid(unit.guid), dist);
    }

    // Stop as soon as a pull rule would actually fire. Farther out than any
    // pull spell reaches (their ranges, pullReach) the bot walks in regardless.
    auto pullCtx = ctx.combatContext(s);
    const bool readyToPull = ctx.combatClass && ctx.combatClass->wantsPull(pullCtx);
    const float pullReach = ctx.combatClass ? ctx.combatClass->pullReach(pullCtx) : 5.0f;

    // Within reach is not the same as the rule firing: it can ask for less
    // ("Mage.Frostbolt if target.distance <= 25" with a 30 yd Frostbolt), and
    // standing there waits for the mob to wander in, or for the pull timeout.
    // So keep closing while no rule fires - except while a cast or the global
    // cooldown is what holds it back, when walking in only draws aggro.
    const bool outsidePullRange = dist > pullReach;
    const bool busy = s.player.isCasting || ctx.actions.isCasting() || ctx.actions.onGlobalCooldown() || s.player.isShooting || (autoRepeatStartedAt_ && now - *autoRepeatStartedAt_ < Duration(800));
    const float floor = cfgPullFloor(ctx);
    const float melee = cfgMeleeReach(ctx);
    const float chase = cfgChaseMargin(ctx);
    const float overshoot = cfgPullOvershoot(ctx);

    // Auto attack already on (switched on by hand, or by a rule from farther
    // out) swings only in melee, and its rule no longer fires to say so.
    const bool swingingFromAfar = s.player.isAutoAttacking && dist > melee;
    // Out of reach: chase in to the margin, even past where a rule would
    // start to fire, rather than stop on the edge of its range.
    const float chaseStop = std::max(floor, pullReach * chase);

    if (outsidePullRange)
    {
        pullChase_ = true;
    }
    else if (dist <= chaseStop && pullChase_)
    {
        // There with the margin already: no further step past where a rule fires.
        pullChase_ = false;
        pullWalking_ = false;
    }

    const bool drewNothing = pullActionExecuted_ && !busy && !s.inCombat() && !unit.inCombat && !s.isAttacker(unit) && now - pullActionAt_ >= kPullDrewNothing;
    const bool canCloseCloser = (!pullActionExecuted_ || drewNothing) && pullFiresCloser(unit, s, ctx, dist);
    // Out of sight a ranged pull does nothing ("not in line of sight"): in
    // until the mob is in sight, a few yards at a time.
    const bool outOfSight = !busy && !s.inCombat() && !unit.inCombat && dist > melee && walkForSight(unit, s, ctx, dist);

    if (pullChase_ || outOfSight || (!readyToPull && (outsidePullRange || (!busy && (swingingFromAfar || canCloseCloser)))))
    {
        inPullRange_ = false;
        pullWalking_ = true;
        pullCloseTo_.reset();

        if (now - pullStarted_ > ctx.config.approachTimeout)
        {
            ctx.navigator.stop();
            ctx.blacklistTarget(unit.guid, unit.name, "approach timeout");
            return BehaviorStatus::Failure;
        }

        // Outside: close to the pull reach. Inside: a few yards at a time, re-checking
        // the pull rules every tick so the walk ends the moment one fires.
        const float stopAt = pullChase_ ? chaseStop : std::max(floor, dist - 3.0f);
        const NavStatus st = ctx.navigator.moveTo(s, unit.position, stopAt);

        if (st == NavStatus::Unreachable)
        {
            ctx.blacklistTarget(unit.guid, unit.name, "unreachable", seconds(300));
            return BehaviorStatus::Failure;
        }

        ctx.log.trace(logcat::Move, "approach distance={:.1f}", dist);
        return BehaviorStatus::Running;
    }

    // A rule started to fire on the way in: carry on a little past that, as
    // long as it would still fire there (a minimum range can stop it).
    if (readyToPull && pullWalking_)
    {
        pullWalking_ = false;
        const float closeTo = dist * overshoot;

        if (closeTo > floor && pullFiresAt(unit, s, ctx, closeTo))
        {
            pullCloseTo_ = closeTo;
        }
    }

    if (pullCloseTo_ && dist > *pullCloseTo_ && now - pullStarted_ <= ctx.config.approachTimeout)
    {
        inPullRange_ = false;

        if (ctx.navigator.moveTo(s, unit.position, *pullCloseTo_) != NavStatus::Unreachable)
        {
            ctx.log.trace(logcat::Move, "approach distance={:.1f} to {:.1f}", dist, *pullCloseTo_);
            return BehaviorStatus::Running;
        }

        ctx.navigator.stop();
        ctx.blacklistTarget(unit.guid, unit.name, "unreachable", seconds(300));
        return BehaviorStatus::Failure;
    }

    pullCloseTo_.reset();

    if (!inPullRange_)
    {
        inPullRange_ = true;
        inPullRangeSince_ = now;
        ctx.navigator.halt();
    }

    if (now - inPullRangeSince_ > ctx.config.pullTimeout)
    {
        ctx.blacklistTarget(unit.guid, unit.name, "pull timeout");
        return BehaviorStatus::Failure;
    }

    // Each pull rule goes off once. One that did nothing - Auto Shot pressed
    // the moment the bot stopped after a gathering detour, still moving - left
    // no rule to fire: the bot stood there with the mob targeted until the
    // pull timeout, and then did the same to the next one. Not shooting, not
    // casting and the mob still not after us a while later: pull again. The
    // while counts from the end of the cast, so a slow spell can land first.
    if (pullActionExecuted_ && busy)
    {
        pullActionAt_ = now;
    }

    if (pullActionExecuted_ && !readyToPull && !busy && !s.inCombat() && !unit.inCombat && !s.isAttacker(unit) && now - pullActionAt_ >= kPullNoEffect)
    {
        ctx.log.info(logcat::Combat, "the pull on {} did nothing - pulling again", unit.name);
        pullActionExecuted_ = false;
        autoRepeatStartedAt_.reset();

        if (ctx.combatClass)
        {
            ctx.combatClass->resetPull();
        }

        return BehaviorStatus::Running;
    }

    if (faceWhileCasting(unit, s, ctx, 0.35f))
    {
        return BehaviorStatus::Running;
    }

    // Pulling straight out of a rest: the pull spell fails while still sitting.
    if (ctx.standUp(s))
    {
        return BehaviorStatus::Running;
    }

    if (!ctx.movement.face(s.player, unit.position, 0.35f))
    {
        return BehaviorStatus::Running;
    }

    if (ctx.combatClass)
    {
        auto cc = ctx.combatContext(s);

        if (ctx.combatClass->onPull(cc))
        {
            pullActionExecuted_ = true;
            pullActionAt_ = now;

            if (ctx.actions.lastAction())
            {
                const std::string_view act = *ctx.actions.lastAction();

                if (act.ends_with(".AutoShot") || act.ends_with(".Shoot"))
                {
                    autoRepeatStartedAt_ = now;
                    wandShooting_ = act.ends_with(".Shoot");
                    wandStoppedByStep_ = false;
                }
            }
        }
    }

    return BehaviorStatus::Running;
}

// Nothing in the rotation fires at this distance, though something would
// nearer or farther: a hunter 6 yards from a mob hitting its pet - Raptor
// Strike reaches 5, Auto Shot starts at 8 - stood there doing nothing for the
// whole fight. A mob on the pet is stepped back from, out to where the shots
// fire; one on us that does not come in is walked in to, to hit in melee.
void CombatBehavior::leaveDeadZone(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float dist)
{
    // Not every tick: a step takes a moment, and the mob moves meanwhile.
    constexpr Duration kDeadZoneEvery{1500};
    // In to well within melee reach; out to a couple of yards past a minimum
    // range (Auto Shot's 8, about 11 with the bodies), so a mob stepping
    // after the pet does not take the hunter back into the gap at once.
    constexpr float kInTo = 3.0f;
    constexpr std::array<float, 3> kOutTo{8.0f + kBodiesReach + 2.0f, 8.0f + kBodiesReach + 4.0f, 8.0f + kBodiesReach + 7.0f};
    const TimePoint now = ctx.now();

    if (dist <= cfgMeleeReach(ctx) || deadZoneIn_ || scriptMove_ || ctx.scriptMove || (deadZoneOf_ == unit.guid && now - deadZoneAt_ < kDeadZoneEvery))
    {
        return;
    }

    // The client may be in the middle of starting Auto Shot.
    if (autoRepeatStartedAt_ && now - *autoRepeatStartedAt_ < Duration(800))
    {
        return;
    }

    if (combatFiresAt(unit, s, ctx, dist))
    {
        return;
    }

    const bool onUs = unit.targetGuid == s.player.guid;

    // Coming for us: it arrives on its own.
    if (onUs && targetClosingIn(unit, s, now))
    {
        return;
    }

    const bool firesNearer = combatFiresAt(unit, s, ctx, kInTo);
    std::optional<float> firesFrom;

    for (const float d : kOutTo)
    {
        if (d > dist && combatFiresAt(unit, s, ctx, d))
        {
            firesFrom = d;
            break;
        }
    }

    // The last step back went nowhere (a wall behind): in instead.
    const bool backBlocked = deadZoneOf_ == unit.guid && deadZoneBackFrom_ && distance2D(*deadZoneBackFrom_, s.player.position) < 1.0f;
    const bool goBack = firesFrom && !backBlocked && (!onUs || !firesNearer);

    if (!goBack && !firesNearer)
    {
        return;
    }

    deadZoneOf_ = unit.guid;
    deadZoneAt_ = now;
    deadZoneBackFrom_.reset();

    if (goBack)
    {
        const float yards = *firesFrom - dist + 0.5f;
        ctx.log.info(logcat::Combat, "{} {:.1f} yd off: too near to shoot, too far to hit - stepping back {:.0f} yd", unit.name, dist, yards);
        deadZoneBackFrom_ = s.player.position;
        ctx.scriptMove = ScriptMove{ScriptMove::Way::Backward, yards, std::format("out of the gap between melee and shooting: back {:.0f} yd", yards)};
        return;
    }

    ctx.log.info(logcat::Combat, "{} {:.1f} yd off: too near to shoot, too far to hit - walking in to melee", unit.name, dist);
    deadZoneIn_ = kInTo;
}

BehaviorStatus CombatBehavior::fight(const UnitState& unit, const GameSnapshot& s, BotContext& ctx)
{
    if (s.player.isCasting || ctx.actions.isCasting())
    {
        stopBacking(ctx);
    }

    if (faceWhileCasting(unit, s, ctx, 0.5f))
    {
        return BehaviorStatus::Running;
    }

    if (ctx.standUp(s))
    {
        return BehaviorStatus::Running;
    }

    // A step - a dodge from Add Avoidance, a scripted step - stops the wand's
    // Shoot in the game, while the client still says it shoots: the rule was
    // held back for good ("pressing Shoot again turns it off"), and the bot
    // stood there doing nothing. After a step it counts as stopped, and is
    // pressed again. (Auto Shot only waits for the step to end.)
    const bool stepped = s.player.isMoving || (lastFightPosition_ && distance2D(*lastFightPosition_, s.player.position) > 0.3f);
    lastFightPosition_ = s.player.position;

    if (!s.player.isShooting)
    {
        wandShooting_ = false;
        wandStoppedByStep_ = false;
    }
    else if (wandShooting_ && stepped)
    {
        if (!wandStoppedByStep_)
        {
            ctx.log.debug(logcat::Combat, "moved while the wand shot: Shoot is off in the game - pressed again");
        }

        wandStoppedByStep_ = true;
    }

    GameSnapshot shooting;
    const GameSnapshot* view = &s;

    if (wandStoppedByStep_)
    {
        shooting = s;
        shooting.player.isShooting = false;
        view = &shooting;
    }

    auto cc = ctx.combatContext(*view);
    const float combatRange = ctx.combatClass ? ctx.combatClass->combatRange(cc) : CombatClassSettings{}.combatRange;
    const float dist = distance(s.player.position, unit.position);
    // Once a second, and at once for a new target: every tick filled the log.
    if (unit.guid != hpSaidOf_ || ctx.now() - hpSaidAt_ >= Duration(1000))
    {
        hpSaidOf_ = unit.guid;
        hpSaidAt_ = ctx.now();
        ctx.log.trace(logcat::Combat, "target hp={:.0f}% distance={:.1f}", unit.healthPercent(), dist);
    }

    // A mob running at us arrives on its own. Walking to meet it cuts the
    // ranged rotation short (moving stops Auto Shot and casts) for nothing.
    // Going in for an auto attack swing, there is nothing to cut short.
    const bool swinging = ctx.combatClass && ctx.combatClass->wantsMeleeSwing(cc);
    // Out of range: chase in to the margin, not just back over the edge, or a
    // target walking away is followed a step at a time.
    const float chaseStop = std::max(1.0f, combatRange * cfgChaseMargin(ctx));

    if (dist > combatRange)
    {
        fightChase_ = true;
    }
    else if (dist <= chaseStop)
    {
        fightChase_ = false;
    }

    // Away from a mob too strong to fight, or one that would join the fight,
    // before anything else: the mob being fought comes along. The rotation waits - a cast would stop the step.
    // After a runner, only from one too strong: stepping away from an add
    // and running on after it again dragged the bot back and forth.
    if (dodgeMobs(unit, s, ctx, !unit.isFleeing))
    {
        return BehaviorStatus::Running;
    }

    // A step the script asked for: walked out before anything else, the
    // rotation waiting (a cast would stop it).
    if (takeScriptMove(unit, s, ctx))
    {
        return BehaviorStatus::Running;
    }

    const bool busyFighting = view->player.isShooting || s.player.isCasting || ctx.actions.isCasting() || (autoRepeatStartedAt_ && ctx.now() - *autoRepeatStartedAt_ < Duration(800));

    // Not while backing up: the step takes the target just out of reach, and
    // walking in to it again ended the step, to back again at once - back,
    // forward, back, a jerk several times a second.
    if (fightChase_ && !busyFighting && !backing_ && (swinging || !targetClosingIn(unit, s, ctx.now())))
    {
        ctx.navigator.moveTo(s, unit.position, chaseStop);
        return BehaviorStatus::Running;
    }

    // Walking in out of the gap between melee and shooting (leaveDeadZone).
    if (deadZoneIn_ && (deadZoneOf_ != unit.guid || dist <= *deadZoneIn_ || ctx.now() - deadZoneAt_ > Duration(4000) || combatFiresAt(unit, *view, ctx, dist)))
    {
        deadZoneIn_.reset();
    }

    if (deadZoneIn_ && !s.player.isCasting && !ctx.actions.isCasting() && !backing_)
    {
        ctx.navigator.moveTo(s, unit.position, *deadZoneIn_);
        return BehaviorStatus::Running;
    }

    ctx.navigator.halt();

    // Backing up, the rotation waits: a cast would stop it, or fail moving.
    if (backFromBehind(unit, s, ctx))
    {
        return BehaviorStatus::Running;
    }

    ctx.movement.face(s.player, unit.position, 0.5f);

    // Abilities need the target in front (WoW: 180 degree arc).
    const float delta = std::fabs(headingDelta(s.player.heading, s.player.position, unit.position));

    bool fired = false;

    if (delta < 1.2f && ctx.combatClass)
    {
        if (ctx.combatClass->onCombat(cc))
        {
            fired = true;

            if (ctx.actions.lastAction())
            {
                const std::string_view act = *ctx.actions.lastAction();

                if (act.ends_with(".AutoShot") || act.ends_with(".Shoot"))
                {
                    autoRepeatStartedAt_ = ctx.now();
                    wandShooting_ = act.ends_with(".Shoot");
                    wandStoppedByStep_ = false;
                }
            }
        }
    }

    if (!fired && delta < 1.2f && !s.player.isCasting && !ctx.actions.isCasting() && !ctx.actions.onGlobalCooldown())
    {
        leaveDeadZone(unit, s, ctx, dist);
    }

    return BehaviorStatus::Running;
}

// Out of sight, walked in; in sight again, a few yards more. At the edge of
// a tree's trunk the line came and went from one tick to the next, and the
// bot walked, stopped and walked again.
bool CombatBehavior::walkForSight(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float dist)
{
    constexpr float kPastTheEdge = 4.0f;

    if (sightGuid_ != unit.guid)
    {
        sightGuid_ = unit.guid;
        sightLost_ = false;
        sightWalkTo_.reset();
    }

    if (!inSight(ctx, s.player.position, unit.position))
    {
        sightLost_ = true;
        sightWalkTo_.reset();
        return true;
    }

    if (std::exchange(sightLost_, false))
    {
        sightWalkTo_ = std::max(cfgPullFloor(ctx), dist - kPastTheEdge);
    }

    if (sightWalkTo_ && dist > *sightWalkTo_)
    {
        return true;
    }

    sightWalkTo_.reset();
    return false;
}

bool CombatBehavior::targetClosingIn(const UnitState& unit, const GameSnapshot& s, TimePoint now)
{
    // Sampled over a short window: per-tick position deltas are noise. Only
    // the target's own movement counts, so our walking cannot look like its.
    constexpr Duration kSampleWindow{400};
    constexpr float kMinGain = 0.5f; // yards per window; a running mob covers ~3

    if (unit.isFleeing)
    {
        return false;
    }

    if (approachGuid_ != unit.guid)
    {
        approachGuid_ = unit.guid;
        approachFrom_ = unit.position;
        approachSampledAt_ = now;
        // Freshly aggroed and coming for us: assume it runs in until a sample says otherwise.
        closingIn_ = unit.targetGuid != 0 && unit.targetGuid == s.player.guid;
        return closingIn_;
    }

    if (now - approachSampledAt_ >= kSampleWindow)
    {
        const float gained = distance(approachFrom_, s.player.position) - distance(unit.position, s.player.position);
        closingIn_ = gained > kMinGain;
        approachFrom_ = unit.position;
        approachSampledAt_ = now;
    }

    return closingIn_;
}

// Several mobs on us and one in melee behind: it hits where we cannot dodge,
// parry or block. A little step back takes us past it, and it comes round in
// front with the others. True while backing (the backward key held).
bool CombatBehavior::backFromBehind(const UnitState& unit, const GameSnapshot& s, BotContext& ctx)
{
    if (s.player.isCasting || ctx.actions.isCasting())
    {
        stopBacking(ctx);
        return false;
    }

    const float facing = headingTo(s.player.position, unit.position);
    int behind = 0;
    int notInFront = 0;

    for (const UnitState* a : s.attackers())
    {
        if (a->guid == unit.guid || !a->isAlive())
        {
            continue;
        }

        if (distance2D(s.player.position, a->position) > cfgMeleeReach(ctx))
        {
            continue;
        }

        const float off = std::fabs(normalizeAngle(headingTo(s.player.position, a->position) - facing));
        behind += off > kBehindAngle ? 1 : 0;
        notInFront += off > kWellInFront ? 1 : 0;
    }

    const TimePoint now = ctx.now();

    if (backing_)
    {
        if (notInFront == 0 || distance2D(s.player.position, backFrom_) >= kBackStep || now - backSince_ >= kBackLongest)
        {
            stopBacking(ctx);
            return false;
        }

        ctx.movement.setBackward(true);
        ctx.movement.faceWhileMoving(s.player, unit.position, 0.4f);
        return true;
    }

    if (behind == 0 || now < backRestUntil_)
    {
        return false;
    }

    // Facing the target first, or the step goes the wrong way.
    if (std::fabs(headingDelta(s.player.heading, s.player.position, unit.position)) > 0.6f)
    {
        return false;
    }

    // With Pathfinder: only onto ground we can walk, not off a ledge or into a wall.
    INavigationProvider& nav = ctx.navigator.provider();

    if (nav.usesClientGeometry() && !nav.canWalkStraight(s.player.position, s.player.position - directionFromHeading(s.player.heading) * kBackStep))
    {
        backRestUntil_ = now + kBackRest;
        return false;
    }

    ctx.log.info(logcat::Combat, "{} hitting us from behind - backing up to bring {} in front", behind == 1 ? std::string("a mob") : std::format("{} mobs", behind), behind == 1 ? "it" : "them");
    backing_ = true;
    backFrom_ = s.player.position;
    backSince_ = now;
    ctx.movement.setForward(false);
    ctx.movement.setStrafeLeft(false);
    ctx.movement.setStrafeRight(false);
    ctx.movement.setBackward(true);
    ctx.movement.faceWhileMoving(s.player, unit.position, 0.4f);
    return true;
}

void CombatBehavior::stopBacking(BotContext& ctx)
{
    if (!backing_)
    {
        return;
    }

    backing_ = false;
    ctx.movement.setBackward(false);
    // However it ends: not another step back at once.
    backRestUntil_ = ctx.now() + kBackRest;
}

void CombatBehavior::stopScriptMove(BotContext& ctx)
{
    if (!scriptMove_)
    {
        return;
    }

    scriptMove_.reset();
    ctx.movement.setForward(false);
    ctx.movement.setBackward(false);
    ctx.movement.setStrafeLeft(false);
    ctx.movement.setStrafeRight(false);
}

bool CombatBehavior::takeScriptMove(const UnitState& unit, const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();
    const PlayerState& p = s.player;

    // A new one, asked for by the last rotation.
    if (ctx.scriptMove && !scriptMove_)
    {
        ScriptMove move = std::move(*ctx.scriptMove);
        ctx.scriptMove.reset();
        const Vec3 forward = directionFromHeading(p.heading);
        const Vec3 left = directionFromHeading(p.heading + kPi / 2.0f);
        const Vec3 way = move.way == ScriptMove::Way::Forward ? forward : move.way == ScriptMove::Way::Backward ? forward * -1.0f : move.way == ScriptMove::Way::Left ? left : left * -1.0f;
        INavigationProvider& nav = ctx.navigator.provider();

        // Only as far as the ground goes, on the client's map: up to a wall,
        // or not at all when there is not a couple of yards.
        if (nav.usesClientGeometry())
        {
            float reach = move.yards;

            while (reach >= 2.0f && !nav.canWalkStraight(p.position, p.position + way * reach))
            {
                reach *= 0.6f;
            }

            if (reach < 2.0f)
            {
                ctx.log.info(logcat::Combat, "{}: no room that way - not stepping", move.rule);
                return false;
            }

            move.yards = reach;
        }

        ctx.log.info(logcat::Action, "{}", move.rule);
        scriptMove_ = std::move(move);
        scriptMoveFrom_ = p.position;
        scriptMoveSince_ = now;
        scriptMoveProgressAt_ = p.position;
        scriptMoveProgressSince_ = now;
    }

    if (!scriptMove_)
    {
        return false;
    }

    const float walked = distance2D(scriptMoveFrom_, p.position);
    // Walking 7 yd a second, backwards 4.5: room for that, and a turn.
    const Duration allowed = Duration(static_cast<long long>(scriptMove_->yards / 4.0f * 1500.0f) + 1000);

    if (distance2D(scriptMoveProgressAt_, p.position) >= 0.3f)
    {
        scriptMoveProgressAt_ = p.position;
        scriptMoveProgressSince_ = now;
    }

    if (walked >= scriptMove_->yards || now - scriptMoveSince_ > allowed || now - scriptMoveProgressSince_ > Duration(800))
    {
        stopScriptMove(ctx);
        return false;
    }

    const ScriptMove::Way way = scriptMove_->way;
    ctx.navigator.halt();
    ctx.movement.setForward(way == ScriptMove::Way::Forward);
    ctx.movement.setBackward(way == ScriptMove::Way::Backward);
    ctx.movement.setStrafeLeft(way == ScriptMove::Way::Left);
    ctx.movement.setStrafeRight(way == ScriptMove::Way::Right);

    // Facing the mob stays: backwards and sideways it is in front all along.
    if (way != ScriptMove::Way::Forward)
    {
        ctx.movement.faceWhileMoving(p, unit.position, 0.4f);
    }

    return true;
}

void CombatBehavior::stopDodging(BotContext& ctx)
{
    if (!dodging_)
    {
        return;
    }

    dodging_ = false;
    dodgeStrafe_ = dodgeAlong_ = 0;
    ctx.movement.setStrafeLeft(false);
    ctx.movement.setStrafeRight(false);
    ctx.movement.setBackward(false);
    ctx.movement.setForward(false);
}

bool CombatBehavior::dodgeMobs(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, bool adds)
{
    const TimePoint now = ctx.now();
    const PlayerState& p = s.player;
    // The nearest mob too strong to fight whose reach holds the character -
    // where it stands or along the way it walks (StrongMobWatch). Not one
    // already on us: there is no stepping away from that.
    const StrongMob* threat = ctx.config.avoidStrongMobs ? ctx.strongMobs.threatTo(p.position, kDodgeMargin + (dodging_ ? kDodgeClear : 0.0f), unit.guid) : nullptr;
    const bool strong = threat != nullptr;

    // Add Avoidance: the same for any other hostile mob that would notice the
    // character and join the fight - one standing that near, or walking to
    // where it will.
    if (!threat && adds && ctx.config.addAvoidance)
    {
        threat = ctx.nearMobs.threatTo(p.position, ctx.config.addAvoidanceMargin + (dodging_ ? kDodgeClear : 0.0f), unit.guid);

        // Never out of a cast for an add.
        if (threat && (p.isCasting || ctx.actions.isCasting()))
        {
            stopDodging(ctx);
            return false;
        }
    }

    // Away from where it comes nearest: its way, not only where it stands now.
    const Vec3 from = threat ? StrongMobWatch::nearestOnWay(*threat, p.position) : Vec3{};
    const float threatDistance = threat ? threat->distance : 0.0f;

    if (!threat)
    {
        stopDodging(ctx);
        return false;
    }

    if (dodging_ && now - dodgeSince_ >= kDodgeLongest)
    {
        stopDodging(ctx);
        dodgeRestUntil_ = now + kDodgeRest;
        return false;
    }

    // A cast is finished first, and after a step that led nowhere the fight
    // goes on where it stands for a while.
    if (!dodging_ && (now < dodgeRestUntil_ || p.isCasting || ctx.actions.isCasting()))
    {
        return false;
    }

    // Which keys take us away from it: sideways first (the mob being fought
    // stays in front), then back, then forward - and with Pathfinder only
    // onto ground that can be walked. Looked at again every kDodgeSteer, not
    // every tick, or the keys flip as the two mobs move.
    if (!dodging_ || now - dodgeSteeredAt_ >= kDodgeSteer)
    {
        // Standing on its way: away from the mob itself.
        const Vec3 origin = distance2D(p.position, from) > 0.5f ? from : threat->position;
        Vec3 away = p.position - origin;
        away.z = 0;
        away = away * (1.0f / std::max(0.1f, distance2D(p.position, origin)));
        const Vec3 forward = directionFromHeading(p.heading);
        const Vec3 left = directionFromHeading(p.heading + kPi / 2.0f);
        const float side = away.x * left.x + away.y * left.y;
        const float along = away.x * forward.x + away.y * forward.y;
        INavigationProvider& nav = ctx.navigator.provider();
        const auto walkable = [&](const Vec3& direction) { return !nav.usesClientGeometry() || nav.canWalkStraight(p.position, p.position + direction * kDodgeStep); };
        int strafe = 0, move = 0;

        if (std::fabs(side) >= 0.25f && walkable(left * (side > 0 ? 1.0f : -1.0f)))
        {
            strafe = side > 0 ? 1 : -1;
        }

        if (std::fabs(along) >= 0.5f && walkable(forward * (along > 0 ? 1.0f : -1.0f)))
        {
            move = along > 0 ? 1 : -1;
        }

        if (strafe == 0 && move == 0)
        {
            // Nowhere to step to: fight on.
            stopDodging(ctx);
            dodgeRestUntil_ = now + kDodgeRest;
            return false;
        }

        if (!dodging_)
        {
            dodgeSince_ = now;
            stopBacking(ctx);
        }

        dodging_ = true;
        dodgeStrafe_ = strafe;
        dodgeAlong_ = move;
        dodgeSteeredAt_ = now;
    }

    if (now - lastDodgeLog_ > Duration(5000))
    {
        const bool walking = distance2D(threat->heading(), threat->position) > 1.0f;
        ctx.log.info(logcat::Combat, "{} (level {}) is {:.0f} yd off{}, {} - stepping away from it", threat->name, threat->level > 0 ? std::to_string(threat->level) : std::string("??"), threatDistance, walking ? " and walking our way" : "", strong ? "too strong to fight" : "near enough to join the fight");
        lastDodgeLog_ = now;
    }

    // A step the script asked for gives way - before the dodge's keys, which
    // letting go of its keys afterwards took back (CodeRabbit).
    stopScriptMove(ctx);
    ctx.navigator.halt();
    ctx.movement.setStrafeLeft(dodgeStrafe_ > 0);
    ctx.movement.setStrafeRight(dodgeStrafe_ < 0);
    ctx.movement.setForward(dodgeAlong_ > 0);
    ctx.movement.setBackward(dodgeAlong_ < 0);
    ctx.movement.faceWhileMoving(p, unit.position, 0.4f);
    return true;
}

} // namespace og
