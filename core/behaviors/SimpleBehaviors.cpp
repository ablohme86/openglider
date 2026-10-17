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
 *  File: SimpleBehaviors.cpp
 *
 *  Desc:
 *  Emergency, Rest, Pull and Navigation behaviors.
 *
 *
 */
// Emergency, Rest, Pull and Navigation behaviors.
#include "og/actions/ActionBinding.h"
#include "og/core/Events.h"
#include "og/core/behaviors/Behaviors.h"
#include "og/navigation/Roaming.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>

namespace og
{

std::vector<std::unique_ptr<IBotBehavior>> createDefaultBehaviors()
{
    std::vector<std::unique_ptr<IBotBehavior>> out;
    out.push_back(std::make_unique<SurfaceBehavior>());
    out.push_back(std::make_unique<LeaveFireBehavior>());
    out.push_back(std::make_unique<EmergencyBehavior>());
    out.push_back(std::make_unique<PartyHealBehavior>());
    out.push_back(std::make_unique<DeathBehavior>());
    out.push_back(std::make_unique<CombatBehavior>());
    out.push_back(std::make_unique<ResurrectEscapeBehavior>());
    out.push_back(std::make_unique<HoldBehavior>());
    out.push_back(std::make_unique<LootBehavior>());
    out.push_back(std::make_unique<LeaveRespawnBehavior>());
    out.push_back(std::make_unique<GatherBehavior>());
    out.push_back(std::make_unique<CampfireBehavior>());
    out.push_back(std::make_unique<VendorBehavior>());
    out.push_back(std::make_unique<InventoryQuestBehavior>());
    out.push_back(std::make_unique<FlightMasterBehavior>());
    out.push_back(std::make_unique<QuestHandInBehavior>());
    out.push_back(std::make_unique<TrainerBehavior>());
    out.push_back(std::make_unique<AutoEquipBehavior>());
    out.push_back(std::make_unique<RestBehavior>());
    out.push_back(std::make_unique<FollowBehavior>());
    out.push_back(std::make_unique<PartyWaitBehavior>());
    out.push_back(std::make_unique<ClearPathBehavior>());
    out.push_back(std::make_unique<FishBehavior>());
    out.push_back(std::make_unique<PullBehavior>());
    out.push_back(std::make_unique<NavigationBehavior>());
    return out;
}

// --- LeaveFire ------------------------------------------------------------------

namespace
{

// A campfire burns within a yard of its middle or so (Northshire's, 1.60.1:
// hurt at 0.9 yd, not at 1.4); a little more, for larger fires. Left for
// twice that.
constexpr float kFireReach = 1.6f;
constexpr float kFireClear = 3.5f;

bool namesFire(std::string_view name)
{
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (const std::string_view word : {"fire", "brazier", "flame"})
    {
        if (lower.find(word) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

} // namespace

const GameObjectState* LeaveFireBehavior::fireUnderfoot(const GameSnapshot& s)
{
    const GameObjectState* best = nullptr;

    for (const auto& g : s.objects)
    {
        // Herbs (Firebloom), ore and chests are no fires.
        if (g.kind != GameObjectKind::Generic && g.kind != GameObjectKind::Unknown)
        {
            continue;
        }

        const float d = distance2D(g.position, s.player.position);

        if (d <= kFireReach && std::abs(g.position.z - s.player.position.z) < 3.0f && namesFire(g.name) && (!best || d < distance2D(best->position, s.player.position)))
        {
            best = &g;
        }
    }

    return best;
}

bool LeaveFireBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A second in it: one walked through on the way is not stood in.
    constexpr Duration kStood{1000};

    if (to_)
    {
        return !s.player.isDeadOrGhost();
    }

    if (s.player.isDeadOrGhost() || !fireUnderfoot(s))
    {
        inSince_.reset();
        return false;
    }

    if (!inSince_)
    {
        inSince_ = ctx.now();
    }

    return ctx.now() - *inSince_ >= kStood;
}

void LeaveFireBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    const GameObjectState* fire = fireUnderfoot(s);

    if (!fire)
    {
        return;
    }

    // Straight out from the middle, the way the character already leans; on
    // ground the map has, else another way.
    const Vec3 middle = fire->position;
    const Vec3 away = s.player.position - middle;
    const float length = std::hypot(away.x, away.y);
    const float heading = length > 0.2f ? std::atan2(away.y, away.x) : s.player.heading + kPi;

    for (const float turn : {0.0f, 0.8f, -0.8f, 1.6f, -1.6f, 2.4f, -2.4f, kPi})
    {
        Vec3 spot = middle + Vec3{std::cos(heading + turn) * kFireClear, std::sin(heading + turn) * kFireClear, 0};
        spot.z = s.player.position.z;

        if (ctx.navigator.provider().usesClientGeometry())
        {
            const auto walkable = ctx.navigator.provider().findWalkablePoint(spot, 2.0f, 0.5f);

            if (!walkable)
            {
                continue;
            }

            spot = *walkable;
        }

        to_ = spot;
        break;
    }

    fire_ = middle;
    startedAt_ = ctx.now();
    ctx.log.info(logcat::Move, "standing in {} - stepping out of it", fire->name);
}

void LeaveFireBehavior::onExit(BotContext& ctx)
{
    inSince_.reset();
    fire_.reset();
    to_.reset();
    ctx.navigator.stop();
}

BehaviorStatus LeaveFireBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const auto done = [&]
    {
        inSince_.reset();
        fire_.reset();
        to_.reset();
        ctx.navigator.stop();
        return BehaviorStatus::Success;
    };

    if (!to_ || !fire_ || ctx.now() - startedAt_ > Duration(5000))
    {
        return done();
    }

    if (distance2D(s.player.position, *fire_) >= kFireClear - 0.5f)
    {
        return done();
    }

    const NavStatus status = ctx.navigator.moveTo(s, *to_, 0.5f);
    return status == NavStatus::Arrived || status == NavStatus::Unreachable ? done() : BehaviorStatus::Running;
}

// --- Campfire -------------------------------------------------------------------

namespace
{

// WoW Forever's campfire auras (1.60.1): "Campfire Nearby" by one, the minute
// sat by it counted down by "Welcoming Campfire", then "Camp Benefits" (and a
// stat, "Extra Strength") for an hour. Welcoming Campfire starts over after
// its minute while the character stays sat.
constexpr std::string_view kWelcoming = "Welcoming Campfire";
constexpr std::string_view kBenefits = "Camp Benefits";
// As near as the fire does not burn (LeaveFire's reach), and no nearer.
constexpr float kCampfireSit = kFireReach + 0.4f;
constexpr float kCampfireSearch = 150.0f;

bool namesCampfire(std::string_view name)
{
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower.find("campfire") != std::string::npos;
}

} // namespace

bool CampfireBehavior::isCampfire(const GameObjectState& object)
{
    return (object.kind == GameObjectKind::Generic || object.kind == GameObjectKind::Unknown) && namesCampfire(object.name);
}

std::vector<const GameObjectState*> CampfireBehavior::campItems(const GameSnapshot& s, const GameObjectState& fire)
{
    // A Sharpening Wheel stood 2.5 yd from Goldshire's (it gave Extra Strength).
    constexpr float kCampReach = 10.0f;
    std::vector<const GameObjectState*> out;

    for (const auto& g : s.objects)
    {
        if (g.createdBy != 0 && g.kind != GameObjectKind::FishingBobber && !namesCampfire(g.name) && distance2D(g.position, fire.position) <= kCampReach)
        {
            out.push_back(&g);
        }
    }

    return out;
}

const GameObjectState* CampfireBehavior::campfireInSight(const GameSnapshot& s, const BotContext& ctx)
{
    const GameObjectState* best = nullptr;

    for (const auto& g : s.objects)
    {
        if (!isCampfire(g) || ctx.objectBlacklist.contains(g.guid) || campItems(s, g).empty())
        {
            continue;
        }

        const float d = distance2D(g.position, s.player.position);

        if (d <= kCampfireSearch && std::abs(g.position.z - s.player.position.z) < 30.0f && (!best || d < distance2D(best->position, s.player.position)))
        {
            best = &g;
        }
    }

    return best;
}

bool CampfireBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.config.sitAtCampfires || s.player.isDeadOrGhost() || s.inCombat() || !s.attackers().empty() || ctx.partyFollower())
    {
        return false;
    }

    if (fire_ != 0)
    {
        return true;
    }

    return !s.player.hasAura(kBenefits) && campfireInSight(s, ctx) != nullptr;
}

void CampfireBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    const GameObjectState* fire = fire_ != 0 ? s.findObject(fire_) : campfireInSight(s, ctx);

    if (!fire)
    {
        return;
    }

    if (fire_ == 0)
    {
        std::string items;

        for (const auto* item : campItems(s, *fire))
        {
            items += (items.empty() ? "" : ", ") + item->name;
        }

        ctx.log.info(logcat::Rest, "{} {:.0f} yd off, {} by it - sitting down there for Camp Benefits", fire->name, distance2D(fire->position, s.player.position), items.empty() ? std::string("?") : items);
    }

    fire_ = fire->guid;
    phase_ = Phase::Walk;
    startedAt_ = ctx.now();
    sitSince_.reset();
    lastSit_.reset();
    lastWelcome_ = -1.0f;
}

void CampfireBehavior::onExit(BotContext& ctx)
{
    // Called away (a fight): looked at afresh after it. The fire is kept:
    // the minute goes on where the character sits.
    ctx.navigator.stop();
}

BehaviorStatus CampfireBehavior::done(BotContext& ctx, std::string_view why, bool leaveAlone)
{
    if (leaveAlone && fire_ != 0)
    {
        ctx.objectBlacklist.add(fire_, Duration(30 * 60 * 1000), std::string(why));
    }

    ctx.log.info(logcat::Rest, "campfire: {}", why);
    fire_ = 0;
    phase_ = Phase::Walk;
    sitSince_.reset();
    lastSit_.reset();
    lastWelcome_ = -1.0f;
    ctx.navigator.stop();
    return BehaviorStatus::Success;
}

BehaviorStatus CampfireBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const GameObjectState* fire = fire_ != 0 ? s.findObject(fire_) : nullptr;

    if (!fire)
    {
        return done(ctx, "the campfire is gone");
    }

    if (s.player.hasAura(kBenefits))
    {
        return done(ctx, "Camp Benefits");
    }

    // Walking up, sitting, and the minute, with time to spare.
    if (ctx.now() - startedAt_ > Duration(120000))
    {
        return done(ctx, "no Camp Benefits in two minutes", true);
    }

    const float d = distance2D(s.player.position, fire->position);

    if (phase_ == Phase::Walk)
    {
        if (d > kCampfireSit + 1.0f)
        {
            // To the side of it the character comes from.
            Vec3 toward = s.player.position - fire->position;
            toward.z = 0;
            const float length = std::max(0.01f, std::hypot(toward.x, toward.y));
            const Vec3 spot = fire->position + Vec3{toward.x / length * kCampfireSit, toward.y / length * kCampfireSit, s.player.position.z - fire->position.z};
            const NavStatus status = ctx.navigator.moveTo(s, spot, 0.5f);

            if (status == NavStatus::Unreachable)
            {
                return done(ctx, "no way to the campfire", true);
            }

            if (status != NavStatus::Arrived)
            {
                return BehaviorStatus::Running;
            }
        }

        ctx.navigator.stop();
        ctx.movement.stop();
        phase_ = Phase::Sit;
    }

    const Aura* welcome = s.player.findAura(kWelcoming);

    // Again while no Welcoming Campfire says it sits; once it does, only
    // seldom: a client without the stand state never shows it sat.
    const Duration again = phase_ == Phase::Wait ? Duration(15000) : Duration(3000);

    if (!s.player.isSitting && !s.player.isMoving && (!lastSit_ || ctx.now() - *lastSit_ >= again))
    {
        ctx.actions.chat("/sit");
        lastSit_ = ctx.now();

        if (!sitSince_)
        {
            sitSince_ = ctx.now();
        }
    }

    if (phase_ == Phase::Sit)
    {
        if (welcome)
        {
            phase_ = Phase::Wait;
            ctx.log.info(logcat::Rest, "campfire: {} - a minute sat by it", kWelcoming);
        }
        else if (sitSince_ && ctx.now() - *sitSince_ > Duration(8000))
        {
            return done(ctx, std::format("no {} by {}", kWelcoming, fire->name), true);
        }

        lastWelcome_ = welcome ? welcome->remaining : -1.0f;
        return BehaviorStatus::Running;
    }

    // The minute run out and started over: as good as Camp Benefits seen.
    if (welcome && lastWelcome_ >= 0 && welcome->remaining > lastWelcome_ + 5.0f)
    {
        return done(ctx, std::format("{} ran its minute", kWelcoming));
    }

    if (!welcome && d > kCampfireSit + 3.0f)
    {
        phase_ = Phase::Walk;
    }

    lastWelcome_ = welcome ? welcome->remaining : lastWelcome_;
    return BehaviorStatus::Running;
}

// --- LeaveRespawn ---------------------------------------------------------------

namespace
{
const FastRespawn* leftBehind(const GameSnapshot& s, const BotContext& ctx)
{
    for (const FastRespawn& f : ctx.fastRespawns)
    {
        if (ctx.now() < f.until && !ctx.questNeeds(f.entry) && distance2D(s.player.position, f.at) <= FastRespawn::kRadius)
        {
            return &f;
        }
    }

    return nullptr;
}

} // namespace

bool LeaveRespawnBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes.
    if (ctx.partyFollower())
    {
        return false;
    }

    if (to_)
    {
        return !s.isDead() && !s.inCombat() && s.attackers().empty();
    }

    return !s.isDead() && !s.player.isDeadOrGhost() && !s.inCombat() && s.attackers().empty() && leftBehind(s, ctx) != nullptr;
}

void LeaveRespawnBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    const FastRespawn* f = leftBehind(s, ctx);

    if (!to_ && f)
    {
        // Straight away from the place, as far again as it reaches, on ground
        // the map has; another way round where that is not walkable.
        Vec3 away = s.player.position - f->at;
        away.z = 0;
        const float length = std::hypot(away.x, away.y);
        const float heading = length > 0.5f ? std::atan2(away.y, away.x) : s.player.heading + kPi;
        const float reach = FastRespawn::kRadius + 20.0f;

        for (const float turn : {0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 1.8f, -1.8f})
        {
            Vec3 spot = f->at + Vec3{std::cos(heading + turn) * reach, std::sin(heading + turn) * reach, 0};
            spot.z = s.player.position.z;

            if (ctx.navigator.provider().usesClientGeometry())
            {
                const auto walkable = ctx.navigator.provider().findWalkablePoint(spot, 4.0f, 0.5f);

                if (!walkable)
                {
                    continue;
                }

                spot = *walkable;
            }

            to_ = spot;
            break;
        }

        from_ = f->at;
        startedAt_ = ctx.now();
        // What lies there unlooted stays there.
        std::erase_if(ctx.pendingLoot, [&](const PendingLoot& p) { return !p.inProgress && distance2D(p.position, f->at) <= FastRespawn::kRadius; });
        ctx.log.info(logcat::Move, "leaving where {} comes back at once - {}", f->name, to_ ? std::format("{:.0f} yd off, to rest there", distance2D(s.player.position, *to_)) : std::string("no ground to go to found"));
    }

    ctx.combat.defendOnly = true;
}

void LeaveRespawnBehavior::onExit(BotContext& ctx)
{
    // Called away (a fight): looked at afresh from where that leaves the bot.
    to_.reset();
    from_.reset();
    ctx.navigator.stop();
    ctx.combat.defendOnly = false;
}

BehaviorStatus LeaveRespawnBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const auto done = [&](std::string_view why)
    {
        ctx.log.info(logcat::Move, "away from where it comes back at once: {}", why);
        to_.reset();
        from_.reset();
        ctx.navigator.stop();
        ctx.combat.defendOnly = false;
        return BehaviorStatus::Success;
    };

    if (!to_ || !from_)
    {
        return done("nowhere to go");
    }

    if (ctx.now() - startedAt_ > Duration(40000))
    {
        return done("it took too long - resting where it got to");
    }

    const bool clear = s.hostilesNear(s.player.position, 20.0f).empty();

    if (distance2D(s.player.position, *from_) > FastRespawn::kRadius + 10.0f && clear)
    {
        return done(std::format("{:.0f} yd off and clear", distance2D(s.player.position, *from_)));
    }

    const NavStatus status = ctx.navigator.moveTo(s, *to_, 2.0f);

    if (status == NavStatus::Arrived)
    {
        return done("there");
    }

    if (status == NavStatus::Unreachable)
    {
        return done("no way there - resting here");
    }

    return BehaviorStatus::Running;
}

// --- Hold ----------------------------------------------------------------------

bool HoldBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    return ctx.followerHold != 0 && !s.isDead() && !s.inCombat() && s.attackers().empty();
}

void HoldBehavior::onEnter(const GameSnapshot&, BotContext& ctx)
{
    // No pull goes on to a mob we had only chosen.
    ctx.combat.engaged = 0;
    ctx.navigator.stop();
    ctx.movement.stop();
}

BehaviorStatus HoldBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (ctx.movement.isMoving() || s.player.isMoving)
    {
        ctx.movement.stop();
    }

    return BehaviorStatus::Running;
}

// --- Emergency -----------------------------------------------------------------

bool EmergencyBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.combatClass || s.isDead())
    {
        return false;
    }

    if (!s.inCombat() && s.attackers().empty())
    {
        return false;
    }

    auto cc = ctx.combatContext(s);
    return ctx.combatClass->wantsEmergency(cc);
}

BehaviorStatus EmergencyBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    auto cc = ctx.combatContext(s);
    ctx.log.warn(logcat::Combat, "emergency at health {:.0f}%", s.player.healthPercent());
    ctx.combatClass->onEmergency(cc);
    return BehaviorStatus::Running;
}

// --- Rest ----------------------------------------------------------------------

bool RestBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    ctx.resting = resting_;

    if (!ctx.combatClass || s.isDead() || s.inCombat() || !s.attackers().empty())
    {
        return false;
    }

    // Loot first before resting - unless every corpse waiting has a hostile standing by it.
    if ((ctx.lootEnabled() || ctx.skinEnabled()) && std::any_of(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return !ctx.corpseGuarded(s, p.position); }))
    {
        return false;
    }

    if (ctx.fishingBusy)
    {
        return false;
    }

    const TimePoint now = ctx.now();
    const bool isCasting = s.player.isCasting || ctx.actions.isCasting();
    const bool wantRest = (resting_ || ctx.needsRest(s) || isCasting) && now >= noRestUntil_;
    bool wantBuff = false;

    if (!wantRest)
    {
        auto cc = ctx.combatContext(s);
        wantBuff = ctx.combatClass->wantsStationaryBuff(cc);
    }

    if (!wantRest && !wantBuff)
    {
        return false;
    }

    // After a fight the rest is taken wherever the character stands, mobs
    // about or not - only not while one is coming for it (it would be pulled
    // out of the seat at once).
    if (!ctx.isSafeToRest(s))
    {
        if (!unsafeLogged_)
        {
            ctx.log.info(logcat::Rest, "a hostile is coming for us - not resting yet");
            unsafeLogged_ = true;
        }

        return false;
    }

    unsafeLogged_ = false;
    return true;
}

void RestBehavior::retryConsumables(const GameSnapshot& s, BotContext& ctx, TimePoint now)
{
    struct Consumable
    {
        std::string_view action;
        const char* buff;
        int available;
        std::optional<TimePoint>& missingSince;
    };
    const Consumable consumables[] = {
        {actions::Eat, "Food", s.player.foodCount(), foodMissingSince_},
        {actions::Drink, "Drink", s.player.waterCount(), drinkMissingSince_},
    };

    for (const auto& c : consumables)
    {
        // Nothing to fix: the buff is up, we are out of the item, or the item
        // is off cooldown anyway and the rest script can press it itself.
        if (s.player.hasBuff(c.buff) || c.available <= 0 || !ctx.actions.hasBinding(c.action) || ctx.actions.isReady(c.action))
        {
            c.missingSince.reset();
            continue;
        }

        if (!c.missingSince)
        {
            c.missingSince = now;
            continue;
        }

        if (now - *c.missingSince < ctx.config.restRetryInterval)
        {
            continue;
        }

        ctx.actions.clearCooldown(c.action);
        c.missingSince = now;
        ctx.log.info(logcat::Rest, "no {} buff {:.1f}s after using {} - using it again", c.buff, toSeconds(ctx.config.restRetryInterval), c.action);
    }
}

BehaviorStatus RestBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();
    ctx.navigator.stop();

    // A rest taken up again after a fight or a corpse run starts its time
    // over: counted from before, it timed out at once and the bot went on at
    // half health, with no rest for 30 seconds more.
    if (resting_ && now - lastTick_ > Duration(2000))
    {
        restStarted_ = now;
    }

    lastTick_ = now;

    const bool isCasting = s.player.isCasting || ctx.actions.isCasting();

    if (isCasting)
    {
        return BehaviorStatus::Running;
    }

    // Ensure the character has physically come to a stop before casting spells or resting
    if (ctx.movement.isMoving() || s.player.isMoving || s.player.speed > 0.0f)
    {
        ctx.movement.stop();
        return BehaviorStatus::Running;
    }

    if (!resting_ && ctx.needsRest(s) && now >= noRestUntil_)
    {
        resting_ = true;
        ctx.resting = true;
        restStarted_ = now;
        ++ctx.stats.rests;
        ctx.log.info(logcat::Rest, "resting: health {:.0f}% mana {:.0f}%", s.player.healthPercent(), s.player.manaPercent());
    }

    auto cc = ctx.combatContext(s);

    if (resting_)
    {
        if (ctx.restComplete(s))
        {
            resting_ = false;
            foodMissingSince_.reset();
            drinkMissingSince_.reset();
            ctx.log.info(logcat::Rest, "rest complete after {:.0f}s", toSeconds(now - restStarted_));
        }
        else if (now - restStarted_ > ctx.config.restTimeout)
        {
            resting_ = false;
            foodMissingSince_.reset();
            drinkMissingSince_.reset();
            noRestUntil_ = now + Duration(30000);
            ctx.log.warn(logcat::Rest, "rest timed out at health {:.0f}%", s.player.healthPercent());
        }
        else
        {
            retryConsumables(s, ctx, now);

            // Leave a gap after using a consumable so the next one is not sent
            // in the following tick, which the client tends to swallow.
            if (now - lastConsumable_ < ctx.config.restConsumableGap)
            {
                return BehaviorStatus::Running;
            }

            if (ctx.combatClass->onRest(cc))
            {
                const auto used = ctx.actions.lastAction();

                if (used && (*used == actions::Eat || *used == actions::Drink))
                {
                    lastConsumable_ = now;
                }
            }

            return BehaviorStatus::Running;
        }
    }

    if (ctx.combatClass->wantsBuff(cc))
    {
        // The rest just ended sitting down; buffs are spells too.
        if (ctx.standUp(s))
        {
            return BehaviorStatus::Running;
        }

        ctx.buffTracker.applyBuff(s, ctx);
    }

    return BehaviorStatus::Running;
}

// --- Clear the path ---------------------------------------------------------------

bool ClearPathBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes, and nowhere on its own.
    if (ctx.partyFollower())
    {
        return false;
    }

    if (!ctx.combatClass || s.isDead() || s.inCombat() || !s.attackers().empty())
    {
        return false;
    }

    if (s.player.isCasting || ctx.actions.isCasting())
    {
        return false;
    }

    // Only on the way somewhere: standing (resting, fishing, at a vendor) it is not walked into.
    if (!s.player.isMoving || ctx.fishingEnabled() || ctx.followerHold != 0)
    {
        return false;
    }

    if (ctx.combat.engaged != 0 || ctx.needsRest(s))
    {
        return false;
    }

    const UnitState* t = ctx.threatInTheWay(s);
    found_ = t ? t->guid : 0;
    return t != nullptr;
}

BehaviorStatus ClearPathBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const UnitState* t = found_ ? s.findUnit(found_) : nullptr;
    found_ = 0;

    if (!t || !t->isAlive())
    {
        return BehaviorStatus::Failure;
    }

    ctx.navigator.stop();
    ctx.combat.engaged = t->guid;
    ctx.combat.clearing = t->guid;
    ++ctx.stats.pulls;
    ctx.log.info(logcat::Target, "{} {} (level {}) is in the way, {:.0f} yd off - fighting it before it comes for us", t->name, formatGuid(t->guid), t->level, distance(s.player.position, t->position));
    return BehaviorStatus::Success;
}

// --- Pull ----------------------------------------------------------------------

bool PullBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes, and nowhere on its own.
    if (ctx.partyFollower())
    {
        return false;
    }

    if (!ctx.combatClass || s.isDead() || s.inCombat() || !s.attackers().empty())
    {
        return false;
    }

    if (ctx.config.quest.enabled && ctx.now() < ctx.questPullAfter)
    {
        return false;
    }

    if (s.player.isCasting || ctx.actions.isCasting())
    {
        return false;
    }

    if (ctx.combat.defendOnly)
    {
        return false; // travelling: do not go looking for a fight
    }

    if (ctx.fishingEnabled())
    {
        return false; // fishing only fights what attacks it
    }

    if (ctx.combat.engaged != 0)
    {
        return false;
    }

    // Loot first - unless every corpse waiting has a hostile standing by it.
    if (std::any_of(ctx.pendingLoot.begin(), ctx.pendingLoot.end(), [&](const PendingLoot& p) { return !ctx.corpseGuarded(s, p.position); }))
    {
        return false;
    }

    if (ctx.needsRest(s))
    {
        return false;
    }

    return corpseGuard(s, ctx) != nullptr || ctx.targets.select(s) != nullptr;
}

const UnitState* PullBehavior::corpseGuard(const GameSnapshot& s, BotContext& ctx) const
{
    // A kill left unlooted for a mob standing by it: that mob next, then the
    // loot - not another mob somewhere else first, and back to the corpse
    // between fights. A quest mob filter does not hold it back: the corpse
    // may carry the quest's item.
    for (const auto& p : ctx.pendingLoot)
    {
        const UnitState* guard = ctx.guardOf(s, p.position);

        if (!guard || guard->isPlayer || ctx.targetBlacklist.contains(guard->guid))
        {
            continue;
        }

        const auto reason = ctx.targets.rejectReason(*guard, s);

        if (!reason || *reason == "not a quest mob")
        {
            return guard;
        }
    }

    return nullptr;
}

std::optional<Vec3> PullBehavior::approachDestination(const GameSnapshot& s, BotContext& ctx) const
{
    const UnitState* target = corpseGuard(s, ctx);

    if (!target)
    {
        target = ctx.targets.select(s);
    }

    return target ? std::optional(target->position) : std::nullopt;
}

BehaviorStatus PullBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (const UnitState* guard = corpseGuard(s, ctx))
    {
        ctx.combat.engaged = guard->guid;
        ctx.combat.clearing = guard->guid;
        ++ctx.stats.pulls;
        ctx.log.info(logcat::Target, "Selected {} {} level {} distance={:.1f}: it stands by a corpse to loot", guard->name, formatGuid(guard->guid), guard->level, distance(s.player.position, guard->position));
        return BehaviorStatus::Success;
    }

    const UnitState* t = ctx.targets.select(s);

    if (!t)
    {
        return BehaviorStatus::Failure;
    }

    if (ctx.avoidPacked(s, *t))
    {
        return BehaviorStatus::Failure; // the next best, next tick
    }

    // One standing between here and there is fought first: going past it for
    // the one behind, the bot had both on it.
    if (const UnitState* nearer = ctx.targets.standingInTheWay(s, *t); nearer && !ctx.avoidPacked(s, *nearer))
    {
        ctx.combat.engaged = nearer->guid;
        ctx.combat.clearing = nearer->guid;
        ++ctx.stats.pulls;
        ctx.log.info(logcat::Target, "Selected {} {} level {} distance={:.1f}: it stands in the way to {} ({:.0f} yd)", nearer->name, formatGuid(nearer->guid), nearer->level, distance(s.player.position, nearer->position), t->name, distance(s.player.position, t->position));
        return BehaviorStatus::Success;
    }

    // Where the fight would be, another mob would notice us and join in.
    {
        auto cc = ctx.combatContext(s);

        if (const UnitState* other = ctx.joinsTheFight(s, *t, ctx.combatClass ? ctx.combatClass->combatRange(cc) : 5.0f))
        {
            ctx.blacklistTarget(t->guid, t->name, std::format("{} (level {}) and others would join the fight, {:.0f} yd from it", other->name, other->level, distance2D(other->position, t->position)), Duration(60000));
            return BehaviorStatus::Failure; // the next best, next tick
        }
    }

    ctx.combat.engaged = t->guid;
    ++ctx.stats.pulls;
    ctx.log.info(logcat::Target, "Selected {} {} level {} distance={:.1f}", t->name, formatGuid(t->guid), t->level, distance(s.player.position, t->position));
    return BehaviorStatus::Success;
}

// --- Navigation ----------------------------------------------------------------

bool NavigationBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes, and nowhere on its own.
    if (ctx.partyFollower())
    {
        return false;
    }

    if (s.player.isCasting || ctx.actions.isCasting())
    {
        return false;
    }

    // Fishing stays by the water: no walking the grind route, even while it
    // cannot fish. Vendor and corpse runs walk their own routes.
    if (ctx.fishingEnabled())
    {
        return false;
    }

    if (s.isDead())
    {
        return false;
    }

    // Questing with Pathfinder needs no profile: the quests say where to go.
    if (questingWithoutRoute(ctx))
    {
        return true;
    }

    return ctx.route && !ctx.route->empty() && !ctx.route->finished();
}

bool NavigationBehavior::questingWithoutRoute(const BotContext& ctx)
{
    return ctx.config.usePathfinding && ctx.config.quest.enabled && (!ctx.route || ctx.route->empty());
}

namespace
{

// Distance from `pos` to the closest point of `points`, and that point's index.
std::pair<float, std::size_t> nearestOf(const std::vector<Vec3>& points, const Vec3& pos)
{
    float best = std::numeric_limits<float>::max();
    std::size_t index = 0;

    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const float d = distance(pos, points[i]);

        if (d < best)
        {
            best = d;
            index = i;
        }
    }

    return {best, index};
}

} // namespace

void NavigationBehavior::planLeadIn(const GameSnapshot& s, BotContext& ctx)
{
    leadInConsidered_ = true;

    if (!ctx.profile)
    {
        return;
    }

    const Vec3 pos = s.player.position;
    const auto& grind = ctx.route->points();

    if (grind.empty())
    {
        return;
    }

    const auto [grindDist, grindIndex] = nearestOf(grind, pos);

    // Already on (or beside) the grind route: nothing to lead in from.
    if (grindDist <= ctx.config.waypointRadius * 2.0f)
    {
        return;
    }

    // Closest auxiliary route wins.
    const std::vector<Vec3>* best = nullptr;
    std::string_view bestName;
    float bestDist = grindDist;

    for (const auto& [points, name] : {std::pair{&ctx.profile->vendorWaypoints, std::string_view("vendor")}, std::pair{&ctx.profile->ghostWaypoints, std::string_view("ghost")}})
    {
        if (points->size() < 2)
        {
            continue;
        }

        const auto [d, _] = nearestOf(*points, pos);

        if (d < bestDist)
        {
            best = points;
            bestName = name;
            bestDist = d;
        }
    }

    if (!best)
    {
        return;
    }

    // Follow it towards whichever of its ends is nearest the grind route.
    const bool forward = nearestOf(grind, best->back()).first < nearestOf(grind, best->front()).first;
    const std::size_t from = nearestOf(*best, pos).second;
    leadIn_.clear();

    if (forward)
    {
        for (std::size_t i = from; i < best->size(); ++i)
        {
            leadIn_.push_back((*best)[i]);
        }
    }
    else
    {
        for (std::size_t i = from + 1; i-- > 0;)
        {
            leadIn_.push_back((*best)[i]);
        }
    }

    leadInIndex_ = 0;

    ctx.log.info(logcat::Move, "starting on the {} route {:.0f} yd out - following it back to the grind route", bestName, bestDist);
}

std::size_t PathDetours::apply(std::vector<Vec3>& path, BotContext& ctx, std::size_t first) const
{
    if (!ctx.obstacles)
    {
        return 0;
    }

    const std::size_t applied = applyDetours(path, ctx.obstacles->detours(), first);

    if (applied)
    {
        ctx.log.debug(logcat::Move, "{} learned detour(s) added to the path", applied);
    }

    return applied;
}

void PathDetours::update(const GameSnapshot& s, BotContext& ctx, std::size_t index, bool recovering)
{
    corrector_.configure(ctx.config.routeCorrector);
    const bool wasRecording = corrector_.recording();
    corrector_.update(s.player.position, index, recovering);

    if (!wasRecording && corrector_.recording() && ctx.obstacles)
    {
        movesAtRecording_ = ctx.obstacles->maneuversLearned();
    }
}

std::size_t PathDetours::reached(std::vector<Vec3>& path, std::size_t index, BotContext& ctx)
{
    // A jump or sidestep learned while working free already handles the obstacle.
    if (corrector_.recording() && ctx.obstacles && ctx.obstacles->maneuversLearned() != movesAtRecording_)
    {
        corrector_.abandon();
        return 0;
    }

    const std::size_t added = corrector_.onWaypointReached(index, path, &ctx.log);

    if (added && ctx.obstacles)
    {
        const RouteCorrection& fix = corrector_.corrections().back();
        ctx.obstacles->learnDetour(LearnedDetour{fix.from, fix.to, fix.stuckAt, fix.detour}, &ctx.log);
    }

    return added;
}

std::vector<Vec3> NavigationBehavior::roamLegs() const
{
    // Build Route: the full circuit, starting at the next point, so the radar
    // shows the whole lap while the active navigation path shows its current leg.
    if (!circuit_.empty() && circuitIndex_ < circuit_.size())
    {
        std::vector<Vec3> legs(circuitLeadIn_.begin() + static_cast<std::ptrdiff_t>(std::min(circuitLeadInIndex_, circuitLeadIn_.size())), circuitLeadIn_.end());
        legs.insert(legs.end(), circuit_.begin() + static_cast<std::ptrdiff_t>(circuitIndex_), circuit_.end());
        legs.insert(legs.end(), circuit_.begin(), circuit_.begin() + static_cast<std::ptrdiff_t>(circuitIndex_));
        legs.push_back(circuit_[circuitIndex_]);
        return legs;
    }

    if (roamLeg_ >= roamPath_.size())
    {
        return {};
    }

    return {roamPath_.begin() + static_cast<std::ptrdiff_t>(roamLeg_), roamPath_.end()};
}

void NavigationBehavior::rejoinCircuit(const GameSnapshot& s, BotContext& ctx)
{
    if (circuit_.empty())
    {
        return;
    }

    const Vec3& pos = s.player.position;
    // Back where it left the lap: the point it was heading for, or one a little
    // further on that lies nearer (a gather or a chase went that way). Never a
    // part of the lap behind it or far ahead.
    constexpr std::size_t kLookAhead = 60; // about 250 yards of circuit

    if (circuitIndex_ >= circuit_.size())
    {
        circuitIndex_ = 0;
    }

    std::size_t best = circuitIndex_;
    const std::size_t last = std::min(circuit_.size(), circuitIndex_ + kLookAhead);

    for (std::size_t i = circuitIndex_ + 1; i < last; ++i)
    {
        if (distance2D(pos, circuit_[i]) < distance2D(pos, circuit_[best]))
        {
            best = i;
        }
    }

    circuitIndex_ = best;
    circuitLeadIn_ = ctx.navigator.provider().usesClientGeometry() ? std::vector<Vec3>{circuit_[circuitIndex_]} : circuitWay(ctx.route->points(), pos, circuit_[circuitIndex_], ctx.obstacles);
    circuitLeadInIndex_ = 0;
    circuitStraight_ = false;
    circuitPointSince_ = ctx.now();
}

bool NavigationBehavior::circuitTick(const GameSnapshot& s, BotContext& ctx)
{
    // Only a new zone can stand in the circuit's way (a learned jump or
    // sidestep cannot). Then the stretches that run into it are taken round
    // it and the rest of the lap is kept; a new circuit only when that fails.
    const std::size_t zones = ctx.obstacles ? ctx.obstacles->noGoZones().size() + ctx.obstacles->avoidZones().size() : 0;

    if (!circuit_.empty() && zones != circuitZones_)
    {
        circuitZones_ = zones;
        const std::size_t before = circuit_.size();
        const std::vector<Vec3> previous = circuit_;

        if (ctx.navigator.provider().usesClientGeometry() || !repairCircuit(ctx.route->points(), circuit_, circuitIndex_, ctx.obstacles))
        {
            ctx.log.info(logcat::Move, "build route: no way round the new obstacle - building a new circuit from here");
            circuit_.clear();
        }
        else if (circuit_ != previous)
        {
            ctx.log.info(logcat::Move, "build route: went round the new obstacle ({} points, were {})", circuit_.size(), before);

            if (circuitIndex_ >= circuit_.size())
            {
                circuit_.clear();
            }
        }
        else
        {
            ctx.log.debug(logcat::Move, "build route: the new obstacle is off the circuit");
        }
    }

    if (circuit_.empty())
    {
        if (ctx.navigator.provider().usesClientGeometry() && ctx.now() < circuitRetryAt_)
        {
            ctx.navigator.stop();
            return true;
        }

        // A full-area circuit can take a while to plan. Release the previous
        // lap's movement before doing this synchronous calculation.
        ctx.navigator.stop();
        circuit_ = ctx.navigator.provider().usesClientGeometry() ? buildClientCircuit(ctx.route->points(), s.player.position, ctx.navigator.provider(), ctx.obstacles, roamRandom_) : buildCircuit(ctx.route->points(), s.player.position, ctx.obstacles, roamRandom_);

        if (circuit_.empty())
        {
            if (!ctx.navigator.provider().usesClientGeometry())
            {
                return false;
            }

            circuitRetryAt_ = ctx.now() + Duration(30000);
            ctx.navigator.stop();
            ctx.log.warn(logcat::Move, "No complete walkable circuit inside the roam area; retrying in 30 seconds.");
            return true;
        }

        circuitZones_ = zones;
        circuitIndex_ = 0;
        float length = 0.0f;

        for (std::size_t i = 0; i < circuit_.size(); ++i)
        {
            length += distance2D(circuit_[i], circuit_[(i + 1) % circuit_.size()]);
        }

        ctx.log.info(logcat::Move, "build route: circuit {} - {} points, {:.0f} yd round", circuitLaps_ + 1, circuit_.size(), length);
        // It starts where the bot stands: only the way onto its first point.
        circuitLeadIn_ = ctx.navigator.provider().usesClientGeometry() ? std::vector<Vec3>{circuit_.front()} : circuitWay(ctx.route->points(), s.player.position, circuit_.front(), ctx.obstacles);
        circuitLeadInIndex_ = 0;
        circuitStraight_ = false;
        circuitPointSince_ = ctx.now();
    }

    ctx.circuitAhead = &circuit_;
    ctx.circuitAheadFrom = circuitIndex_;
    const float radius = ctx.config.waypointRadius;
    const TimePoint now = ctx.now();
    NavStatus status;

    if (circuitLeadInIndex_ < circuitLeadIn_.size())
    {
        status = ctx.navigator.moveTo(s, circuitLeadIn_[circuitLeadInIndex_], radius, false);

        if (status == NavStatus::Arrived || status == NavStatus::Unreachable)
        {
            ++circuitLeadInIndex_;
        }

        humanizer_.applyMovement(ctx);
        return true;
    }

    const Vec3& point = circuit_[circuitIndex_];
    status = ctx.navigator.moveTo(s, point, radius, false, circuitStraight_);

    if (status == NavStatus::Unreachable && !circuitStraight_ && !ctx.navigator.provider().usesClientGeometry())
    {
        // Most likely a gap between two obstacles the navigator keeps a wider
        // berth from than the circuit does.
        circuitStraight_ = true;
        ctx.navigator.stop();
        return true;
    }

    // Stuck on one point for long: leave it out and go on.
    const bool stalled = now - circuitPointSince_ > Duration(20000);

    if (status == NavStatus::Arrived || stalled || (status == NavStatus::Unreachable && ctx.navigator.provider().usesClientGeometry()))
    {
        if (status != NavStatus::Arrived)
        {
            ctx.log.info(logcat::Move, "build route: no way to circuit point {} - going on to the next", circuitIndex_);
        }
        else
        {
            ++ctx.stats.waypointsReached;
        }

        ++circuitIndex_;
        circuitStraight_ = false;
        circuitPointSince_ = now;

        if (circuitIndex_ >= circuit_.size())
        {
            ++circuitLaps_;
            ctx.log.info(logcat::Move, "build route: lap {} done - building the next circuit", circuitLaps_);
            circuit_.clear();
        }
    }

    humanizer_.applyMovement(ctx);
    return true;
}

bool NavigationBehavior::roamTick(const GameSnapshot& s, BotContext& ctx)
{
    if (!roamTarget_)
    {
        const std::vector<Vec3>& route = ctx.route->points();
        roamTarget_ = pickRoamPoint(route, s.player.position, ctx.obstacles, roamRandom_, ctx.config.roam.minLeg, 100, &ctx.navigator.provider(), ctx.config.roam.zone);

        if (!roamTarget_)
        {
            return false;
        }

        if (ctx.navigator.provider().usesClientGeometry())
        {
            roamPath_ = {*roamTarget_};
        }
        else
        {
            // Round any corner the straight line would cut, so the bot stays inside the route.
            roamPath_ = roamPath(route, s.player.position, *roamTarget_, ctx.obstacles);

            if (roamPath_.empty())
            {
                roamPath_ = {*roamTarget_};
            }
        }

        roamLeg_ = 0;

        if (roamPath_.size() > 1)
        {
            ctx.log.debug(logcat::Move, "roaming to {} round {} corner(s)", *roamTarget_, roamPath_.size() - 1);
        }
        else
        {
            ctx.log.debug(logcat::Move, "roaming to {}", *roamTarget_);
        }
    }

    const bool last = roamLeg_ + 1 >= roamPath_.size();
    const NavStatus status = ctx.navigator.moveTo(s, roamPath_[roamLeg_], ctx.config.waypointRadius, false);

    if (status == NavStatus::Recovering)
    {
        return true;
    }

    if (status == NavStatus::Unreachable)
    {
        ctx.log.info(logcat::Move, "roaming: no clear route to {} - picking another spot", *roamTarget_);
        roamStuckAt_.reset();
        roamTarget_.reset();
        roamPath_.clear();
        ctx.navigator.stop();
    }
    else if (status == NavStatus::Arrived && !last)
    {
        ++roamLeg_;
    }
    else if (status == NavStatus::Arrived)
    {
        ++ctx.stats.waypointsReached;
        roamTarget_.reset();
        roamPath_.clear();
    }

    humanizer_.applyMovement(ctx);
    return true;
}

bool NavigationBehavior::questTick(const GameSnapshot& s, BotContext& ctx)
{
    static const std::vector<Vec3> kNoRoute;
    const std::vector<Vec3>& route = ctx.route ? ctx.route->points() : kNoRoute;
    const Vec3& pos = s.player.position;
    const TimePoint now = ctx.now();
    // A quest set aside for its packs is out of the choice of the lowest level
    // quest too: the others are worked on meanwhile.
    const Duration kPacksFor{300000};
    std::erase_if(questInPacks_, [&](const auto& q) { return now - q.second > kPacksFor; });
    std::vector<QuestTarget> open;
    open.reserve(ctx.questTargets.size());
    std::copy_if(ctx.questTargets.begin(), ctx.questTargets.end(), std::back_inserter(open), [&](const QuestTarget& t) { return !questInPacks_.contains(t.questId); });

    if (questSpot_ && questSpotQuest_ != 0)
    {
        const auto quest = std::find_if(open.begin(), open.end(), [&](const auto& t) { return t.questId == questSpotQuest_; });

        if (quest == open.end() || quest->done || !isLowestLevelQuest(*quest, open))
        {
            questSpot_.reset();
            questPath_.clear();
            questWaitSince_.reset();
            questOffSurfaceSince_.reset();
            questSurfaceCheckAt_ = TimePoint{};
            ctx.navigator.stop();
        }
    }

    // A spot looked at is left alone for a while: the mobs there are dead or
    // were never there, and the next spot is tried meanwhile.
    constexpr float kLookedAt = 25.0f;
    const Duration kLeaveFor{300000};
    std::erase_if(questVisited_, [&](const auto& v) { return now - v.second > kLeaveFor; });
    // Reached, the keys stay held: the next place is planned on the next tick
    // (a new destination is a new way), and letting go stopped the character
    // for a moment at every place it looked for mobs.
    const auto markVisited = [&](bool stopMoving = true)
    {
        if (questSpot_)
        {
            questVisited_.emplace_back(*questSpot_, now);
        }

        questSpot_.reset();
        questPath_.clear();
        questSpotTotalDistance_ = 0.0f;
        questWaitSince_.reset();
        questOffSurfaceSince_.reset();
        questSurfaceCheckAt_ = TimePoint{};

        if (stopMoving)
        {
            ctx.navigator.stop();
        }
    };

    // A navigation failure while falling is transient: once the player lands,
    // Pathfinder can plan again from the new position. Do not mark the quest
    // spot visited or blacklist its mob just because the current position is
    // momentarily above the mesh.
    if (questOffSurfaceSince_)
    {
        constexpr Duration kSurfaceCheckEvery{250};
        constexpr Duration kOffSurfaceTimeout{10000};
        const bool timedOut = now - *questOffSurfaceSince_ >= kOffSurfaceTimeout;

        if (now < questSurfaceCheckAt_ && !timedOut)
        {
            return true;
        }

        questSurfaceCheckAt_ = now + kSurfaceCheckEvery;

        if (ctx.navigator.provider().canWalkStraight(pos, pos))
        {
            ctx.log.info(logcat::Move, "quest mode: landed on a walkable surface - replanning the way to {}", *questSpot_);
            questOffSurfaceSince_.reset();
            questSurfaceCheckAt_ = TimePoint{};
        }
        else if (timedOut)
        {
            ctx.log.warn(logcat::Move, "quest mode: still off a walkable surface after {}s - trying another spot", std::chrono::duration_cast<std::chrono::seconds>(kOffSurfaceTimeout).count());

            if (questSpotMob_ != 0)
            {
                questMobNoWay_[std::exchange(questSpotMob_, 0)] = now;
            }

            markVisited();
            return true;
        }
        else
        {
            return true;
        }
    }

    // A quest after one mob in one place - Garrick Padfoot - is waited for
    // there: dead, it respawns, and another player's, it is free again soon.
    // Gone off at once, the bot wandered off to Goldshire looking for other
    // quests. Pull takes the mob when it can, Combat answers what comes.
    constexpr Duration kWaitForOne{360000};
    constexpr Duration kWaitSaidEvery{60000};
    const auto singleTarget = [&](std::uint32_t questId) -> const QuestTarget*
    {
        const auto q = std::find_if(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const auto& t) { return t.questId == questId; });

        if (q == ctx.questTargets.end() || q->done || q->mobs.size() != 1 || !q->objects.empty() || q->spots.empty() || q->progress.empty())
        {
            return nullptr;
        }

        for (const auto& p : q->progress)
        {
            if (!p.ends_with("/1"))
            {
                return nullptr;
            }
        }

        for (const auto& a : q->spots)
        {
            for (const auto& b : q->spots)
            {
                if (distance2D(a, b) > 40.0f)
                {
                    return nullptr;
                }
            }
        }

        return &*q;
    };

    // Questie has no heights: a spot gets the character's, and the way to it
    // ends on the ground the character is on - on the hill over Fargodeep
    // Mine, with Goldtooth in the mine underneath, where the bot stood and
    // waited for him. The client has the mob itself, through the ground: one
    // of a quest's mobs in sight on another level is gone to where it stands,
    // and Pathfinder finds the way down (or up) to it. Pull leaves a mob that
    // far above or below alone until then.
    constexpr float kOtherLevel = 6.0f;
    constexpr float kSightAcross = 80.0f;
    std::erase_if(questMobNoWay_, [&](const auto& m) { return now - m.second > kLeaveFor; });

    if (ctx.config.usePathfinding && ctx.navigator.provider().usesClientGeometry())
    {
        const UnitState* mob = nullptr;
        const QuestTarget* mobOf = nullptr;

        for (const auto& target : open)
        {
            if (target.done || target.mobs.empty() || !isLowestLevelQuest(target, open))
            {
                continue;
            }

            for (const auto& u : s.units)
            {
                if (u.isPlayer || !u.isAlive() || std::find(target.mobs.begin(), target.mobs.end(), u.entry) == target.mobs.end() || questMobNoWay_.contains(u.guid))
                {
                    continue;
                }

                // Another player's, or one not to be attacked: Pull would leave
                // it alone once there.
                if (u.isTappedByOther || !u.isAttackable || (u.inCombat && u.targetGuid != 0 && u.targetGuid != s.player.guid))
                {
                    continue;
                }

                if (std::fabs(u.position.z - pos.z) < kOtherLevel || distance2D(u.position, pos) > kSightAcross || (ctx.obstacles && ctx.obstacles->avoids(u.position)) || ctx.dangerAt(u.position))
                {
                    continue;
                }

                // The one gone for is kept while it will do: with one above and
                // one below, the nearest changed with every step towards either,
                // and the bot turned from one to the other for as long as it stood
                // between them.
                if (mob && mob->guid == questSpotMob_)
                {
                    continue;
                }

                if (!mob || u.guid == questSpotMob_ || distance(u.position, pos) < distance(mob->position, pos))
                {
                    mob = &u;
                    mobOf = &target;
                }
            }
        }

        // Followed as it walks about, a few yards at a time.
        if (mob && (questSpotMob_ != mob->guid || !questSpot_ || distance(*questSpot_, mob->position) > 5.0f))
        {
            if (questSpotMob_ != mob->guid)
            {
                ctx.log.info(logcat::Move, "quest mode: {} for {} is {:.0f} yd {} here, on another level - going to it at {}", mob->name, mobOf->questName, std::fabs(mob->position.z - pos.z), mob->position.z < pos.z ? "below" : "above", mob->position);
            }

            questSpot_ = mob->position;
            questSpotQuest_ = mobOf->questId;
            questSpotMob_ = mob->guid;
            questPath_ = {mob->position};
            questLeg_ = 0;
            questSpotTotalDistance_ = distance(pos, mob->position);
            questWaitSince_.reset();
        }
        else if (!mob && questSpotMob_ != 0)
        {
            // Dead, or on the character's level now: Pull's from here.
            questSpotMob_ = 0;
        }
    }

    if (questSpot_ && questWaitSince_ && distance2D(pos, *questSpot_) <= ctx.config.waypointRadius + 5.0f)
    {
        const QuestTarget* one = singleTarget(questSpotQuest_);

        if (!one || now - *questWaitSince_ >= kWaitForOne)
        {
            if (one)
            {
                ctx.log.info(logcat::Move, "quest mode: waited {} minutes at {} for {} - trying elsewhere", std::chrono::duration_cast<std::chrono::minutes>(kWaitForOne).count(), *questSpot_, one->questName);
            }

            markVisited();
        }
        else
        {
            ctx.navigator.halt();

            if (now - questWaitSaid_ >= kWaitSaidEvery)
            {
                questWaitSaid_ = now;
                ctx.log.info(logcat::Move,
                             "quest mode: waiting at {} for the one mob {} needs - it may be dead or "
                             "another player's ({} s so far)",
                             *questSpot_, one->questName, std::chrono::duration_cast<std::chrono::seconds>(now - *questWaitSince_).count());
            }

            return true;
        }
    }

    if (!questSpot_)
    {
        const bool area = outlinesArea(route);
        const auto nearestWaypoint = [&](const Vec3& p)
        {
            const Vec3* best = nullptr;

            for (const auto& w : route)
            {
                if (!best || distance2D(w, p) < distance2D(*best, p))
                {
                    best = &w;
                }
            }

            return best;
        };
        // Inside the profile's area; a route walked there and back has no
        // inside, so near it will do.
        constexpr float kNearTheRoute = 60.0f;
        constexpr float kWaypointHeightReach = 60.0f;
        std::optional<Vec3> best;
        std::string quest;
        std::uint32_t questId = 0;
        SpotGroups bestGroups;

        for (const auto& target : open)
        {
            if (target.done || !isLowestLevelQuest(target, open))
            {
                continue;
            }

            // Every place it has within a pack's reach: set aside a while.
            if (!target.spots.empty() && std::all_of(target.spots.begin(), target.spots.end(), [&](const Vec3& spot) { return ctx.dangerAt(spot).has_value(); }))
            {
                const auto zone = ctx.dangerAt(target.spots.front());
                ctx.log.info(logcat::Move, "quest mode: every place for {} lies within the reach of {} - leaving it for {} minutes, other quests meanwhile", target.questName, zone->name.empty() ? std::string("mobs kept clear of") : zone->name, std::chrono::duration_cast<std::chrono::minutes>(kPacksFor).count());
                // The next quest is chosen this tick: no tick stood still for it.
                questInPacks_[target.questId] = now;
                continue;
            }

            const auto usable = [&](std::size_t i)
            {
                const Vec3& spot = target.spots[i];

                // In a No Go zone: its mobs are not fought either.
                if (ctx.obstacles && ctx.obstacles->avoids(spot))
                {
                    return false;
                }

                if (!ctx.config.usePathfinding)
                {
                    const Vec3* w = nearestWaypoint(spot);

                    if (!w)
                    {
                        return false;
                    }

                    if (area ? !insideRoute(route, spot) : distance2D(*w, spot) > kNearTheRoute)
                    {
                        return false;
                    }
                }

                return !std::any_of(questVisited_.begin(), questVisited_.end(), [&](const auto& v) { return distance2D(v.first, spot) < kLookedAt; });
            };
            // Only the group of spots with the lowest mobs, where there are several.
            const SpotGroups groups = lowestLevelSpots(target, usable);

            for (std::size_t i = 0; i < target.spots.size(); ++i)
            {
                if (!groups.chosen[i])
                {
                    continue;
                }

                Vec3 spot = target.spots[i];
                const Vec3* w = nearestWaypoint(spot);

                // Questie has no heights: the nearest waypoint's, where one is
                // near; else the character's (QuestSource's). A profile made
                // for another zone gave Northshire spots Dun Morogh's height,
                // 300 yards up, and Pathfinder found none of them.
                if (w && distance2D(*w, spot) <= kWaypointHeightReach)
                {
                    spot.z = w->z;
                }

                if (!best || distance2D(pos, spot) < distance2D(pos, *best))
                {
                    best = spot;
                    quest = target.questName;
                    questId = target.questId;
                    bestGroups = groups;
                }
            }
        }

        // The quest data's quest givers are gone looking for only with an
        // empty quest log, to get started. With quests in the log, new ones
        // are taken from the NPCs met on the way (QuestHandInBehavior).
        if (!best && ctx.config.usePathfinding && ctx.questSource && s.player.questLog.empty())
        {
            for (const auto& spot : ctx.questSource->questGiverSpots(s))
            {
                if (std::any_of(questVisited_.begin(), questVisited_.end(), [&](const auto& v) { return distance2D(v.first, spot) < kLookedAt; }))
                {
                    continue;
                }

                if (!best || distance2D(pos, spot) < distance2D(pos, *best))
                {
                    best = spot;
                }
            }

            quest = "available quests in the starting zone";
        }

        if (!best)
        {
            return false;
        }

        questSpot_ = best;
        questSpotQuest_ = questId;
        questSpotMob_ = 0;
        questWaitSince_.reset();
        questOffSurfaceSince_.reset();
        questSurfaceCheckAt_ = TimePoint{};
        questPath_ = area && !ctx.config.usePathfinding ? circuitWay(route, pos, *best, ctx.obstacles) : std::vector<Vec3>{*best};
        questLeg_ = 0;
        questSpotTotalDistance_ = !questPath_.empty() ? remainingPathDistance(pos, questPath_, 0) : distance2D(pos, *best);

        if (questPath_.empty())
        {
            ctx.log.debug(logcat::Move, "quest mode: no clear way to {} - trying another spot", *best);
            markVisited();
            return true;
        }

        if (questId != 0 && bestGroups.groups > 1 && bestGroups.level > 0)
        {
            ctx.log.info(logcat::Move,
                         "quest mode: heading for {} ({:.0f} yd) for {} - of {} places its mobs are at, the one "
                         "with the lowest (level {:.1f} on average)",
                         *questSpot_, distance2D(pos, *questSpot_), quest, bestGroups.groups, bestGroups.level);
        }
        else
        {
            ctx.log.info(logcat::Move, "quest mode: heading for {} ({:.0f} yd) for {}", *questSpot_, distance2D(pos, *questSpot_), quest);
        }
    }

    // The place its mobs are at is a pack's (Defias Pathstalkers among
    // Pillagers), or a strong mob's: the way would end in their midst, the
    // zone round it being the one place a way goes into. Another place.
    if (const auto zone = ctx.dangerAt(*questSpot_))
    {
        ctx.log.info(logcat::Move, "quest mode: {} lies within the reach of {} - trying another spot", *questSpot_, zone->name.empty() ? std::string("mobs kept clear of") : zone->name);
        markVisited();
        return true;
    }

    const NavStatus status = ctx.navigator.moveTo(s, questPath_[questLeg_], ctx.config.waypointRadius, false);

    if (status == NavStatus::Unreachable)
    {
        if (ctx.config.usePathfinding && ctx.navigator.provider().usesClientGeometry() && !ctx.navigator.provider().canWalkStraight(pos, pos))
        {
            questOffSurfaceSince_ = now;
            questSurfaceCheckAt_ = now + Duration(250);
            ctx.navigator.stop();
            ctx.log.info(logcat::Move, "quest mode: off a walkable surface - keeping {} and waiting to replan after landing", *questSpot_);
            return true;
        }

        ctx.log.info(logcat::Move, "quest mode: {} cannot be reached - trying another spot", *questSpot_);

        // A mob on another level with no way to it: not gone for again for a while.
        if (questSpotMob_ != 0)
        {
            questMobNoWay_[std::exchange(questSpotMob_, 0)] = now;
        }

        markVisited();
    }
    else if (status == NavStatus::Arrived && questLeg_ + 1 < questPath_.size())
    {
        ++questLeg_;
    }
    else if (status == NavStatus::Arrived && questSpotMob_ != 0 && std::fabs(questSpot_->z - pos.z) >= kOtherLevel)
    {
        // Over it or under it still: the way ended on this level.
        ctx.log.info(logcat::Move, "quest mode: no way found to the level of {} - leaving that mob for now", *questSpot_);
        questMobNoWay_[std::exchange(questSpotMob_, 0)] = now;
        markVisited();
    }
    else if (status == NavStatus::Arrived)
    {
        ++ctx.stats.waypointsReached;

        if (singleTarget(questSpotQuest_))
        {
            if (!questWaitSince_)
            {
                questWaitSince_ = now;
                questWaitSaid_ = TimePoint{};
            }

            ctx.navigator.halt();
            return true;
        }

        markVisited(false);
    }

    humanizer_.applyMovement(ctx);
    return true;
}

void NavigationBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    ctx.navigating = true;

    // Back from a fight: the way to the spot is planned again from here.
    if (questSpot_)
    {
        questPath_.clear();
        questSpot_.reset();
    }
    questOffSurfaceSince_.reset();
    questSurfaceCheckAt_ = TimePoint{};

    // Back from a fight or a vendor trip: the old spot may be far behind.
    roamTarget_.reset();
    roamPath_.clear();
    roamStuckAt_.reset();

    if (ctx.config.roam.enabled && ctx.config.roam.circuit)
    {
        rejoinCircuit(s, ctx);
    }

    if (ctx.config.roam.enabled || questingWithoutRoute(ctx))
    {
        // With no spot to roam to, the route is walked from where it stands.
        waypointSince_ = ctx.now();
        recoveringSince_.reset();
        return;
    }

    if (!leadInConsidered_)
    {
        planLeadIn(s, ctx);
    }

    WaypointRoute& route = *ctx.route;
    const Vec3& pos = s.player.position;
    const std::size_t nearest = route.nearestIndex(pos);

    // Far from where we were heading (e.g. after a chase or a corpse run): resume at the nearest waypoint.
    if (nearest != route.index() && distance(pos, route.current()) > distance(pos, route.points()[nearest]) + 30.0f)
    {
        route.setIndex(nearest);
        ctx.log.info(logcat::Move, "resuming route at waypoint {} {}", nearest, route.current());
    }
    else
    {
        ctx.log.debug(logcat::Move, "continuing route to waypoint {} {}", route.index(), route.current());
    }

    waypointSince_ = ctx.now();
    recoveringSince_.reset();
    currentWaypointIndex_ = route.index();
}

void NavigationBehavior::onExit(BotContext& ctx)
{
    onYield(ctx, {});
}

void NavigationBehavior::onYield(BotContext& ctx, std::string_view next)
{
    ctx.navigating = false;
    humanizer_.reset(ctx);
    // Combat, looting or a vendor trip took over: a half-recorded way round an
    // obstacle no longer belongs to whatever leg we resume on.
    corrector_.abandon();

    if ((next != "Pull" && next != "Gather") || !ctx.navigator.hasPreparedPath())
    {
        ctx.navigator.stop();
    }
}

BehaviorStatus NavigationBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (s.player.isCasting || ctx.actions.isCasting())
    {
        ctx.navigator.stop();
        ctx.movement.stop();
        return BehaviorStatus::Running;
    }

    if (questingWithoutRoute(ctx))
    {
        // No profile: the quest spots and the quest givers are the way.
        if (humanizer_.tick(s, ctx))
        {
            ctx.navigator.stop();
            return BehaviorStatus::Running;
        }

        if (!questTick(s, ctx))
        {
            ctx.navigator.stop();
        }

        return BehaviorStatus::Running;
    }

    WaypointRoute& route = *ctx.route;
    const float radius = ctx.config.waypointRadius;

    // A humanizer pause holds the route where it is for a moment.
    if (humanizer_.tick(s, ctx))
    {
        ctx.navigator.stop();
        return BehaviorStatus::Running;
    }

    // Walk the lead-in first when the session started off the grind route.
    if (leadInIndex_ < leadIn_.size())
    {
        const NavStatus lead = ctx.navigator.moveTo(s, leadIn_[leadInIndex_], radius, false);

        if (lead == NavStatus::Arrived)
        {
            ++leadInIndex_;
        }
        else if (lead == NavStatus::Unreachable)
        {
            ctx.log.warn(logcat::Move, "lead-in waypoint {} unreachable - skipping", leadInIndex_);
            ++leadInIndex_;
        }

        if (leadInIndex_ >= leadIn_.size())
        {
            leadIn_.clear();
            leadInIndex_ = 0;
            route.setIndex(route.nearestIndex(s.player.position));
            ctx.log.info(logcat::Move, "joined the grind route at waypoint {}", route.index());
        }

        humanizer_.applyMovement(ctx);
        return BehaviorStatus::Running;
    }

    if (ctx.config.usePathfinding && ctx.config.quest.enabled)
    {
        if (!questTick(s, ctx))
        {
            ctx.navigator.stop();
        }

        return BehaviorStatus::Running;
    }

    if (!ctx.questTargets.empty() && questTick(s, ctx))
    {
        return BehaviorStatus::Running;
    }

    questSpot_.reset();

    if (ctx.config.roam.enabled && ctx.config.roam.circuit && circuitTick(s, ctx))
    {
        return BehaviorStatus::Running;
    }

    if (ctx.config.roam.enabled && roamTick(s, ctx))
    {
        return BehaviorStatus::Running;
    }

    if (route.index() != currentWaypointIndex_)
    {
        currentWaypointIndex_ = route.index();
        waypointSince_ = ctx.now();
        recoveringSince_.reset();
    }

    const NavStatus status = ctx.navigator.moveTo(s, route.current(), radius, false);

    if (status != NavStatus::Recovering)
    {
        recoveringSince_.reset();
    }
    else if (!recoveringSince_)
    {
        recoveringSince_ = ctx.now();
    }

    // Remember the way out of anything the bot jams on, so the next lap can
    // steer round it.
    corrector_.configure(ctx.config.routeCorrector);
    const bool wasRecording = corrector_.recording();
    corrector_.update(s.player.position, route.index(), status == NavStatus::Recovering);

    if (!wasRecording && corrector_.recording() && ctx.obstacles)
    {
        movesAtRecording_ = ctx.obstacles->maneuversLearned();
    }

    if (status == NavStatus::Arrived)
    {
        const std::size_t reached = route.index();
        ++ctx.stats.waypointsReached;
        ctx.log.debug(logcat::Move, "waypoint {} reached", reached);
        ctx.events.publish(WaypointReachedEvent{reached, route.current()});

        // A jump or sidestep learned while working free already handles this
        // obstacle; a detour on top would only take the bot the long way.
        if (corrector_.recording() && ctx.obstacles && ctx.obstacles->maneuversLearned() != movesAtRecording_)
        {
            ctx.log.debug(logcat::Stuck, "a learned move handles the obstacle before waypoint {} - no detour", reached);
            corrector_.abandon();
        }

        // Splice before advancing: the detour belongs on the leg just walked.
        const std::size_t fixes = corrector_.corrections().size();
        corrector_.onWaypointReached(reached, route, &ctx.log);

        if (ctx.obstacles && corrector_.corrections().size() > fixes)
        {
            const RouteCorrection& fix = corrector_.corrections().back();
            ctx.obstacles->learnDetour(LearnedDetour{fix.from, fix.to, fix.stuckAt, fix.detour}, &ctx.log);
        }

        route.advance();
        waypointSince_ = ctx.now();
        recoveringSince_.reset();
        currentWaypointIndex_ = route.index();

        if (route.finished())
        {
            ctx.navigator.stop();
            ctx.log.info(logcat::Move, "route finished");
            return BehaviorStatus::Success;
        }

        ctx.navigator.moveTo(s, route.current(), radius, false);
    }
    else
    {
        const bool stalled = recoveringSince_ && (ctx.now() - *recoveringSince_ > Duration(20000));

        if (status == NavStatus::Unreachable || stalled)
        {
            if (stalled)
            {
                ctx.log.warn(logcat::Move, "stalled on waypoint {} for 20s - skipping waypoint", route.index());
                ctx.navigator.stop();
                route.advance();
            }
            else if (ctx.navigator.lastFailure() == RecoveryResult::RequestFallback)
            {
                route.retreat();
                ctx.log.warn(logcat::Move, "falling back to previous waypoint {}", route.index());
            }
            else
            {
                ctx.log.warn(logcat::Move, "skipping unreachable waypoint {}", route.index());
                route.advance();
            }

            waypointSince_ = ctx.now();
            recoveringSince_.reset();
            currentWaypointIndex_ = route.index();

            if (route.finished())
            {
                ctx.navigator.stop();
                ctx.log.info(logcat::Move, "route finished");
                return BehaviorStatus::Success;
            }
        }
    }

    humanizer_.applyMovement(ctx);
    return BehaviorStatus::Running;
}

bool SurfaceBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // Rising at a little under swimming speed, with ten seconds to spare -
    // five of them a cast under way may still take: swimming up breaks it.
    constexpr float kRiseSpeed = 4.0f;
    const bool casting = !rising_ && (s.player.isCasting || ctx.actions.isCasting());
    const float kSpare = casting ? 5.0f : 10.0f;
    const auto& b = ctx.breath;

    if (!b.since || !b.left || s.player.isDeadOrGhost())
    {
        return false;
    }

    // Without the map's water, how deep is not known: the spare alone.
    const float depth = b.surface ? std::max(0.0f, *b.surface - s.player.position.z) : 0.0f;
    return rising_ || *b.left < depth / kRiseSpeed + kSpare;
}

BehaviorStatus SurfaceBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.breath.since)
    {
        ctx.movement.setVertical(0);
        rising_ = false;
        lastZ_.reset();
        still_ = 0;
        retryAt_.reset();
        return BehaviorStatus::Success;
    }

    if (!rising_)
    {
        rising_ = true;
        ctx.breath.surfacedAt = ctx.now();
        ctx.log.warn(logcat::Move, "{:.0f} s of breath left - up for air", ctx.breath.left.value_or(0));
    }

    // Up no further: at the surface, where holding the key only jumps - or
    // under something. Let go, and try again in a while: still under water
    // then, the way up may be clear.
    constexpr Duration kRetry{3000};
    const bool still = lastZ_ && s.player.position.z - *lastZ_ < 0.05f;

    if (still && ++still_ >= 3)
    {
        if (!retryAt_)
        {
            retryAt_ = ctx.now() + kRetry;
        }

        if (ctx.now() < *retryAt_)
        {
            ctx.movement.setVertical(0);
            return BehaviorStatus::Running;
        }

        still_ = 0;
        retryAt_.reset();
    }

    if (!still)
    {
        still_ = 0;
        retryAt_.reset();
    }

    lastZ_ = s.player.position.z;
    ctx.navigator.halt();
    ctx.movement.setVertical(1);
    return BehaviorStatus::Running;
}

void SurfaceBehavior::onExit(BotContext& ctx)
{
    rising_ = false;
    lastZ_.reset();
    still_ = 0;
    retryAt_.reset();
    ctx.movement.setVertical(0);
}

} // namespace og
