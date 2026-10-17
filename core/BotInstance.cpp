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
 *  File: BotInstance.cpp
 *
 *  Desc:
 *  Implementation of BotInstance (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/BotInstance.h"

#include <limits>
#include <format>

#include "og/core/Events.h"
#include "og/core/QuestFocus.h"
#include "og/core/behaviors/Behaviors.h"
#include "og/navigation/Roaming.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>
#include <utility>

namespace og
{

namespace
{

TargetFilterConfig filterFromProfile(const GliderProfile& p)
{
    TargetFilterConfig f;
    f.names = p.targets;
    f.excludedNames = p.exclusions;
    f.minLevel = p.minLevel;
    f.maxLevel = p.maxLevel;

    if (p.options.minPullLevel > 0)
    {
        f.minLevel = std::max(f.minLevel, p.options.minPullLevel);
    }

    if (p.options.maxPullLevel > 0)
    {
        f.maxLevel = std::min(f.maxLevel, p.options.maxPullLevel);
    }

    f.attackElites = p.options.attackElites;
    // 0: no limit - whatever the client sees.
    f.maxDistance = p.options.pullDistance > 0 ? p.options.pullDistance : std::numeric_limits<float>::max();
    return f;
}

std::vector<AvoidZone> nonQuestMobZones(const GameSnapshot& s, const std::vector<QuestTarget>& quests, Guid engaged)
{
    std::vector<std::uint32_t> needed;

    for (const QuestTarget& quest : quests)
    {
        if (!quest.done)
        {
            needed.insert(needed.end(), quest.mobs.begin(), quest.mobs.end());
        }
    }

    if (needed.empty())
    {
        return {};
    }

    std::vector<AvoidZone> zones;

    for (const UnitState& mob : s.units)
    {
        if (!mob.isAlive() || mob.isPlayer || mob.isCritter || mob.reaction != Reaction::Hostile || mob.isTappedByOther || !mob.isAttackable || mob.ownerGuid || mob.guid == engaged || mob.targetGuid == s.player.guid || distance2D(mob.position, s.player.position) > 45.0f || std::find(needed.begin(), needed.end(), mob.entry) != needed.end())
        {
            continue;
        }

        zones.push_back({mob.position, 11.0f, mob.name, false, AvoidZone::reachOf(11.0f)});
    }

    return zones;
}

// Avoid all mobs during travels: every hostile mob near by that is not to be
// pulled - in Quest Mode the ones no quest wants - gone round by where it
// notices the character and some yards more.
std::vector<AvoidZone> travelMobZones(const GameSnapshot& s, const TargetSelector& targets, const std::vector<QuestTarget>& quests, Guid engaged, float margin)
{
    constexpr float kNear = 60.0f;
    std::vector<AvoidZone> zones;
    // A quest's mobs - to kill, or for what they drop - are never gone round.
    std::vector<std::uint32_t> needed;

    for (const QuestTarget& quest : quests)
    {
        if (!quest.done)
        {
            needed.insert(needed.end(), quest.mobs.begin(), quest.mobs.end());
        }
    }

    for (const UnitState& mob : s.units)
    {
        if (!mob.isAlive() || mob.isPlayer || mob.isCritter || mob.reaction != Reaction::Hostile || mob.isTappedByOther || !mob.isAttackable || mob.ownerGuid || mob.guid == engaged || mob.targetGuid == s.player.guid || mob.name.starts_with("[DNT]") || distance2D(mob.position, s.player.position) > kNear)
        {
            continue;
        }

        // Not one busy with something else (another player, an NPC): it does
        // not come for us, and is not gone round.
        if (mob.targetGuid != 0 || targets.isValidTarget(mob, s) || std::find(needed.begin(), needed.end(), mob.entry) != needed.end())
        {
            continue;
        }

        const float reach = TargetSelector::aggroRadius(mob.level > 0 ? mob.level : s.player.level + 10, s.player.level);
        zones.push_back({mob.position, reach + margin, mob.name, false, AvoidZone::reachOf(reach + margin)});
    }

    return zones;
}

} // namespace

BotInstance::BotInstance(BotInstanceOptions options, std::unique_ptr<IGameStateProvider> provider, std::unique_ptr<IInputBackend> input, std::shared_ptr<const IClock> clock) : name_(options.name), clock_(std::move(clock)), log_(options.name), provider_(std::move(provider)), input_(std::move(input)), actions_(std::move(options.actions), *input_, *clock_, log_, &events_), movement_(actions_, *clock_, log_, options.movement), navigator_(directNavigation_, movement_, *clock_, log_, &events_, options.navigator), targetBlacklist_(*clock_), objectBlacklist_(*clock_), areaBlacklist_(*clock_), targets_(TargetFilterConfig{}, targetBlacklist_), objectives_(log_, &events_), config_(options.config), context_{*clock_, log_, events_, actions_, movement_, navigator_, targets_, targetBlacklist_, objectBlacklist_, areaBlacklist_, objectives_, stats_, config_, provider_.get()}, engine_(*provider_, context_)
{
    stats_.started = clock_->now();
    actions_.setClientCooldowns(
        [this](int slot) -> std::optional<ActionService::ClientCooldown>
        {
            if (const auto shown = provider_->actionCooldown(slot))
            {
                return ActionService::ClientCooldown{shown->remaining, shown->duration, shown->shared};
            }

            return std::nullopt;
        });

    for (auto& b : createDefaultBehaviors())
    {
        engine_.addBehavior(std::move(b));
    }

    subscriptions_.push_back(events_.subscribe<StuckDetectedEvent>([this](const StuckDetectedEvent&) { ++stats_.stuckEvents; }));
    subscriptions_.push_back(events_.subscribe<ActionExecutedEvent>([this](const ActionExecutedEvent&) { ++stats_.actionsExecuted; }));
    status_.name = name_;
    navigator_.setObstacleMemory(&obstacles_);
    context_.obstacles = &obstacles_;
    targets_.setAvoidedPosition([this](const Vec3& position) { return obstacles_.avoids(position); });
    targets_.setOutOfBounds(
        [this](const Vec3& position)
        {
            if (config_.usePathfinding && config_.quest.enabled)
            {
                return false;
            }

            const RoamConfig& roam = context_.config.roam;

            if (!roam.enabled || !roam.pullInsideOnly || !context_.route)
            {
                return false;
            }

            const std::vector<Vec3>& area = context_.route->points();
            return (outlinesArea(area) && !insideRoute(area, position)) || !insideZone(&navigator_.provider(), roam.zone, position);
        });
    // Action bar slots the game has no key bound to are pressed with UseAction().
    actions_.setLuaRunner([provider = provider_.get()](std::string_view script) { return provider->supportsLua() && provider->executeLua(script); });
    // Without Lua, on a client whose UI can be read, they are clicked instead,
    // on the bar the slot shows on: what the bindings addon's keys did.
    actions_.setSlotClicker(
        [this](int slot)
        {
            const std::string button = actionButtonFrame(slot);
            return !button.empty() && !provider_->supportsLua() && provider_->supportsUi() && context_.clickUiFrame(button);
        });
}

BotInstance::~BotInstance()
{
    movement_.stop();
    actions_.releaseAll();
    syncClientBindings(false);
}

void BotInstance::setProfile(GliderProfile profile)
{
    findTemporaryVendor_ = true;

    if (config_.usePathfinding)
    {
        profile.learned = {};
    }

    profile_ = std::move(profile);
    route_ = WaypointRoute(profile_->waypoints, profile_->routeMode);
    obstacles_.setLearned(profile_->learned);
    // Kept out of when questing too: a zone painted over a camp was walked
    // straight through for a herb beyond it. Only the visit to a quest NPC
    // standing in one goes in (QuestHandInBehavior).
    obstacles_.setNoGoZones(profile_->noGoZones);
    navigator_.stop();
    observedRevision_ = obstacles_.revision();

    if (const std::size_t detours = applyDetours(route_, profile_->learned.detours))
    {
        log_.info(logcat::Profile, "{} learned detour(s) added to the route", detours);
    }

    if (!profile_->learned.maneuvers.empty())
    {
        log_.info(logcat::Profile, "{} learned move(s) past obstacles", profile_->learned.maneuvers.size());
    }

    refreshTargetFilter();
    context_.profile = &*profile_;
    context_.route = route_.empty() ? nullptr : &route_;
    objectives_.setMain(Objective{"GrindRoute"});

    if (auto* nav = dynamic_cast<NavigationBehavior*>(engine_.findBehavior("Navigation")))
    {
        nav->resetCircuit();
    }

    log_.info(logcat::Profile, "loaded profile '{}' ({} waypoints, targets: {})", profile_->name, profile_->waypoints.size(), profile_->targets.empty() ? std::string("any") : std::to_string(profile_->targets.size()));

    if (config_.usePathfinding)
    {
        log_.info(logcat::Profile, "pathfinding targets: level {}-{}, scan range {:.0f} yd, quest mode {}", targets_.config().minLevel, targets_.config().maxLevel, targets_.config().maxDistance, config_.quest.enabled ? "on" : "off");
    }

    events_.publish(ProfileLoadedEvent{profile_->name});
}

void BotInstance::setProfileOptions(GliderProfile::Options options)
{
    if (!profile_)
    {
        return;
    }

    profile_->options = std::move(options);
    refreshTargetFilter();
}

void BotInstance::refreshTargetFilter()
{
    if (!profile_)
    {
        return;
    }

    TargetFilterConfig filter = filterFromProfile(*profile_);
    filter.blacklistDuration = config_.blacklistDuration;
    filter.skipGrayMobs = config_.skipGrayMobs;
    filter.attackElites = filter.attackElites || config_.pullElites;
    filter.eliteEntries = eliteEntries_;
    // A party follower pulls nothing of its own: it fights what the leader does.
    filter.passiveMode = config_.passiveMode || config_.party.follower();
    filter.maxPackAdds = config_.maxPackAdds;
    filter.addRadius = config_.packRadius;
    filter.protectRange = config_.protectRange;
    filter.protectAngle = config_.protectAngle;
    targets_.setConfig(std::move(filter));
}

void BotInstance::reloadProfile(GliderProfile profile)
{
    setProfile(std::move(profile));

    // Carry on from where the character is, not from the first waypoint.
    if (const auto& s = engine_.lastSnapshot(); s && !route_.empty())
    {
        route_.resumeFrom(s->player.position);
    }
}

void BotInstance::setCombatClass(std::unique_ptr<ICombatClass> combatClass)
{
    combatClass_ = std::move(combatClass);
    context_.combatClass = combatClass_.get();

    if (combatClass_)
    {
        log_.info(logcat::Profile, "combat class '{}'", combatClass_->name());
    }
}

void BotInstance::tick()
{
    if (finished_)
    {
        return;
    }

    if (config_.usePathfinding && !clientNavigation_)
    {
        pausedByQuesting_ = false;
        engine_.pause();
    }

    if (config_.usePathfinding && clientNavigation_ && &navigator_.provider() != clientNavigation_.get())
    {
        navigator_.setProvider(*clientNavigation_);
    }
    else if (!config_.usePathfinding && navigator_.provider().usesClientGeometry())
    {
        navigator_.setProvider(directNavigation_);
    }

    INavigationProvider* navigation = &navigator_.provider();
    // Roads are for travel - quest spots, quest givers, vendors, Go To - not
    // for roaming the grind area, where the bot goes about the ground itself.
    // Nor on a corpse run: a ghost goes the short way back to its body.
    const auto& seen = engine_.lastSnapshot();
    const bool ghost = seen && seen->player.isDeadOrGhost();
    navigation->setUseRoads(config_.useRoads && !ghost && !(!config_.quest.enabled && engine_.currentName() == "Navigation"));
    // Over a bridge rather than through the water - but not on a corpse run.
    navigation->setAvoidWater(!ghost);
    // A tauren (race 6) is too tall for the low eaves others pass under.
    navigation->setTall(seen && seen->player.race == 6);
    navigator_.setSway(config_.humanizer.enabled ? config_.humanizer.sway : 0.0f);

    if (config_.usePathfinding && engine_.state() != EngineState::Paused && !pathfindingOrigin_)
    {
        const auto snapshot = provider_->capture();

        if (snapshot && snapshot->mapId && (!config_.quest.enabled || snapshot->zoneId != 0))
        {
            navigation->setMap(snapshot->mapId);
            config_.roam.enabled = true;

            if (snapshot->player.isDeadOrGhost())
            {
                // Started dead: the corpse run comes first. Where the roaming
                // and the questing are centred is taken once alive again -
                // not at the graveyard the ghost stands in.
                if (!deadAtStartSaid_)
                {
                    log_.info(logcat::Move, "dead at Start: running back to the corpse first - {} once alive again", config_.quest.enabled ? "questing starts" : "grinding starts");
                    deadAtStartSaid_ = true;
                }
            }
            else
            {
                pathfindingOrigin_ = snapshot->player.position;

                if (questSource_ && snapshot->zoneId)
                {
                    questSource_->setStartZone(snapshot->zoneId);
                }

                if (std::exchange(deadAtStartSaid_, false))
                {
                    log_.info(logcat::Move, "alive again: {} from here", config_.quest.enabled ? "questing" : "grinding");
                }
            }
        }
        else
        {
            pausedByQuesting_ = false;
            engine_.pause();
            log_.warn(logcat::Move, "Pathfinding paused: waiting for a known map/zone at Start.");
        }
    }

    if (config_.quest.enabled && !questSource_)
    {
        pausedByQuesting_ = false;
        engine_.pause();

        if (!questDataMissingSaid_)
        {
            log_.warn(logcat::Profile, "Questing paused: quest data is unavailable. Reload the quest data before starting.");
            questDataMissingSaid_ = true;
        }
    }
    else
    {
        questDataMissingSaid_ = false;
    }

    // Claimed before the engine presses anything with them.
    const bool controlsClient = engine_.state() != EngineState::Paused || engine_.manualTravel();
    syncClientBindings(controlsClient && !engine_.waitingForGame());
    allowChatScripts();
    runClientSetupChat(controlsClient && !engine_.waitingForGame());
    obstacles_.setLearning(config_.routeCorrector.enabled && !config_.usePathfinding);
    obstacles_.setPathfinding(config_.usePathfinding);
    const auto currentSnapshot = engine_.lastSnapshot() ? engine_.lastSnapshot() : provider_->capture();

    if (currentSnapshot)
    {
        if (findTemporaryVendor_)
        {
            findTemporaryVendor(*currentSnapshot);
        }

        // The way goes round where each mob too strong to fight would notice
        // us (watchStrongMobs keeps track of them).
        std::vector<AvoidZone> mobZones;

        if (config_.avoidStrongMobs)
        {
            mobZones = context_.strongMobs.zones(config_.strongMobMargin, context_.combat.engaged);
        }

        if (config_.avoidMobsTravelling && config_.usePathfinding)
        {
            auto avoid = travelMobZones(*currentSnapshot, targets_, context_.questTargets, context_.combat.engaged, config_.packMargin);
            mobZones.insert(mobZones.end(), avoid.begin(), avoid.end());
        }
        else if (config_.quest.enabled && config_.usePathfinding)
        {
            auto avoid = nonQuestMobZones(*currentSnapshot, context_.questTargets, context_.combat.engaged);
            mobZones.insert(mobZones.end(), avoid.begin(), avoid.end());
        }

        // A ghost is seen by no mob: the corpse run goes the short way, not
        // round every mob in the cave the body lies in.
        std::vector<AvoidZone> packZones;

        if (!currentSnapshot->player.isDeadOrGhost())
        {
            for (const auto& z : context_.packZones)
            {
                if (!z.ignored)
                {
                    packZones.push_back(z);
                }
            }
        }
        else
        {
            mobZones.clear();
        }

        // A mob is kept clear of on its own level, not on the hill over its cave.
        for (AvoidZone& z : mobZones)
        {
            z.reach = AvoidZone::reachOf(z.radius);
        }

        navigator_.setMobZones(std::move(mobZones));
        navigator_.setPackZones(std::move(packZones));
    }
    else
    {
        navigator_.setMobZones({});
        navigator_.setPackZones({});
    }

    engine_.tick(
        [this](const GameSnapshot& snapshot)
        {
            reportReceivedItems(snapshot);
            reportTargetCasts(snapshot);
            reportFleeing(snapshot);
            context_.party.update(snapshot, config_.party, clock_->now());
            assistLeader(snapshot);
            partyChat(snapshot);
            partyVendor(snapshot);
            watchRespawns(snapshot);
            watchStrongMobs(snapshot);
            watchBreath(snapshot);
            updateQuestTargets(snapshot);
        });

    if (context_.pauseRequest)
    {
        log_.warn(logcat::Engine, "paused: {} - Resume to go on", *context_.pauseRequest);
        context_.pauseRequest.reset();
        pausedByQuesting_ = false;
        engine_.pause();
        navigator_.stop();
    }

    if (!config_.movementTrace.empty())
    {
        if (!movementTrace_.isOpen() && !std::exchange(movementTraceTried_, true) && !movementTrace_.open(config_.movementTrace))
        {
            log_.warn(logcat::Engine, "movement trace: cannot write {}", config_.movementTrace);
        }

        if (const auto& moved = engine_.lastSnapshot(); moved && !engine_.waitingForGame())
        {
            movementTrace_.add(*moved, clock_->now(), engine_.state() != EngineState::Paused);
        }
    }

    // A loading screen (logging in again) takes them from the client.
    if (engine_.waitingForGame())
    {
        bagsOf_ = 0; // the bags are read anew after it, not received
        clientBindingsClaimed_ = false;
        clientSetupAt_.reset();
        actions_.forgetPreparations();
    }

    // Only while actually botting: attached and paused, the player is at the keys.
    else if (const auto& s = engine_.lastSnapshot(); s && engine_.state() != EngineState::Paused)
    {
        playerAlert_.update(*s, config_.playerAlert, clock_->now(), log_);
        // Nobody is watched for following on a vendor trip: towns are full of
        // players walking the same way.
        FollowerConfig follower = config_.follower;

        if (context_.vendorTrip)
        {
            follower.enabled = false;
        }

        context_.followerHold = followerWatch_.update(*s, follower, clock_->now(), log_);
        buffThanks_.update(*s, config_.buffThanks, clock_->now(), [this] { return context_.randomBetween(config_.buffThanks.delayMin, config_.buffThanks.delayMax); }, [this](const std::string& line) { return actions_.chat(line); }, log_);
        reportSkippedTargets(*s);
    }

    if (obstacles_.revision() != observedRevision_)
    {
        observedRevision_ = obstacles_.revision();

        if (learnedObserver_)
        {
            learnedObserver_(obstacles_.learned());
        }
    }

    checkStopAtLevel();
    checkStopAfterDeaths();
    updateStatus();
}

void BotInstance::findTemporaryVendor(const GameSnapshot& s)
{
    findTemporaryVendor_ = false;

    if (config_.usePathfinding)
    {
        context_.temporaryVendor.reset();
        return;
    }

    if (!profile_ || !config_.vendor.enabled || !profile_->vendorRoute().empty())
    {
        context_.temporaryVendor.reset();
        return;
    }

    // The nearest vendor on the radar that is not shut in: a No Go zone or an
    // obstacle round it would have the bot stuck on the way.
    const UnitState* best = nullptr;

    for (const auto& u : s.units)
    {
        if (u.isPlayer || !u.isAlive() || !u.isVendor || u.reaction == Reaction::Hostile || u.name.empty())
        {
            continue;
        }

        const bool noGo = std::any_of(profile_->noGoZones.begin(), profile_->noGoZones.end(), [&](const AvoidZone& z) { return distance2D(z.center, u.position) <= z.radius; });

        if (noGo || obstacles_.avoids(u.position))
        {
            continue;
        }

        if (!best || distance(s.player.position, u.position) < distance(s.player.position, best->position))
        {
            best = &u;
        }
    }

    if (!best)
    {
        if (context_.temporaryVendor || !temporaryVendorMissingSaid_)
        {
            log_.info(logcat::Loot, "no vendor route in the profile, and no vendor in sight to use instead");
        }

        temporaryVendorMissingSaid_ = true;
        context_.temporaryVendor.reset();
        return;
    }

    temporaryVendorMissingSaid_ = false;
    context_.temporaryVendor = BotContext::TemporaryVendor{best->name, best->position, best->canRepair};
    log_.info(logcat::Loot, "no vendor route in the profile: {} ({:.0f} yd{}) is the vendor for this session", best->name, distance(s.player.position, best->position), best->canRepair ? ", repairs" : "");
}

void BotInstance::watchStrongMobs(const GameSnapshot& s)
{
    // The mobs too strong to fight (above the target filter's maximum level),
    // followed as they walk, before anything is chosen this tick: no mob
    // standing where one of them would notice us is pulled. Not the ones
    // already fought or coming - there is no keeping away from those.
    std::vector<TargetSelector::DangerZone> dangerZones;
    context_.strongMobs.update(s, config_.avoidStrongMobs ? targets_.config().maxLevel : std::numeric_limits<int>::max(), clock_->now());

    context_.nearMobs.update(s, config_.addAvoidance ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max(), clock_->now());

    for (const StrongMob& mob : context_.strongMobs.mobs())
    {
        if (mob.onUs || mob.guid == context_.combat.engaged)
        {
            continue;
        }

        // A fight at the edge of its reach drifts: some yards more.
        constexpr float kPullMargin = 5.0f;
        dangerZones.push_back({mob.position, mob.aggroRange + kPullMargin, mob.name});

        if (const Vec3 to = mob.heading(); distance2D(to, mob.position) > 1.0f)
        {
            dangerZones.push_back({to, mob.aggroRange + kPullMargin, mob.name});
        }

        if (std::find(context_.strongMobs.spotted().begin(), context_.strongMobs.spotted().end(), mob.guid) != context_.strongMobs.spotted().end())
        {
            log_.info(logcat::Target, "keeping clear of {} (level {}, {:.0f} yd off): too strong to fight, it notices us from {:.0f} yd", mob.name, mob.level > 0 ? std::to_string(mob.level) : std::string("??"), mob.distance, mob.aggroRange);
        }
    }

    // Avoid packed mobs: a pack, as a camp of gnolls round a fire, is more
    // than a mob to leave alone. A mob standing within reach of one of them
    // is not pulled either, and the way goes round them - the bot walked into
    // the camp for the gnolls about it.
    context_.packZones.clear();
    markUnderRoof(s);

    if (config_.maxPackAdds > 0 && !s.player.isDeadOrGhost())
    {
        for (const UnitState& u : s.units)
        {
            if (!u.isAlive() || u.isPlayer || u.isCritter || u.ownerGuid != 0 || u.reaction != Reaction::Hostile || u.name.starts_with("[DNT]"))
            {
                continue;
            }

            if (u.guid == context_.combat.engaged || u.targetGuid == s.player.guid || s.isAttacker(u) || !targets_.packed(u, s))
            {
                continue;
            }

            const bool allGrey = targets_.packAllGrey(u, s);
            const float reach = TargetSelector::aggroRadius(u.level > 0 ? u.level : s.player.level + 10, s.player.level);
            constexpr float kPackPullMargin = 5.0f;
            if (!allGrey)
            {
                dangerZones.push_back({u.position, reach + kPackPullMargin, u.name, true});
            }
            context_.packZones.push_back({u.position, reach + config_.packMargin, u.name + " (a pack)", allGrey, AvoidZone::reachOf(reach + config_.packMargin)});
        }
    }

    // Where a mob comes back at once: not gone into again, nor pulled at,
    // while no quest needs it.
    for (const FastRespawn& f : context_.fastRespawns)
    {
        if (clock_->now() < f.until && !context_.questNeeds(f.entry))
        {
            dangerZones.push_back({f.at, FastRespawn::kRadius, f.name, false, true});
            context_.packZones.push_back({f.at, FastRespawn::kRadius, f.name + " (respawns at once)", false, AvoidZone::reachOf(FastRespawn::kRadius)});
        }
    }

    targets_.setDangerZones(std::move(dangerZones));
}

void BotInstance::watchRespawns(const GameSnapshot& s)
{
    const TimePoint now = clock_->now();
    constexpr Duration kWithin{10000};
    constexpr Duration kFor{300000};
    constexpr float kNear = 20.0f;
    std::erase_if(context_.recentKills, [&](const RecentKill& k) { return now - k.when > kWithin; });
    std::erase_if(context_.fastRespawns, [&](const FastRespawn& f) { return now >= f.until; });
    std::unordered_set<Guid> seen;

    for (const UnitState& u : s.units)
    {
        seen.insert(u.guid);

        if (!u.isAlive() || u.isPlayer || u.entry == 0 || u.reaction == Reaction::Friendly)
        {
            continue;
        }

        // The one killed alive again, or one of its kind come into sight
        // right by it - out of sight a mob shows up a hundred yards off, not
        // twenty.
        for (const RecentKill& k : context_.recentKills)
        {
            const bool again = u.guid == k.guid || (!respawnSeen_.contains(u.guid) && u.entry == k.entry && distance2D(u.position, k.at) <= kNear);

            if (!again || u.entry != k.entry || context_.fastRespawnAt(k.at, k.entry))
            {
                continue;
            }

            context_.fastRespawns.push_back({k.at, k.entry, k.name, now + kFor});
            log_.info(logcat::Target, "{} came back {:.0f}s after the kill - what is needed is looted, and the bot leaves the place for {} minutes", k.name, toSeconds(now - k.when), std::chrono::duration_cast<std::chrono::minutes>(kFor).count());
        }
    }

    respawnSeen_ = std::move(seen);
}

void BotInstance::markUnderRoof(const GameSnapshot& s)
{
    std::unordered_set<Guid> under;
    INavigationProvider& map = navigator_.provider();

    if (config_.maxPackAdds <= 0 || !map.usesClientGeometry())
    {
        roofSeen_.clear();
        targets_.setUnderRoof({});
        return;
    }

    std::unordered_map<Guid, std::pair<Vec3, bool>> seen;
    const UnitState* first = nullptr;

    for (const UnitState& u : s.units)
    {
        if (!u.isAlive() || u.isPlayer || u.isCritter || u.reaction != Reaction::Hostile)
        {
            continue;
        }

        // Looked up once where it stands; again when it has walked off.
        auto it = roofSeen_.find(u.guid);
        bool roofed = false;

        if (it != roofSeen_.end() && distance(it->second.first, u.position) < 4.0f)
        {
            roofed = it->second.second;
        }
        else
        {
            // Ground straight over the mob, from a little over its head to a
            // hill a hundred yards high: the rock over a mine, a cave.
            // A map tile that cannot be built: not known, so open sky.
            try
            {
                for (const float up : {20.0f, 50.0f, 110.0f})
                {
                    const auto ground = map.groundBelow({u.position.x, u.position.y, u.position.z + up});

                    if (ground && *ground > u.position.z + 6.0f)
                    {
                        roofed = true;
                        break;
                    }
                }
            }
            catch (const std::exception&)
            {
                roofed = false;
            }
        }

        seen[u.guid] = {u.position, roofed};

        if (roofed)
        {
            under.insert(u.guid);
            first = first ? first : &u;
        }
    }

    if (first && !underRoofSaid_)
    {
        log_.info(logcat::Target, "{} and the mobs about it stand under a roof (a mine, a cave): no way round them there - not kept clear of as a pack", first->name);
    }

    underRoofSaid_ = first != nullptr;
    roofSeen_ = std::move(seen);
    targets_.setUnderRoof(std::move(under));
}

void BotInstance::partyChat(const GameSnapshot& s)
{
    context_.partyHeard.clear();
    const auto lines = provider_->takeChat();

    if (!config_.party.enabled)
    {
        context_.partyOutbox.clear();
        return;
    }

    // Only the members on the list are listened to: anyone else saying the
    // same learns nothing of where the bot is.
    for (const ChatMessage& line : lines)
    {
        if (line.channel != "Party" || !context_.party.named(line.sender))
        {
            continue;
        }

        if (const auto heard = context_.partyChat.hear(line.text))
        {
            log_.debug(logcat::Combat, "party: {} says {}", line.sender, PartyChat::nameOf(heard->kind));
            context_.partyHeard.push_back({line.sender, *heard});
        }
    }

    // The leader: where it is, to a member that is lost, once out of a fight.
    if (config_.party.leads())
    {
        for (const auto& line : context_.partyHeard)
        {
            if (line.heard.kind == PartyChat::Kind::Lost && std::find(partyLostFrom_.begin(), partyLostFrom_.end(), line.from) == partyLostFrom_.end())
            {
                partyLostFrom_.push_back(line.from);
            }
        }

        if (!partyLostFrom_.empty() && !s.inCombat() && s.attackers().empty())
        {
            log_.info(logcat::Move, "party: telling {} where we are", partyLostFrom_.front());
            context_.partySay(PartyChat::Kind::Position, s.player.position);
            partyLostFrom_.clear();
        }
    }

    // A follower: the leader's answer, a place to walk to.
    if (config_.party.follower())
    {
        for (const auto& line : context_.partyHeard)
        {
            const PartyMember* from = context_.party.named(line.from);

            if (line.heard.kind == PartyChat::Kind::Position && line.heard.place && from && from->leader)
            {
                context_.partyRegroup = Vec3{line.heard.place->x, line.heard.place->y, s.player.position.z};
                log_.info(logcat::Move, "party: {} is at {:.0f} {:.0f} - going there", line.from, line.heard.place->x, line.heard.place->y);
            }
        }
    }

    // One line every two seconds, typed as a player does.
    constexpr Duration kSayEvery{2000};

    if (!context_.partyOutbox.empty() && clock_->now() - partySaidAt_ >= kSayEvery)
    {
        partySaidAt_ = clock_->now();
        actions_.chat("/p " + context_.partyOutbox.front());
        context_.partyOutbox.erase(context_.partyOutbox.begin());
    }
}

void BotInstance::partyVendor(const GameSnapshot& s)
{
    auto& queue = context_.partyVendorQueue;

    if (!config_.party.enabled)
    {
        queue.clear();
        context_.partyVendorAsked = context_.partyVendorTurn = context_.partyVendorForMember = false;
        return;
    }

    const TimePoint now = clock_->now();
    const auto same = [](std::string_view a, std::string_view b) { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); }); };
    const auto inQueue = [&](std::string_view name) { return std::find_if(queue.begin(), queue.end(), [&](const auto& ask) { return same(ask.name, name); }); };
    const std::string& me = s.player.name;

    for (const auto& line : context_.partyHeard)
    {
        if (line.heard.kind == PartyChat::Kind::Vendor && inQueue(line.from) == queue.end())
        {
            log_.info(logcat::Loot, "party: {} needs a vendor", line.from);
            queue.push_back({line.from, now});
            context_.partySay(PartyChat::Kind::Ack);
        }
        else if (line.heard.kind == PartyChat::Kind::VendorDone)
        {
            if (const auto it = inQueue(line.from); it != queue.end())
            {
                queue.erase(it);
            }
        }
    }

    // A member that never says it is done is not waited for for ever.
    constexpr Duration kVendorWait{180000};
    std::erase_if(queue,
                  [&](const auto& ask)
                  {
                      const bool old = !same(ask.name, me) && now - ask.at > kVendorWait;

                      if (old)
                      {
                          log_.warn(logcat::Loot, "party: {} has not said it is done at the vendor - going on", ask.name);
                      }

                      return old;
                  });

    // Ourselves: once, when the bags are full or the gear wants repair.
    const bool need = vendorNeeded(s, config_.vendor);

    if (need && !context_.partyVendorAsked && !s.player.isDeadOrGhost() && !me.empty())
    {
        log_.info(logcat::Loot, "party: asking the party for a vendor trip");
        context_.partyVendorAsked = true;
        context_.partySay(PartyChat::Kind::Vendor);

        if (inQueue(me) == queue.end())
        {
            queue.push_back({me, now});
        }
    }

    if (config_.party.leads())
    {
        // A trip for the members that asked; its own full bags start its own.
        context_.partyVendorForMember = std::any_of(queue.begin(), queue.end(), [&](const auto& ask) { return !same(ask.name, me); });

        if (context_.partyVendorAsked && !need && !context_.vendorTrip)
        {
            context_.partyVendorAsked = false;

            if (const auto it = inQueue(me); it != queue.end())
            {
                queue.erase(it);
            }
        }

        return;
    }

    if (!config_.party.follower())
    {
        return;
    }

    // Our turn: first of the followers that asked, with the leader by a vendor.
    const PartyMember* leader = context_.party.leader();
    const UnitState* vendor = nullptr;
    constexpr float kByTheVendor = 20.0f;

    if (leader && leader->inSight)
    {
        for (const UnitState& u : s.units)
        {
            if (u.isVendor && !u.isPlayer && u.isAlive() && distance2D(u.position, leader->position) <= kByTheVendor && (!vendor || distance2D(u.position, leader->position) < distance2D(vendor->position, leader->position)))
            {
                vendor = &u;
            }
        }
    }

    const auto first = std::find_if(queue.begin(), queue.end(), [&](const auto& ask) { return !leader || !same(ask.name, leader->name); });
    const bool turn = context_.partyVendorAsked && vendor && first != queue.end() && same(first->name, me);

    if (context_.partyVendorTurn && context_.vendorTrip)
    {
        partyVendorBegan_ = true;
    }

    // Our trip over: done, and the next one's turn.
    if (context_.partyVendorTurn && partyVendorBegan_ && !context_.vendorTrip)
    {
        log_.info(logcat::Loot, "party: done at the vendor");
        context_.partySay(PartyChat::Kind::VendorDone);
        context_.partyVendorTurn = context_.partyVendorAsked = partyVendorBegan_ = false;
        context_.temporaryVendor.reset();

        if (const auto it = inQueue(me); it != queue.end())
        {
            queue.erase(it);
        }

        return;
    }

    if (turn && !context_.partyVendorTurn)
    {
        log_.info(logcat::Loot, "party: our turn at {}", vendor->name);
        context_.temporaryVendor = BotContext::TemporaryVendor{vendor->name, vendor->position, vendor->canRepair, vendor->entry};
        context_.partyVendorTurn = true;
    }
}

void BotInstance::assistLeader(const GameSnapshot& s)
{
    if (!config_.party.follower() || config_.party.role != PartyConfig::Role::Damage || s.player.isDeadOrGhost())
    {
        return;
    }

    const PartyMember* leader = context_.party.leader();

    if (!leader || !leader->inSight || !leader->inCombat || leader->target == 0 || leader->target == context_.combat.engaged)
    {
        return;
    }

    // Only a mob already in the fight: a follower pulls nothing of its own,
    // and the leader's target before it is pulled is not ours to tag.
    const UnitState* mob = s.findUnit(leader->target);

    if (!mob || !mob->isAlive() || mob->isPlayer || mob->reaction == Reaction::Friendly || !mob->inCombat)
    {
        return;
    }

    // One of ours hitting us is finished first.
    if (const UnitState* engaged = s.findUnit(context_.combat.engaged); engaged && engaged->isAlive() && s.isAttacker(*engaged))
    {
        return;
    }

    log_.info(logcat::Combat, "party: assisting {} on {} {}", leader->name, mob->name, formatGuid(mob->guid));
    context_.combat.engaged = mob->guid;
}

void BotInstance::reportFleeing(const GameSnapshot& s)
{
    std::vector<Guid> now;

    for (const UnitState& u : s.units)
    {
        // Ours: the one we go for, or one fighting us.
        if (!u.isFleeing || !u.isAlive() || u.isPlayer || (u.guid != context_.combat.engaged && u.guid != s.targetGuid && !s.isAttacker(u)))
        {
            continue;
        }

        now.push_back(u.guid);

        if (std::find(fleeing_.begin(), fleeing_.end(), u.guid) == fleeing_.end())
        {
            log_.info(logcat::Combat, "{} {} is fleeing (hp {:.0f}%, {:.1f} yd off)", u.name, formatGuid(u.guid), u.healthPercent(), distance(s.player.position, u.position));
        }
    }

    for (const Guid guid : fleeing_)
    {
        if (std::find(now.begin(), now.end(), guid) != now.end())
        {
            continue;
        }

        if (const UnitState* u = s.findUnit(guid); u && u->isAlive())
        {
            log_.info(logcat::Combat, "{} {} stopped fleeing (hp {:.0f}%, {:.1f} yd off)", u->name, formatGuid(guid), u->healthPercent(), distance(s.player.position, u->position));
        }
    }

    fleeing_ = std::move(now);
}

void BotInstance::reportTargetCasts(const GameSnapshot& s)
{
    // A real cast takes its cast time; one gone this soon, with nothing of
    // ours stopping it, had none.
    constexpr Duration kInstantBelow{600};
    const TimePoint now = clock_->now();
    const UnitState* target = s.target();
    const std::uint32_t spell = target && target->isCasting ? target->castingSpell : 0;

    // The actions that stop a cast, by name (Rogue.Kick, Warrior.Pummel, ...).
    if (const auto last = actions_.lastAction(); last && *last != lastActionSeen_)
    {
        lastActionSeen_ = *last;

        for (const char* stops : {"Kick", "Pummel", "ShieldBash", "Counterspell", "EarthShock", "Gouge", "KidneyShot", "CheapShot", "Bash", "Silence", "ConcussionBlow", "HammerOfJustice", "Blind", "PsychicScream", "Fear", "WarStomp", "Charge", "Intercept"})
        {
            if (last->ends_with(std::string(".") + stops))
            {
                interruptedAt_ = now;
            }
        }
    }

    const bool same = target && target->guid == castTarget_ && spell == castSpell_;

    // Ended on the same mob: not a target changed in the middle of a cast.
    // Alive still: a mob killed, or stunned by something else, ends its cast too.
    if (castSpell_ && target && target->guid == castTarget_ && target->isAlive() && spell != castSpell_ && now - castSince_ < kInstantBelow && interruptedAt_ < castSince_ && !context_.instantSpells.contains(castSpell_))
    {
        context_.instantSpells.insert(castSpell_);
        log_.info(logcat::Combat, "spell {} ended after {} ms: an instant one, target.isCasting leaves it out", castSpell_, std::chrono::duration_cast<std::chrono::milliseconds>(now - castSince_).count());
    }

    if (target && spell && !same)
    {
        log_.info(logcat::Combat, "{} is casting spell {}{}", target->name, spell, context_.instantSpells.contains(spell) ? " (instant)" : "");
        castSince_ = now;
    }

    castTarget_ = target ? target->guid : 0;
    castSpell_ = spell;
}

void BotInstance::reportReceivedItems(const GameSnapshot& s)
{
    std::map<std::uint32_t, int> counts;
    std::map<std::uint32_t, const std::string*> names;

    for (const auto& stack : s.player.bagItems)
    {
        if (stack.itemId == 0 || stack.count <= 0)
        {
            continue;
        }

        counts[stack.itemId] += stack.count;
        names[stack.itemId] = &stack.name;
    }

    std::vector<std::uint32_t> gained;

    for (const auto& [item, count] : counts)
    {
        const auto before = bagCounts_.find(item);

        if (count > (before == bagCounts_.end() ? 0 : before->second))
        {
            gained.push_back(item);
        }
    }

    // Not the first look at a character's bags, and not bags that were read
    // empty for a moment and are back: those are not received.
    constexpr std::size_t kAtOnce = 12;
    const bool known = bagsOf_ != 0 && bagsOf_ == s.player.guid && !(bagCounts_.empty() && gained.size() > 1);

    if (known && gained.size() <= kAtOnce)
    {
        for (const auto item : gained)
        {
            const auto before = bagCounts_.find(item);
            const int had = before == bagCounts_.end() ? 0 : before->second;
            // The client's own name for it, or else the data's.
            const std::string* name = names[item];

            if (name->empty() && questSource_)
            {
                if (const std::string* fromData = questSource_->itemName(item))
                {
                    name = fromData;
                }
            }

            log_.info(logcat::Loot, "received {} x item {}{}, {} in the bags", counts[item] - had, item, name->empty() ? std::string() : " (" + *name + ")", counts[item]);
        }
    }

    bagCounts_ = std::move(counts);
    bagsOf_ = s.player.guid;
}

// The names of the quests turned off, kept while they stay off - also once
// the quest is out of the log (abandoned), which is when a quest giver offers
// it again.
void BotInstance::noteIgnoredQuestNames()
{
    std::erase_if(ignoredQuestNames_, [&](const auto& named) {
        return !context_.ignoredQuests.contains(named.first) && !(questSource_ && questSource_->isAutoIgnored(named.first));
    });

    for (const auto& t : questDisplayTargets_)
    {
        if ((context_.ignoredQuests.contains(t.questId) || (questSource_ && questSource_->isAutoIgnored(t.questId))) && !t.questName.empty())
        {
            ignoredQuestNames_[t.questId] = t.questName;
        }
    }

    context_.ignoredQuestNames.clear();

    for (const auto& [id, name] : ignoredQuestNames_)
    {
        context_.ignoredQuestNames.insert(name);
    }
}

void BotInstance::setIgnoredQuests(std::set<std::uint32_t> quests)
{
    if (quests == context_.ignoredQuests)
    {
        return;
    }
    context_.ignoredQuests = std::move(quests);
    noteIgnoredQuestNames();
    if (questSource_)
    {
        questSource_->setIgnoredQuests(context_.ignoredQuests);
    }
    questTargetsAt_ = TimePoint{};
    questFocusLevel_.reset();
    questFocusKept_ = false;
    questUnderWay_.clear();
    questLead_ = 0;
    // Questing paused for want of a quest to do: the new selection may give
    // it one. A pause of the user's, or of a limit (deaths, runtime), stays -
    // Resume is theirs to press.
    if (pausedByQuesting_ && engine_.state() == EngineState::Paused && config_.quest.enabled)
    {
        engine_.resume();
        pausedByQuesting_ = false;
        questingPausedSaid_ = false;
    }
    const auto behavior = engine_.currentName();
    if (behavior == "Navigation" || behavior == "QuestHandIn" || behavior == "Gather" || behavior == "InventoryQuest")
    {
        engine_.shutdown();
        navigator_.stop();
    }
    log_.info(logcat::Profile, "quest selection refreshed: {} quest(s) ignored", context_.ignoredQuests.size());
}

void BotInstance::updateQuestTargets(const GameSnapshot& s)
{
    // Another character: not its failed hand-ins.
    if (s.player.guid != handInHeldBackOf_)
    {
        context_.handInHeldBack.clear();
        handInHeldBackOf_ = s.player.guid;
    }

    if (!config_.quest.enabled || !questSource_)
    {
        if (!context_.questTargets.empty())
        {
            context_.questTargets.clear();
            targets_.setQuestMobs({});
        }

        questReported_.clear();
        questDisplayTargets_.clear();
        questFocusLevel_.reset();
        questUnderWay_.clear();
        questLead_ = 0;
        return;
    }

    // A kill or loot can finish a quest between two ticks. Refresh before choosing
    // another target, even inside the periodic refresh interval.
    const auto& previous = engine_.lastSnapshot();
    const bool questStateChanged = !previous || s.player.questLog != previous->player.questLog || s.player.level != previous->player.level || !std::equal(s.player.questProgress.begin(), s.player.questProgress.end(), previous->player.questProgress.begin(), previous->player.questProgress.end(), [](const auto& a, const auto& b) { return a.questId == b.questId && a.kills == b.kills; }) || !std::equal(s.player.bagItems.begin(), s.player.bagItems.end(), previous->player.bagItems.begin(), previous->player.bagItems.end(), [](const auto& a, const auto& b) { return a.itemId == b.itemId && a.count == b.count; });
    const TimePoint now = clock_->now();

    if (!questStateChanged && !context_.questTargets.empty() && questTargetsAt_ != TimePoint{} && now - questTargetsAt_ < Duration(2000))
    {
        return;
    }

    questTargetsAt_ = now;
    auto targets = questSource_->targets(s);
    questDisplayTargets_ = targets;
    noteIgnoredQuestNames();
    std::erase_if(targets, [this](const QuestTarget& t) {
        return context_.ignoredQuests.contains(t.questId) || (questSource_ && questSource_->isAutoIgnored(t.questId));
    });
    const auto focusBefore = questFocusLevel_;
    std::set<std::uint32_t> heldBack;

    for (const auto& [quest, until] : context_.handInHeldBack)
    {
        if (clock_->now() < until)
        {
            heldBack.insert(quest);
        }
    }

    // Pathfinder goes anywhere: the quests that cost least, with those near
    // them. A profile's route keeps to its area, lowest level first.
    if (config_.usePathfinding)
    {
        questFocusLevel_.reset();
        const auto choice = focusQuestsByArea(targets, {s.player.position, s.player.level, s.player.questLog.size()}, questUnderWay_, heldBack);

        if (choice && choice->lead != std::exchange(questLead_, choice->lead))
        {
            const auto lead = std::find_if(targets.begin(), targets.end(), [&](const QuestTarget& t) { return t.questId == choice->lead; });
            std::string with;

            for (const auto& t : targets)
            {
                if (t.focus && t.questId != choice->lead)
                {
                    with += (with.empty() ? "" : ", ") + t.questName;
                }
            }

            log_.info(logcat::Profile, "quest mode: {} {} ({:.0f} yd){}{}", choice->handIn ? "handing in" : "working on", lead->questName, choice->distance, with.empty() ? "" : choice->handIn ? ", and with it " : ", and near it ", with);
        }
        else if (!choice)
        {
            questLead_ = 0;
        }
    }
    else
    {
        questFocusLevel_ = focusQuests(targets, questFocusLevel_, heldBack);
    }

    if (questFocusLevel_ && focusBefore && *questFocusLevel_ == *focusBefore && std::any_of(targets.begin(), targets.end(), [&](const QuestTarget& t) { return questLevel(t) < *questFocusLevel_; }) && !std::exchange(questFocusKept_, true))
    {
        log_.info(logcat::Profile, "quest mode: a lower level quest waits until the level {} quest(s) under way are handed in", *questFocusLevel_);
    }

    if (questFocusLevel_ && focusBefore && *questFocusLevel_ != *focusBefore)
    {
        questFocusKept_ = false;
    }

    if (config_.usePathfinding && pathfindingOrigin_ && targets.empty() && !s.inCombat() && engine_.currentName() != "QuestHandIn")
    {
        // Gone looking for only with an empty quest log (see NavigationBehavior).
        const auto givers = s.player.questLog.empty() ? questSource_->questGiverSpots(s) : std::vector<Vec3>{};
        // An NPC in sight with a quest to give (a yellow !): taken from it
        // (QuestHandIn), rather than pausing with quests in the log the bot
        // cannot do (Marla's Last Wish, at a grave) - one stood right there.
        // Only one QuestHandIn would go to: room in the log, and quests the
        // data names for that NPC.
        constexpr std::size_t kLogFull = 20;
        const bool offerInSight = s.player.questLog.size() < kLogFull && std::any_of(s.units.begin(), s.units.end(), [&](const UnitState& u) { return u.questMarker == QuestMarker::Available && u.isAlive() && u.entry != 0 && distance2D(u.position, s.player.position) <= 100.0f && !questSource_->offeredBy(s, u.entry).empty(); });

        if (givers.empty() && !offerInSight)
        {
            // Only a pause that is questing's own is taken back by a new quest selection.
            pausedByQuesting_ = pausedByQuesting_ || engine_.state() != EngineState::Paused;
            engine_.pause();
            navigator_.stop();

            if (!s.player.questLog.empty() || questSource_->hasPendingQuests(s))
            {
                // Once: this is come through on every tick while there is nothing to work on.
                if (!std::exchange(questingPausedSaid_, true))
                {
                    log_.warn(logcat::Profile, "Questing paused: active quests remain in the quest log, but none could be resolved in the starting zone.");
                }
            }
            else
            {
                log_.info(logcat::Profile, "Questing complete: no remaining quests or available quest givers in the starting zone.");
                finished_ = true;
                questingComplete_ = true;
            }
        }
    }

    if (!targets.empty())
    {
        questingPausedSaid_ = false;
    }

    std::vector<std::uint32_t> mobs;
    std::string working;

    for (const auto& t : targets)
    {
        if (!t.done)
        {
            working += (working.empty() ? "" : ", ") + t.questName;

            if (isLowestLevelQuest(t, targets))
            {
                mobs.insert(mobs.end(), t.mobs.begin(), t.mobs.end());
            }
        }
    }

    std::sort(mobs.begin(), mobs.end());
    mobs.erase(std::unique(mobs.begin(), mobs.end()), mobs.end());
    std::string workingBefore;

    for (const auto& t : context_.questTargets)
    {
        if (!t.done)
        {
            workingBefore += (workingBefore.empty() ? "" : ", ") + t.questName;
        }
    }

    if (working != workingBefore)
    {
        if (working.empty())
        {
            log_.info(logcat::Profile, "{}", !config_.usePathfinding ? "quest mode: nothing left to kill or loot for - grinding" : s.player.questLog.empty() ? "quest mode: checking for quests in the starting zone" : "quest mode: nothing left to kill or loot for");
        }
        else
        {
            log_.info(logcat::Profile, "quest mode: working on {}", working);
        }
    }

    // Progress: every count that moved, and a quest the moment it is done.
    std::map<std::uint32_t, std::pair<std::vector<std::string>, bool>> reported;

    for (const auto& t : targets)
    {
        const auto before = questReported_.find(t.questId);
        const bool known = before != questReported_.end();

        if (t.done && !(known && before->second.second))
        {
            if (t.handInTo.empty())
            {
                log_.info(logcat::Profile, "quest complete: {} - ready to hand in", t.questName);
            }
            else
            {
                log_.info(logcat::Profile, "quest complete: {} - hand it in to {}", t.questName, t.handInTo);
            }
        }
        else if (!t.done)
        {
            std::string changed;

            for (std::size_t k = 0; k < t.progress.size(); ++k)
            {
                if (!known || k >= before->second.first.size() || before->second.first[k] != t.progress[k])
                {
                    changed += (changed.empty() ? "" : ", ") + t.progress[k];
                }
            }

            if (!changed.empty())
            {
                log_.info(logcat::Profile, "quest progress: {} - {}", t.questName, changed);
            }
        }

        reported[t.questId] = {t.progress, t.done};
    }

    questReported_ = std::move(reported);
    context_.questTargets = std::move(targets);

    if (mobs.empty() && (config_.usePathfinding || !context_.questTargets.empty()))
    {
        mobs.push_back(std::numeric_limits<std::uint32_t>::max());
    }

    targets_.setQuestMobs(std::move(mobs));
}

void BotInstance::reportSkippedTargets(const GameSnapshot& s)
{
    if (s.player.isDeadOrGhost())
    {
        skipReported_.clear();
        return;
    }

    const TimePoint now = clock_->now();

    if (skipCheckedAt_ && now - *skipCheckedAt_ < Duration(1000))
    {
        return;
    }

    skipCheckedAt_ = now;
    // Near enough that walking past it would be noticed.
    constexpr float kNear = 25.0f;
    std::unordered_map<Guid, std::string> seen;

    for (const auto& u : s.units)
    {
        if (!u.isAlive() || u.isCritter || u.isPlayer || u.ownerGuid != 0 || u.reaction != Reaction::Hostile)
        {
            continue;
        }

        if (u.guid == context_.combat.engaged || s.isAttacker(u))
        {
            continue;
        }

        const float dist = distance(s.player.position, u.position);

        if (dist > kNear)
        {
            continue;
        }

        const auto reason = targets_.rejectReason(u, s);

        if (!reason)
        {
            continue;
        }

        seen.emplace(u.guid, *reason);

        if (const auto it = skipReported_.find(u.guid); it != skipReported_.end() && it->second == *reason)
        {
            continue;
        }

        log_.info(logcat::Target, "not pulling {} (level {}, {:.0f} yd): {}", u.name, u.level, dist, *reason);
    }

    // Only the mobs still near: one that comes back is said again.
    skipReported_ = std::move(seen);
}

// The character lies where it died: nothing is released or logged out, so
// whoever looks in decides what to do with it.
void BotInstance::checkStopAfterDeaths()
{
    if (finished_ || config_.stopAfterDeaths <= 0 || context_.stats.deaths - deathsAtLastLimit_ < config_.stopAfterDeaths || engine_.state() == EngineState::Paused)
    {
        return;
    }

    log_.warn(logcat::Engine, "died {} time(s) this session (stop after {} deaths) - pausing", context_.stats.deaths, config_.stopAfterDeaths);
    pause();
    syncClientBindings(false);
    stoppedAfterDeaths_ = true;
}

void BotInstance::checkStopAtLevel()
{
    if (!profile_ || profile_->options.stopAtLevel <= 0 || engine_.state() == EngineState::Paused)
    {
        return;
    }

    const auto& s = engine_.lastSnapshot();

    if (!s || s->player.level < profile_->options.stopAtLevel)
    {
        return;
    }

    if (!stopLevelAnnounced_)
    {
        stopLevelAnnounced_ = true;
        log_.info(logcat::Engine, "level {} reached (stop at level {}) - stopping once out of combat", s->player.level, profile_->options.stopAtLevel);
    }

    // The client refuses to log out in combat, and standing still mid-fight
    // gets the character killed: finish the fight, and any corpse run, first.
    if (s->isDead() || s->inCombat() || !s->attackers().empty())
    {
        return;
    }

    if (provider_->supportsLua())
    {
        if (!provider_->luaIdle())
        {
            return; // another script is still queued; try next tick
        }

        if (provider_->executeLua("Logout()"))
        {
            log_.info(logcat::Engine, "logging out");
        }
        else
        {
            log_.warn(logcat::Engine, "could not run Logout() - the bot stopped but the character stays in game");
        }
    }
    else
    {
        log_.warn(logcat::Engine, "this client cannot log out for the bot (no Lua) - stopped in place");
    }

    engine_.shutdown();
    syncClientBindings(false);
    finished_ = true;
}

std::vector<std::string> BotInstance::clientSetupCommands()
{
    // Auto loot too: nothing else takes a loot window's items without Lua.
    return {"/console SoftTargetInteract 3", "/console autoLootDefault 1"};
}

std::string BotInstance::interactBindCommand(const KeyChord& interactKey)
{
    // WoW names keys like "F11" or "CTRL-F11"; the bindings addon's chord format.
    std::string key = interactKey.toString();

    for (char& c : key)
    {
        if (c == '+')
        {
            c = '-';
        }
    }

    return std::format("/script if (GetBindingAction('{0}') or '')=='' then _ib=_ib or CreateFrame('Frame') "
                       "SetOverrideBinding(_ib,true,'{0}','INTERACTTARGET') end",
                       key);
}

void BotInstance::allowChatScripts()
{
    if (!provider_->supportsUi())
    {
        return;
    }

    if (!scriptsToRetype_.empty())
    {
        // Once the dialog has gone and the chat can take a line again.
        if (!actions_.mouseFree() || context_.uiFrameVisible("StaticPopup1"))
        {
            return;
        }

        const auto lines = std::exchange(scriptsToRetype_, {});

        for (const auto& line : lines)
        {
            if (!actions_.chat(line))
            {
                break;
            }
        }

        return;
    }

    // Asked only just after a /script line; the text is the English client's.
    auto recent = actions_.recentScripts(Duration(5000));

    if (recent.empty() || !context_.clickDialog("script"))
    {
        return;
    }

    log_.info(logcat::Action, "allowed custom scripts in the client - typing {} line(s) again", recent.size());
    scriptsToRetype_ = std::move(recent);
    actions_.clearRecentScripts();
}

void BotInstance::runClientSetupChat(bool wanted)
{
    if (clientSetupChat_.empty() || !wanted)
    {
        return;
    }

    const TimePoint now = clock_->now();

    if (clientSetupAt_ && now - *clientSetupAt_ < kClientSetupInterval)
    {
        return;
    }

    const auto& s = engine_.lastSnapshot();

    // Bindings cannot change in combat, and a cast is better left alone.
    if (!s || s->inCombat() || s->player.isCasting || actions_.isCasting())
    {
        return;
    }

    // A /console setting is saved by the client and survives a /reload, so it
    // is typed once a session; an override binding is not, so it is typed again.
    for (const auto& line : clientSetupChat_)
    {
        if (clientSetupAt_ && line.starts_with("/console "))
        {
            continue;
        }

        if (!actions_.chat(line))
        {
            return;
        }
    }

    if (!clientSetupAt_)
    {
        log_.info(logcat::Action, "set the client up through the chat ({} lines)", clientSetupChat_.size());
    }

    clientSetupAt_ = now;
}

void BotInstance::syncClientBindings(bool wanted)
{
    if (!clientBindingKeys_)
    {
        return;
    }

    // A /reload takes them without a loading screen the bot would notice, so
    // claiming again now and then puts them back; the addon ignores a repeat.
    constexpr Duration kReclaimInterval(60000);
    const TimePoint now = clock_->now();

    if (wanted && (!clientBindingsClaimed_ || now - lastClaim_ >= kReclaimInterval))
    {
        input_->press(clientBindingKeys_->first);
        lastClaim_ = now;

        if (!clientBindingsClaimed_)
        {
            log_.info(logcat::Action, "claimed the action bar keys ({})", clientBindingKeys_->first.toString());
        }

        clientBindingsClaimed_ = true;
    }
    else if (!wanted && clientBindingsClaimed_)
    {
        input_->press(clientBindingKeys_->second);
        clientBindingsClaimed_ = false;
        log_.info(logcat::Action, "gave the action bar keys back ({})", clientBindingKeys_->second.toString());
    }
}

void BotInstance::run(std::stop_token stop)
{
    obstacles_.setStopToken(stop);
    SteadyClock wall;

    while (!stop.stop_requested() && !finished_)
    {
        const TimePoint start = wall.now();
        tick();
        const Duration elapsed = wall.now() - start;

        if (elapsed < config_.tickInterval)
        {
            std::this_thread::sleep_for(config_.tickInterval - elapsed);
        }
    }

    engine_.shutdown();
    syncClientBindings(false);
    updateStatus();
}

void BotInstance::watchBreath(const GameSnapshot& s)
{
    // Feet this far under the surface: the head is under too. Afloat, a
    // human's feet are 1.5 yd under (Lake Everstill, 1.60.1).
    constexpr float kSubmerged = 1.9f;
    // Undead (race 5) hold their breath four times as long: Underwater Breathing.
    constexpr int kUndead = 5;
    auto& b = context_.breath;
    b.surface = config_.usePathfinding ? navigator_.provider().waterSurface(s.player.position) : std::nullopt;
    // The breath bar stays up while the breath fills again at the surface:
    // with the water known, the head must be under too.
    const bool headUnder = b.surface && s.player.position.z < *b.surface - kSubmerged;
    const bool under = !s.player.isDeadOrGhost() && (s.player.breathDraining ? *s.player.breathDraining && (!b.surface || headUnder) : headUnder);

    if (!under)
    {
        if (b.since)
        {
            log_.debug(logcat::Move, "breathing again after {:.0f} s under water", toSeconds(clock_->now() - *b.since));
        }

        b.since.reset();
        b.left.reset();
        return;
    }

    if (!b.since)
    {
        b.since = clock_->now();
        log_.debug(logcat::Move, "under water");
    }

    const float full = s.player.race == kUndead ? 240.0f : 60.0f;
    b.left = full - static_cast<float>(toSeconds(clock_->now() - *b.since));
}

std::vector<std::string> BotInstance::pullActionsOffBars() const
{
    auto actions = combatClass_ ? combatClass_->blockActions("pull") : std::vector<std::string>{};

    if (std::any_of(actions.begin(), actions.end(), [this](const std::string& action) { return actions_.hasBinding(action); }))
    {
        actions.clear();
    }

    return actions;
}

void BotInstance::updateStatus()
{
    std::lock_guard lock(statusMutex_);
    status_.behavior = engine_.onTaxi() ? std::string("Flying") : engine_.currentName();
    status_.objective = objectives_.describe();
    status_.lastAction = actions_.lastAction().value_or("");
    status_.profile = profile_ ? profile_->name : "";
    status_.combatClass = combatClass_ ? combatClass_->name() : "";
    status_.pullActions = combatClass_ ? combatClass_->blockActions("pull") : std::vector<std::string>{};
    status_.pullOnBar = pullActionsOffBars().empty();
    status_.snapshot = engine_.lastSnapshot();
    status_.stats = stats_;
    status_.fishingProblem = context_.fishingProblem;
    status_.now = clock_->now();
    status_.paused = engine_.state() == EngineState::Paused;
    status_.waitingForGame = engine_.waitingForGame();
    status_.partyMembers.clear();
    status_.partyLeader = 0;

    for (const PartyMember& member : context_.party.members())
    {
        if (member.inSight)
        {
            status_.partyMembers.push_back(member.guid);
            status_.partyLeader = member.leader ? member.guid : status_.partyLeader;
        }
    }
    status_.finished = finished_;

    if (status_.learnedRevision != obstacles_.revision() || status_.learned.empty() != obstacles_.learned().empty())
    {
        status_.learned = obstacles_.learned();
        status_.learnedRevision = obstacles_.revision();
    }

    status_.routeIndex = route_.index();
    status_.goingFor = 0;

    if (const Objective* top = objectives_.top(); top && top->kind == "GatherNode")
    {
        status_.goingFor = top->guid;
    }
    else if (engine_.currentName() == "Combat")
    {
        status_.goingFor = context_.combat.engaged;
    }

    // Flashed on the radar, with the Player Near sound: only a player found following us.
    status_.alertedPlayers = followerWatch_.foundWithin(clock_->now(), Duration(3000));
    status_.questing = config_.quest.enabled && questSource_ != nullptr;
    status_.quests.clear();
    status_.questLogSize = status_.snapshot ? status_.snapshot->player.questLog.size() : 0;

    if (status_.questing)
    {
        for (const auto& t : questDisplayTargets_)
        {
            const bool ignored = context_.ignoredQuests.contains(t.questId) || (questSource_ && questSource_->isAutoIgnored(t.questId));
            const auto active = std::find_if(context_.questTargets.begin(), context_.questTargets.end(), [&](const QuestTarget& q) { return q.questId == t.questId; });
            const bool current = !ignored && active != context_.questTargets.end() && isLowestLevelQuest(*active, context_.questTargets);
            status_.quests.push_back({t.questId, t.questName, questLevel(t), t.progress, t.done, current, t.handInTo, ignored});
        }

        std::stable_sort(status_.quests.begin(), status_.quests.end(), [](const QuestStatus& a, const QuestStatus& b) { return a.current != b.current ? a.current : a.level < b.level; });
    }

    status_.roamTarget.reset();
    status_.pathfindingRoamOrigin.reset();
    status_.pathfindingRoamRadius = 0;
    status_.pathfinding = config_.usePathfinding;
    status_.packZones = context_.packZones;
    status_.navigationPath.clear();

    if (navigator_.destination())
    {
        const auto& path = navigator_.path().points;
        const auto begin = std::min(navigator_.pathIndex(), path.size());
        status_.navigationPath.assign(path.begin() + begin, path.end());
    }

    status_.roamPath.clear();

    if (const auto* nav = dynamic_cast<const NavigationBehavior*>(engine_.findBehavior("Navigation")))
    {
        status_.routeCorrections = nav->corrector().corrections().size();

        // A spot picked before a fight or a death is no longer where the bot is heading.
        if (config_.roam.enabled && engine_.current() == nav)
        {
            status_.roamTarget = nav->roamTarget();
            status_.roamPath = nav->roamLegs();
        }
    }

    status_.travel = TravelStatus{};

    if (status_.snapshot)
    {
        const PlayerState& player = status_.snapshot->player;
        const float speed = std::max(1.0f, navigator_.estimatedRunSpeed());

        auto setTravel = [&](float totalDist, float remDist, std::string name)
        {
            if (totalDist >= kLongTravelDistanceThreshold)
            {
                status_.travel.active = true;
                status_.travel.totalDistance = totalDist;
                status_.travel.remainingDistance = std::max(0.0f, remDist);
                status_.travel.estimatedSpeed = speed;
                status_.travel.totalSeconds = status_.travel.totalDistance / speed;
                status_.travel.remainingSeconds = status_.travel.remainingDistance / speed;
                status_.travel.destinationName = std::move(name);
                return true;
            }
            return false;
        };

        bool found = false;

        // 1. Vendor
        if (const auto* vendor = dynamic_cast<const VendorBehavior*>(engine_.findBehavior("Vendor")))
        {
            if (vendor->isTravelling() || context_.vendorTrip)
            {
                found = setTravel(vendor->totalTripDistance(), vendor->remainingTripDistance(player.position), vendor->destinationName());
            }
        }

        // 2. Corpse run
        if (!found)
        {
            if (const auto* death = dynamic_cast<const DeathBehavior*>(engine_.findBehavior("Death")))
            {
                if (player.isDeadOrGhost() || death->isCorpseRun())
                {
                    const auto corpse = player.corpsePosition ? player.corpsePosition : context_.lastDeathPosition;
                    found = setTravel(death->corpseRunTotalDistance(), death->remainingCorpseDistance(player.position, corpse), "Corpse");
                }
            }
        }

        // 3. Quest hand-in / pickup
        if (!found)
        {
            if (const auto* handIn = dynamic_cast<const QuestHandInBehavior*>(engine_.findBehavior("QuestHandIn")))
            {
                if (handIn->isTravelling() || context_.questTravel)
                {
                    found = setTravel(handIn->totalTripDistance(), handIn->remainingTripDistance(player.position), handIn->destinationName());
                }
            }
        }

        // 4. Navigation quest spot
        if (!found)
        {
            if (const auto* nav = dynamic_cast<const NavigationBehavior*>(engine_.findBehavior("Navigation")))
            {
                if (nav->isTravellingToQuestSpot())
                {
                    found = setTravel(nav->questSpotTotalDistance(), nav->remainingQuestSpotDistance(player.position), "Quest Spot");
                }
            }
        }

        // 5. General navigation movement (e.g. navigator_.destination())
        if (!found)
        {
            if (navigator_.destination())
            {
                const Vec3 dest = *navigator_.destination();
                if (!generalTravelDest_ || distance(*generalTravelDest_, dest) > 5.0f)
                {
                    const float pathDist = !status_.navigationPath.empty() ? remainingPathDistance(player.position, status_.navigationPath, 0) : distance2D(player.position, dest);
                    if (pathDist >= kLongTravelDistanceThreshold)
                    {
                        generalTravelStart_ = player.position;
                        generalTravelDest_ = dest;
                        generalTravelTotalDist_ = pathDist;
                    }
                    else
                    {
                        generalTravelDest_.reset();
                        generalTravelTotalDist_ = 0.0f;
                    }
                }

                if (generalTravelDest_)
                {
                    const float remDist = !status_.navigationPath.empty() ? remainingPathDistance(player.position, status_.navigationPath, 0) : distance2D(player.position, *generalTravelDest_);
                    if (remDist <= 3.0f)
                    {
                        generalTravelDest_.reset();
                        generalTravelTotalDist_ = 0.0f;
                    }
                    else
                    {
                        setTravel(generalTravelTotalDist_, remDist, "Destination");
                    }
                }
            }
            else
            {
                generalTravelDest_.reset();
                generalTravelTotalDist_ = 0.0f;
            }
        }
    }
}

InstanceStatus BotInstance::status() const
{
    std::lock_guard lock(statusMutex_);
    return status_;
}

} // namespace og
