/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-12
 *  File: VendorBehavior.cpp
 *
 *  Desc:
 *  Implementation of VendorBehavior (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/behaviors/Behaviors.h"

#include "og/actions/ActionBinding.h"
#include "og/core/Events.h"
#include "og/core/VendorScript.h"
#include "og/navigation/Roaming.h"
#include "og/state/IGameStateProvider.h"

#include <algorithm>
#include <limits>

namespace og
{

namespace
{

// How close to come to each point of the vendor route: tight enough to take a
// doorway the way it was recorded instead of cutting the corner.
constexpr float kVendorRouteRadius = 1.5f;

// A right-click on the vendor opens its window at once, or not at all.
constexpr Duration kClickAnswerWait(3000);

// The vendors to choose from, nearest first: with "Only vendors in zone" those
// of the zone the character is in, where it has any (any that repairs, when a
// repair is due).
std::vector<VendorSpot> vendorChoices(const GameSnapshot& s, BotContext& ctx, bool repair)
{
    auto spots = ctx.questSource->vendorSpots(s);
    // Not one standing among hostile mobs (Defias Profiteer in Moonbrook),
    // where there is another.
    const auto safe = [&](const VendorSpot& spot) { return spot.hostilesAbout < VendorSpot::kDangerous && (!repair || spot.repairs); };

    if (std::any_of(spots.begin(), spots.end(), safe))
    {
        std::erase_if(spots, [](const VendorSpot& spot) { return spot.hostilesAbout >= VendorSpot::kDangerous; });
    }

    const auto here = [&](const VendorSpot& spot) { return spot.inZone && (!repair || spot.repairs); };

    if (ctx.config.vendor.onlyInZone && std::any_of(spots.begin(), spots.end(), here))
    {
        std::erase_if(spots, [](const VendorSpot& spot) { return !spot.inZone; });
    }

    return spots;
}

// Bags are full enough to be worth a trip.
bool bagsFull(const GameSnapshot& s, const VendorConfig& config)
{
    // -1 means the provider cannot read bag space; never trigger on a guess.
    return s.player.freeBagSlots >= 0 && s.player.freeBagSlots <= config.freeBagSlotsTrigger;
}

// Gear is broken - or yellow, when the trip is set to go then - and the trip
// would fix it. Unknown durability (-1) never triggers. A client that tells
// only of broken items (no damaged count) repairs at red.
bool needsRepair(const GameSnapshot& s, const VendorConfig& config)
{
    if (!config.repair)
    {
        return false;
    }

    if (config.repairAt == VendorConfig::RepairAt::Yellow && s.player.damagedItems > 0)
    {
        return true;
    }

    return s.player.brokenItems > 0;
}

// Index of the route waypoint closest to a position.
std::size_t nearestIndex(const std::vector<Vec3>& route, const Vec3& pos)
{
    std::size_t best = 0;
    float bestDist = std::numeric_limits<float>::max();

    for (std::size_t i = 0; i < route.size(); ++i)
    {
        const float d = distance(pos, route[i]);

        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }

    return best;
}

// Distance from a position to the closest waypoint of a route.
float nearestDistance(const std::vector<Vec3>& route, const Vec3& pos)
{
    if (route.empty())
    {
        return std::numeric_limits<float>::max();
    }

    return distance(pos, route[nearestIndex(route, pos)]);
}

// The grind waypoints to walk to get from `from` to `to`: join the route at the
// nearest waypoint and follow it round, rather than cutting across country.
// A looping route is walked whichever way round is shorter.
std::vector<Vec3> grindLink(const std::vector<Vec3>& grind, RouteMode mode, const Vec3& from, const Vec3& to)
{
    std::vector<Vec3> link;

    if (grind.size() < 2)
    {
        return link;
    }

    const std::size_t n = grind.size();
    const std::size_t start = nearestIndex(grind, from);
    const std::size_t end = nearestIndex(grind, to);
    const bool wrap = mode == RouteMode::Loop;
    const std::size_t forward = (end + n - start) % n;
    const std::size_t backward = (start + n - end) % n;
    const bool goBack = wrap ? backward < forward : end < start;
    // Without wrapping the two indices bound the walk, so it never leaves the
    // array and the modulo below is a no-op.
    const std::size_t steps = goBack ? (wrap ? backward : start - end) : (wrap ? forward : end - start);
    std::size_t i = start;

    for (std::size_t step = 0; step <= steps; ++step)
    {
        link.push_back(grind[i]);
        i = goBack ? (i + n - 1) % n : (i + 1) % n;
    }

    return link;
}

// What the professions the bot is actually running bring in. Those items must
// survive the sale, or the trip would throw away the point of gathering.
// What the quests in the log ask for is kept, whatever is set to be sold:
// many of those items are plain white ones (meat, pelts) with no "Quest Item"
// line to tell them by.
VendorConfig keepingQuestItems(VendorConfig config, const GameSnapshot& s, BotContext& ctx)
{
    if (ctx.questSource)
    {
        for (auto& name : ctx.questSource->questItems(s))
        {
            std::erase(config.alwaysSell, name);
            config.neverSell.push_back(std::move(name));
        }
    }

    return config;
}

KeepGathered gatherKeep(const BotContext& ctx)
{
    KeepGathered keep;
    keep.herbs = ctx.config.gathering.herbalism.enabled;
    keep.ore = ctx.config.gathering.mining.enabled;
    keep.leather = true;
    return keep;
}

std::string_view stageName(int stage)
{
    static constexpr std::string_view names[] = {"Travelling", "Approaching", "Opening", "Selling", "Repairing", "Returning", "Done"};
    return names[stage];
}

} // namespace

void VendorBehavior::setStage(Stage next, BotContext& ctx)
{
    if (stage_ == next)
    {
        return;
    }

    ctx.log.debug(logcat::Loot, "vendor stage {} -> {}", stageName(static_cast<int>(stage_)), stageName(static_cast<int>(next)));
    stage_ = next;
    stageStarted_ = ctx.now();

    // Only the vendor itself is clicked with the camera in: back out once its
    // window is dealt with, or the trip moves on.
    if (next != Stage::Opening && next != Stage::Selling)
    {
        ctx.restoreCamera();
    }

    if (next == Stage::Opening)
    {
        interactSent_ = false;
        mouseTried_ = false;
        gossipHandled_ = false;
        lastInteract_.reset();
    }
    else if (next == Stage::Selling)
    {
        soldItems_ = false;
        gossipHandled_ = false;
        uiStep_ = UiStep::Open;
        uiStepAt_ = ctx.now();
        visitedBagButtons_.clear();
        keptBagSlots_ = 0;
        uiBagSlot_.reset();
        uiTooltip_.reset();
        openedBags_ = false;
    }
    else if (next == Stage::Repairing)
    {
        repairedItems_ = false;
    }
}

// The profile's way to its vendor; without one, the vendor found in sight at Resume.
static std::vector<Vec3> vendorRouteOf(const BotContext& ctx)
{
    auto route = ctx.config.usePathfinding ? std::vector<Vec3>{} : ctx.profile ? ctx.profile->vendorRoute() : std::vector<Vec3>{};

    if (route.empty() && ctx.temporaryVendor)
    {
        route.push_back(ctx.temporaryVendor->position);
    }

    return route;
}

const UnitState* VendorBehavior::findVendor(const GameSnapshot& s, BotContext& ctx) const
{
    const VendorConfig& config = ctx.config.vendor;
    const UnitState* best = nullptr;
    const UnitState* bestRepairer = nullptr;
    float bestDist = config.searchRadius;
    float bestRepairerDist = config.searchRadius;
    const std::string vendorName = ctx.config.usePathfinding && ctx.temporaryVendor ? ctx.temporaryVendor->name : ctx.profile && !ctx.profile->vendorName.empty() ? ctx.profile->vendorName : ctx.temporaryVendor ? ctx.temporaryVendor->name : std::string{};

    for (const auto& u : s.units)
    {
        if (!u.isAlive())
        {
            continue;
        }

        if (ctx.config.usePathfinding && ctx.temporaryVendor && u.entry != ctx.temporaryVendor->entry && u.name != ctx.temporaryVendor->name)
        {
            continue;
        }

        const bool nameMatch = !vendorName.empty() && u.name == vendorName;

        if (!u.isVendor && !nameMatch)
        {
            continue;
        }

        const float dist = distance(s.player.position, u.position);

        if (dist < bestDist)
        {
            best = &u;
            bestDist = dist;
        }

        if ((u.canRepair || nameMatch) && dist < bestRepairerDist)
        {
            bestRepairer = &u;
            bestRepairerDist = dist;
        }
    }

    // Broken gear is the one thing only some vendors can fix.
    return needsRepair(s, config) && bestRepairer ? bestRepairer : best;
}

bool vendorNeeded(const GameSnapshot& s, const VendorConfig& config)
{
    return config.enabled && (bagsFull(s, config) || needsRepair(s, config));
}

bool VendorBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes: to the vendor only on its
    // turn, at the vendor the leader stands by (temporaryVendor).
    if (ctx.partyFollower() && !(ctx.partyVendorTurn || trip_))
    {
        return false;
    }

    const VendorConfig& config = ctx.config.vendor;

    if (!config.enabled || s.isDead() || s.inCombat() || !s.attackers().empty())
    {
        return false;
    }

    // The leader, for a member that asked: a trip as if its own bags were full.
    const bool wanted = bagsFull(s, config) || needsRepair(s, config) || ctx.partyVendorForMember || ctx.vendorBeforeTraining;
    // Asked for by the trainer trip: not held back by the last trip's wait.
    const bool waited = ctx.now() >= noTripUntil_ || ctx.vendorBeforeTraining;

    if (ctx.config.usePathfinding && !ctx.partyFollower() && !trip_ && ctx.questSource && waited && wanted)
    {
        ctx.temporaryVendor.reset();
        const bool repair = needsRepair(s, config);

        for (const auto& spot : vendorChoices(s, ctx, repair))
        {
            if (repair && !spot.repairs)
            {
                continue;
            }

            if (ctx.obstacles && ctx.obstacles->avoids(spot.position))
            {
                continue;
            }

            if (spot.entry != 0 && unreachableVendors_.contains(spot.entry) && ctx.now() < unreachableVendors_[spot.entry])
            {
                continue;
            }

            if (!spot.name.empty() && unreachableVendorNames_.contains(spot.name) && ctx.now() < unreachableVendorNames_[spot.name])
            {
                continue;
            }

            ctx.temporaryVendor = BotContext::TemporaryVendor{spot.name, spot.position, spot.repairs, spot.entry};
            break;
        }
    }

    // A party follower has no profile: the vendor its leader stands by is its route.
    if ((!ctx.profile && !(ctx.partyFollower() && ctx.temporaryVendor)) || vendorRouteOf(ctx).empty())
    {
        return false;
    }

    // Selling needs Lua, the merchant window's buttons, or the bindings addon
    // doing it at the window; without any of them the trip would be pointless.
    if (!ctx.provider || (!ctx.canRunScripts() && !ctx.provider->supportsUi() && !config.merchantAddon))
    {
        return false;
    }

    if (config.sellsNothing() && !config.repair)
    {
        return false;
    }

    // Once started, keep going even after bags free up mid-trip.
    if (trip_)
    {
        return true;
    }

    return waited && wanted;
}

void VendorBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    if (trip_)
    {
        // Resuming after a fight (or a pause). The trip is only ticked while it
        // is the running behavior, so the time away was not spent travelling:
        // push both deadlines out by it. Without this a route that has to be
        // fought through would abort on the travel timeout, and a scrap at the
        // vendor itself would trip the stage timeout the moment it ended.
        if (leftAt_ != TimePoint{})
        {
            const Duration away = ctx.now() - leftAt_;
            started_ += away;
            stageStarted_ += away;

            if (lastInteract_)
            {
                *lastInteract_ += away;
            }

            if (lastDistanceLog_)
            {
                *lastDistanceLog_ += away;
            }
        }

        // On the way from the route back to where the trip began: keep going
        // for the point it was heading to - the way there is planned from
        // wherever the fight left it. Its points lie far apart, and the
        // nearest was the end of the route behind it: the bot ran back to the
        // vendor route after every fight.
        if (stage_ == Stage::Returning && pathIndex_ >= homeFrom_)
        {
            return;
        }

        // A fight may have moved the bot off the vendor route: pick it up again
        // at the nearest of its points rather than cutting across to the next.
        const std::size_t first = stage_ == Stage::Returning ? 0 : linkPoints_;
        const std::size_t end = stage_ == Stage::Returning ? std::min(homeFrom_, path_.size()) : path_.size();

        if (((stage_ == Stage::Travelling && entryReached_) || stage_ == Stage::Returning) && pathIndex_ > first && pathIndex_ < path_.size())
        {
            float best = std::numeric_limits<float>::max();

            for (std::size_t i = first; i < end; ++i)
            {
                const float d = distance(s.player.position, path_[i]);

                if (d < best)
                {
                    best = d;
                    pathIndex_ = i;
                }
            }
        }

        return;
    }

    trip_ = true;
    ctx.vendorTrip = true;
    tripFrom_ = s.player.position;
    timedOut_ = false;
    leftAt_ = TimePoint{};
    stage_ = Stage::Travelling;
    vendor_ = 0;
    interactSent_ = false;
    gossipHandled_ = false;
    soldItems_ = false;
    repairedItems_ = false;
    unreachableCount_ = 0;
    giveUpTrip_ = false;
    lastInteract_.reset();
    lastDistanceLog_.reset();

    // Walk past mobs on the way: defend if attacked, but never pull. A target
    // left engaged from the last fight would otherwise outrank this behavior.
    ctx.combat.defendOnly = true;
    ctx.combat.engaged = 0;
    freeSlotsOnArrival_ = s.player.freeBagSlots;
    started_ = ctx.now();
    stageStarted_ = started_;

    buildOutboundPath(s, ctx);

    if (ctx.config.usePathfinding && ctx.temporaryVendor)
    {
        ctx.log.info(logcat::Loot, "pathfinding: heading to {}{} ({:.0f} yd)", ctx.temporaryVendor->name, needsRepair(s, ctx.config.vendor) ? " for repairs" : " to sell", distance2D(s.player.position, ctx.temporaryVendor->position));
    }

    const auto route = vendorRouteOf(ctx);
    ctx.objectives.push(Objective{"Vendor", 0, enteredAtFront_ ? route.back() : route.front()});

    if (bagsFull(s, ctx.config.vendor))
    {
        ctx.log.info(logcat::Loot, "bags full ({} free) - heading to the vendor", s.player.freeBagSlots);
    }
    else if (s.player.brokenItems > 0)
    {
        ctx.log.info(logcat::Loot, "{} broken item(s) equipped - heading to the vendor to repair", s.player.brokenItems);
    }
    else
    {
        ctx.log.info(logcat::Loot, "{} item(s) equipped low on durability (yellow) - heading to the vendor to repair", s.player.damagedItems);
    }

    logPathDistance(s, ctx);
    lastDistanceLog_ = started_;
}

void VendorBehavior::onExit(BotContext& ctx)
{
    leftAt_ = ctx.now();
    // Combat can take over mid-interact; leave no click-to-move behind.
    ctx.cancelInteraction();
    ctx.restoreCamera();
    ctx.navigator.stop();
    // A fight on the way moves the bot off the leg it was recording.
    detours_.abandon();
}

void VendorBehavior::buildOutboundPath(const GameSnapshot& s, BotContext& ctx)
{
    const auto route = vendorRouteOf(ctx);
    // The live route, so remembered obstacle detours are used here too; none
    // for a party follower, which has no profile.
    static const std::vector<Vec3> kNoGrind;
    const std::vector<Vec3>& grind = ctx.route ? ctx.route->points() : ctx.profile ? ctx.profile->waypoints : kNoGrind;

    // The vendor route meets the grind route at one of its ends. Profiles are
    // recorded in either direction, so take the end that sits on the grind
    // route rather than assuming the recorded start.
    const UnitState* visibleVendor = findVendor(s, ctx);

    if (visibleVendor)
    {
        enteredAtFront_ = distance(visibleVendor->position, route.front()) >= distance(visibleVendor->position, route.back());
    }
    else
    {
        enteredAtFront_ = grind.empty() || nearestDistance(grind, route.front()) <= nearestDistance(grind, route.back());
    }

    const Vec3& entry = enteredAtFront_ ? route.front() : route.back();

    path_.clear();
    pathIndex_ = 0;
    retryPathAt_.reset();
    unreachableCount_ = 0;
    giveUpTrip_ = false;
    entryReached_ = false;

    // Already on the vendor route (started, or restarted, somewhere along it):
    // by one of its points, or outside the grind area and nearer the vendor
    // route than the grind route. Join it at its nearest point and follow it
    // on to the vendor, instead of walking back to where it meets the grind
    // route and the whole way out again - a vendor route running inside the
    // area went back to its start.
    constexpr float kOnVendorRoute = 30.0f;
    const Vec3& pos = s.player.position;
    const bool outsideGrind = !outlinesArea(grind) || !insideRoute(grind, pos);

    if (nearestDistance(route, pos) <= kOnVendorRoute || (outsideGrind && nearestDistance(route, pos) < nearestDistance(grind, pos)))
    {
        const std::size_t from = nearestIndex(route, pos);

        if (enteredAtFront_)
        {
            path_.assign(route.begin() + static_cast<std::ptrdiff_t>(from), route.end());
        }
        else
        {
            for (std::size_t i = from + 1; i-- > 0;)
            {
                path_.push_back(route[i]);
            }
        }

        linkPoints_ = 0;
        entryReached_ = true;
        ctx.log.info(logcat::Loot, "already on the vendor route: joining it at waypoint {}/{} towards the NPC", enteredAtFront_ ? from + 1 : route.size() - from, route.size());
        detours_.abandon();
        detours_.apply(path_, ctx, 0);
        return;
    }

    // In Freeroam mode, we do not follow the normal grind waypoint line.
    // Instead, navigate to where the vendor route starts (entry), avoiding zones and obstacles,
    // and once we arrive at the first vendor waypoint, follow the vendor route down to the NPC.
    if (!ctx.config.roam.enabled)
    {
        // Walk the grind route round to that junction instead of striking out
        // across country towards it. Skipped when we are already nearer the
        // junction than the grind route is - there is nothing to lead in from.
        if (distance(s.player.position, entry) > nearestDistance(grind, entry))
        {
            path_ = grindLink(grind, ctx.route ? ctx.route->mode() : ctx.profile ? ctx.profile->routeMode : RouteMode::Loop, s.player.position, entry);
        }
    }
    else
    {
        ctx.log.info(logcat::Loot, "freeroam: navigating to vendor route entry at ({:.1f}, {:.1f}, {:.1f})", entry.x, entry.y, entry.z);
    }

    linkPoints_ = path_.size();

    if (enteredAtFront_)
    {
        path_.insert(path_.end(), route.begin(), route.end());
    }
    else
    {
        path_.insert(path_.end(), route.rbegin(), route.rend());
    }

    // The grind points already carry their detours.
    detours_.abandon();
    detours_.apply(path_, ctx, linkPoints_);
    totalTripDistance_ = remainingPathDistance(s.player.position, path_, 0);
}

void VendorBehavior::buildReturnPath(const GameSnapshot& s, BotContext& ctx)
{
    const auto route = vendorRouteOf(ctx);
    path_.clear();
    pathIndex_ = 0;
    retryPathAt_.reset();
    linkPoints_ = 0;
    // Pick the route up from wherever we are standing: the leg out leaves it as
    // soon as the vendor comes into range, so it can still be on the very first
    // waypoint, and walking straight there is the cross-country line this is
    // meant to avoid.
    const std::size_t from = nearestIndex(route, s.player.position);

    if (enteredAtFront_)
    {
        for (std::size_t i = from + 1; i-- > 0;)
        {
            path_.push_back(route[i]);
        }
    }
    else
    {
        for (std::size_t i = from; i < route.size(); ++i)
        {
            path_.push_back(route[i]);
        }
    }

    detours_.abandon();
    detours_.apply(path_, ctx);

    // Then on to where the trip began, the grinding picking up where it left
    // off - unless that was by the route already.
    constexpr float kBackToTheRoute = 15.0f;
    homeFrom_ = path_.size();

    if (tripFrom_ && !path_.empty() && distance2D(path_.back(), *tripFrom_) > kBackToTheRoute)
    {
        if (ctx.dangerAt(*tripFrom_) || !ctx.navigator.canStandAt(*tripFrom_))
        {
            ctx.log.info(logcat::Loot, "where the vendor trip began is inside a danger zone - stopping return route outside");
        }
        else
        {
            static const std::vector<Vec3> noArea;
            const auto& area = ctx.route ? ctx.route->points() : noArea;
            const auto way = circuitWay(area, path_.back(), *tripFrom_, ctx.obstacles);
            path_.insert(path_.end(), way.begin(), way.end());
            ctx.log.info(logcat::Loot, "after the vendor route: back to where the trip began, {:.0f} yd on", distance2D(path_[homeFrom_ - 1], *tripFrom_));
        }
    }

    totalTripDistance_ = remainingPathDistance(s.player.position, path_, 0);
    lastDistanceLog_ = ctx.now();
    const float distRemaining = remainingPathDistance(s.player.position, path_, pathIndex_);
    ctx.log.info(logcat::Loot, "following vendor path back to freeroam: {:.0f} yd remaining (waypoint {}/{})", distRemaining, pathIndex_ + 1, path_.size());
}

void VendorBehavior::logPathDistance(const GameSnapshot& s, BotContext& ctx)
{
    if (stage_ == Stage::Travelling)
    {
        if (const UnitState* v = findVendor(s, ctx))
        {
            ctx.log.info(logcat::Loot, "following vendor path towards NPC: {:.0f} yd to NPC (waypoint {}/{})", distance(s.player.position, v->position), pathIndex_ + 1, path_.size());
        }
        else
        {
            ctx.log.info(logcat::Loot, "following vendor path: {:.0f} yd of route remaining, NPC not visible (waypoint {}/{})", remainingPathDistance(s.player.position, path_, pathIndex_), pathIndex_ + 1, path_.size());
        }
    }
    else if (stage_ == Stage::Returning)
    {
        ctx.log.info(logcat::Loot, "{}: {:.0f} yd remaining (waypoint {}/{})", pathIndex_ >= homeFrom_ ? "back to where the trip began" : "following vendor path back to freeroam", remainingPathDistance(s.player.position, path_, pathIndex_), pathIndex_ + 1, path_.size());
    }
}

bool VendorBehavior::switchToNextVendor(const GameSnapshot& s, BotContext& ctx, const std::string& why)
{
    const std::uint32_t currentEntry = ctx.temporaryVendor ? ctx.temporaryVendor->entry : 0;
    const std::string currentName = ctx.temporaryVendor ? ctx.temporaryVendor->name : ctx.profile && !ctx.profile->vendorName.empty() ? ctx.profile->vendorName : "vendor";

    // Blacklist the current unreachable vendor for 15 minutes.
    constexpr Duration kBlacklistDuration = Duration(15 * 60 * 1000);

    if (currentEntry != 0)
    {
        unreachableVendors_[currentEntry] = ctx.now() + kBlacklistDuration;
    }

    if (!currentName.empty())
    {
        unreachableVendorNames_[currentName] = ctx.now() + kBlacklistDuration;
    }

    if (!ctx.config.usePathfinding || !ctx.questSource)
    {
        return false;
    }

    const bool repair = needsRepair(s, ctx.config.vendor);
    std::optional<VendorSpot> nextSpot;

    for (const auto& spot : vendorChoices(s, ctx, repair))
    {
        if (repair && !spot.repairs)
        {
            continue;
        }

        if (ctx.obstacles && ctx.obstacles->avoids(spot.position))
        {
            continue;
        }

        if (spot.entry != 0 && unreachableVendors_.contains(spot.entry) && ctx.now() < unreachableVendors_[spot.entry])
        {
            continue;
        }

        if (!spot.name.empty() && unreachableVendorNames_.contains(spot.name) && ctx.now() < unreachableVendorNames_[spot.name])
        {
            continue;
        }

        nextSpot = spot;
        break;
    }

    if (!nextSpot)
    {
        ctx.log.warn(logcat::Loot, "vendor {} {} and no alternative vendors found", currentName, why);
        return false;
    }

    ctx.log.warn(logcat::Loot, "vendor {} {} - switching to next closest vendor {} ({:.0f} yd)", currentName, why, nextSpot->name, distance2D(s.player.position, nextSpot->position));

    ctx.temporaryVendor = BotContext::TemporaryVendor{nextSpot->name, nextSpot->position, nextSpot->repairs, nextSpot->entry};
    unreachableCount_ = 0;
    retryPathAt_.reset();
    giveUpTrip_ = false;
    vendor_ = 0;
    ctx.navigator.stop();
    ctx.movement.stop();
    buildOutboundPath(s, ctx);
    ctx.objectives.pop("Vendor");
    const auto route = vendorRouteOf(ctx);
    ctx.objectives.push(Objective{"Vendor", 0, enteredAtFront_ ? route.back() : route.front()});
    stageStarted_ = ctx.now();
    lastDistanceLog_.reset();
    logPathDistance(s, ctx);
    return true;
}

bool VendorBehavior::walkPath(const GameSnapshot& s, BotContext& ctx)
{
    if (path_.empty())
    {
        return true;
    }

    // Keep tight waypoint arrival radii indoors.
    // The way from the route back to where the trip began is not the route.
    const bool onRoute = (stage_ == Stage::Returning && pathIndex_ < homeFrom_) || (stage_ != Stage::Returning && pathIndex_ > linkPoints_);
    const bool isFinalReturnPoint = (stage_ == Stage::Returning && pathIndex_ + 1 >= path_.size());
    const float radius = isFinalReturnPoint ? ctx.config.waypointRadius : (onRoute ? kVendorRouteRadius : ctx.config.waypointRadius);

    const TimePoint now = ctx.now();

    if (!lastDistanceLog_ || now - *lastDistanceLog_ > Duration(2500))
    {
        lastDistanceLog_ = now;
        logPathDistance(s, ctx);
    }

    // An unreachable point is not tried again every tick: working out that
    // there is no way can take a large part of a second.
    if (retryPathAt_ && now < *retryPathAt_)
    {
        return false;
    }

    retryPathAt_.reset();

    const bool homeLeg = stage_ == Stage::Returning && pathIndex_ >= homeFrom_;
    if (homeLeg && (ctx.dangerAt(path_[pathIndex_]) || !ctx.navigator.canStandAt(path_[pathIndex_])))
    {
        ctx.log.info(logcat::Loot, "vendor return waypoint {} is in danger zone - completing return trip outside", pathIndex_);
        pathIndex_ = path_.size();
        return true;
    }

    // The vendor route is the player's own, walked point by point whatever No Go
    // zones or learned obstacles say; only the way onto it goes round them, and
    // into one covering the point it joins the route at.
    const bool exact = onRoute;
    const NavStatus status = ctx.navigator.moveTo(s, path_[pathIndex_], radius, false, exact, !homeLeg);
    detours_.update(s, ctx, pathIndex_, status == NavStatus::Recovering);

    if (status == NavStatus::Unreachable)
    {
        detours_.abandon();
        // A spot on the way back to where the trip began is only a way there:
        // if unreachable, finish return trip.
        if (homeLeg)
        {
            ctx.log.warn(logcat::Loot, "vendor return waypoint {} unreachable - finishing return trip", pathIndex_);
            pathIndex_ = path_.size();
            return true;
        }

        if (onRoute || pathIndex_ >= linkPoints_)
        {
            ++unreachableCount_;

            if (unreachableCount_ >= 3)
            {
                if (stage_ == Stage::Travelling && switchToNextVendor(s, ctx))
                {
                    return false;
                }

                ctx.log.warn(logcat::Loot, "vendor path point {} unreachable after 3 attempts - abandoning trip", pathIndex_);
                unreachableCount_ = 0;
                retryPathAt_.reset();
                giveUpTrip_ = true;
                return false;
            }

            ctx.log.warn(logcat::Loot, "vendor path point {} unreachable (attempt {}/3) - trying it again in 5 seconds", pathIndex_, unreachableCount_);
            ctx.navigator.stop();
            retryPathAt_ = now + Duration(5000);
            return false;
        }

        ctx.log.warn(logcat::Loot, "vendor path point {} unreachable - skipping", pathIndex_);
    }
    else if (status != NavStatus::Arrived)
    {
        return false;
    }
    else
    {
        unreachableCount_ = 0;

        if (stage_ == Stage::Travelling && pathIndex_ == linkPoints_)
        {
            entryReached_ = true;
        }

        if (const std::size_t added = detours_.reached(path_, pathIndex_, ctx))
        {
            // The way round went in before the point just reached.
            if (pathIndex_ < linkPoints_)
            {
                linkPoints_ += added;
            }

            if (stage_ == Stage::Returning && pathIndex_ < homeFrom_)
            {
                homeFrom_ += added;
            }

            pathIndex_ += added;
        }
    }

    if (pathIndex_ + 1 >= path_.size())
    {
        return true;
    }

    ++pathIndex_;
    stageStarted_ = ctx.now();
    lastDistanceLog_ = stageStarted_;
    logPathDistance(s, ctx);
    return false;
}

BehaviorStatus VendorBehavior::merchantNeverOpened(const GameSnapshot& s, BotContext& ctx)
{
    for (const auto* close : {"GossipFrameCloseButton", "MerchantFrameCloseButton"})
    {
        if (ctx.uiFrameVisible(close))
        {
            ctx.clickUiFrame(close);
        }
    }

    ctx.cancelInteraction();

    if (switchToNextVendor(s, ctx, "opened no merchant window"))
    {
        setStage(Stage::Travelling, ctx);
        return BehaviorStatus::Running;
    }

    return finish(ctx, "the merchant window never opened");
}

BehaviorStatus VendorBehavior::finish(BotContext& ctx, const std::string& reason, Duration cooldown)
{
    ctx.navigator.stop();
    ctx.movement.stop();
    ctx.restoreCamera();
    ctx.objectives.pop("Vendor");
    trip_ = false;
    ctx.vendorTrip = false;
    ctx.vendorBeforeTraining = false;
    stage_ = Stage::Done;
    totalTripDistance_ = 0.0f;
    vendor_ = 0;
    ctx.combat.defendOnly = false; // back to normal grinding
    unreachableCount_ = 0;
    giveUpTrip_ = false;

    if (freed_ > 0 || repairedItems_)
    {
        if (ctx.temporaryVendor)
        {
            if (ctx.temporaryVendor->entry != 0)
            {
                unreachableVendors_.erase(ctx.temporaryVendor->entry);
            }

            if (!ctx.temporaryVendor->name.empty())
            {
                unreachableVendorNames_.erase(ctx.temporaryVendor->name);
            }
        }
    }

    noTripUntil_ = ctx.now() + cooldown;
    ctx.log.info(logcat::Loot, "vendor trip finished: {} (no more trips for {:.0f}s)", reason, toSeconds(cooldown));
    return BehaviorStatus::Success;
}

BehaviorStatus VendorBehavior::finish(BotContext& ctx, const std::string& reason, bool backoff)
{
    const Duration cooldown = backoff ? ctx.config.vendor.retryDelay : Duration(90000);
    return finish(ctx, reason, cooldown);
}

BehaviorStatus VendorBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    const VendorConfig& config = ctx.config.vendor;
    const TimePoint now = ctx.now();

    if (now - started_ > config.travelTimeout)
    {
        // The budget is per leg, and the way home gets its own: abandoning the
        // trip mid-route leaves the bot standing wherever it ran out of time,
        // free to pull again, with Navigation cutting straight back to the
        // nearest grind waypoint through whatever lies in between.
        if (timedOut_)
        {
            return finish(ctx, "timed out on the way back");
        }

        // Still on the grind lead-in: we are already on the grind route, so
        // there is nothing to retrace and the vendor route is not ours to join.
        if (stage_ == Stage::Travelling && pathIndex_ < linkPoints_)
        {
            return finish(ctx, "timed out");
        }

        // Quest mode has nowhere to go back to: it goes on from here.
        if (ctx.config.quest.enabled)
        {
            ctx.cancelInteraction();
            return finish(ctx, "timed out");
        }

        timedOut_ = true;
        started_ = now;

        if (stage_ != Stage::Returning)
        {
            ctx.cancelInteraction();
            buildReturnPath(s, ctx);
            setStage(Stage::Returning, ctx);
        }

        ctx.log.warn(logcat::Loot, "vendor trip is out of time - walking the route back");
        return BehaviorStatus::Running;
    }

    switch (stage_)
    {
    case Stage::Travelling:
    {
        if (giveUpTrip_)
        {
            giveUpTrip_ = false;
            return finish(ctx, "vendor unreachable after 3 attempts");
        }

        const UnitState* v = findVendor(s, ctx);

        if (v)
        {
            const auto route = vendorRouteOf(ctx);
            const bool entryAtFront = distance(v->position, route.front()) >= distance(v->position, route.back());

            if (entryAtFront != enteredAtFront_)
            {
                ctx.navigator.stop();
                buildOutboundPath(s, ctx);
                ctx.objectives.pop("Vendor");
                ctx.objectives.push(Objective{"Vendor", v->guid, v->position});
                stageStarted_ = now;
                lastDistanceLog_.reset();
                ctx.log.info(logcat::Loot, "vendor path direction corrected from NPC world position");
            }
        }

        // The end of the way stands in for the NPC at a height taken from the
        // map: the ground. Up a tower that is the floor under it - for Dawn
        // Brightstar in the Tower of Azora 35 yards under, and the bot left
        // the ramp it had just started up to get there, into the wall. Seen,
        // the NPC says itself where it stands.
        if (v && ctx.config.usePathfinding && !path_.empty() && distance2D(path_.back(), v->position) <= 10.0f && std::abs(path_.back().z - v->position.z) > 2.0f)
        {
            ctx.log.info(logcat::Loot, "the vendor stands {:.0f} yd {} where the map had it - going there", std::abs(v->position.z - path_.back().z), v->position.z > path_.back().z ? "above" : "below");
            path_.back() = v->position;
        }

        // Arrival depends on world distance to the NPC, never the waypoint
        // index: the last target may still be far away or behind a doorway.
        if (entryReached_ && v && distance(s.player.position, v->position) <= std::max(6.0f, ctx.config.interactDistance))
        {
            ctx.navigator.stop();
            ctx.movement.stop();
            vendor_ = v->guid;
            setStage(Stage::Approaching, ctx);
            return BehaviorStatus::Running;
        }

        if (!walkPath(s, ctx))
        {
            if (giveUpTrip_)
            {
                giveUpTrip_ = false;
                return finish(ctx, "vendor unreachable after 3 attempts");
            }

            return BehaviorStatus::Running;
        }

        // Path reached the end.
        if (v)
        {
            ctx.navigator.stop();
            ctx.movement.stop();
            vendor_ = v->guid;
            setStage(Stage::Approaching, ctx);
            return BehaviorStatus::Running;
        }

        // The route ran out with no vendor in sight: give it a moment to come
        // into view, then give up rather than idle at the end.
        if (now - stageStarted_ > config.stageTimeout)
        {
            if (switchToNextVendor(s, ctx))
            {
                return BehaviorStatus::Running;
            }

            return finish(ctx, "no vendor found on the route");
        }

        return BehaviorStatus::Running;
    }

    case Stage::Approaching:
    {
        const UnitState* v = s.findUnit(vendor_);

        if (!v)
        {
            v = findVendor(s, ctx);

            if (!v)
            {
                setStage(Stage::Travelling, ctx);
                vendor_ = 0;
                return BehaviorStatus::Running;
            }

            vendor_ = v->guid;
        }

        const float dist = distance(s.player.position, v->position);
        const float reach = ctx.humanReach(ctx.config.interactDistance, v->guid);

        if (dist <= reach)
        {
            ctx.navigator.stop();
            ctx.movement.stop();
            setStage(Stage::Opening, ctx);
            return BehaviorStatus::Running;
        }

        // Once on the vendor route, drawn by hand, No Go zones no longer count:
        // straight on to the NPC, who may well stand inside one.
        const NavStatus status = ctx.navigator.moveTo(s, v->position, std::max(0.25f, reach - 0.5f), true, !ctx.config.roam.enabled || entryReached_, true);

        if (status == NavStatus::Unreachable || (status == NavStatus::Arrived && dist > ctx.config.interactDistance))
        {
            // A client-map route can end on a different floor when the target's
            // height is approximate. A visible NPC has an exact position: the
            // end of that route is not arrival within interaction range.
            if (switchToNextVendor(s, ctx))
            {
                setStage(Stage::Travelling, ctx);
                return BehaviorStatus::Running;
            }

            return finish(ctx, "cannot reach the vendor");
        }

        if (status != NavStatus::Arrived)
        {
            return BehaviorStatus::Running;
        }

        ctx.navigator.stop();
        ctx.movement.stop();
        setStage(Stage::Opening, ctx);
        return BehaviorStatus::Running;
    }

    case Stage::Opening:
    {
        const UnitState* v = s.findUnit(vendor_);

        if (!v)
        {
            return finish(ctx, "the vendor is gone");
        }

        if (now - stageStarted_ > config.stageTimeout)
        {
            return merchantNeverOpened(s, ctx);
        }

        if (!ctx.turningRound(vendor_) && !ctx.movement.face(s.player, v->position, 0.4f))
        {
            return BehaviorStatus::Running;
        }

        if (!ctx.provider->supportsLua())
        {
            // No Lua: right-click the vendor under the mouse (1.60.1), as the
            // loot is; where it cannot be found there, the interact key.
            if (clickedVendor_ != vendor_)
            {
                clickedVendor_ = vendor_;
                rightClicks_ = 0;
            }

            if (!mouseTried_ && ctx.zoomInForClick(rightClicks_))
            {
                return BehaviorStatus::Running;
            }

            const auto seek = mouseTried_ ? BotContext::MouseSeek::Unsupported : (v ? ctx.seekNpcTurning(s.player, vendor_, v->position) : ctx.seekUnderMouse(vendor_));

            if (seek == BotContext::MouseSeek::Searching)
            {
                return BehaviorStatus::Running;
            }

            if (seek == BotContext::MouseSeek::Found)
            {
                ctx.actions.click(MouseButton::Right);
                ctx.stopMouseSeek();
                ++rightClicks_;
                ctx.log.info(logcat::Loot, "opening {} with a right-click", v->name);
                setStage(Stage::Selling, ctx);
                return BehaviorStatus::Running;
            }

            if (seek == BotContext::MouseSeek::NotFound)
            {
                ctx.stopMouseSeek();
            }

            mouseTried_ = true;

            // 2.5.6: the interact key opens the merchant frame, and the
            // bindings addon repairs and sells the greys when it does. Not
            // pressed yet (its binding was just typed, or a chat line was):
            // again on the next tick.
            if (ctx.actions.execute(actions::Interact) == ActionResult::InputUnavailable)
            {
                return BehaviorStatus::Running;
            }
            ctx.log.info(logcat::Loot, "opening {} with the interact key", v->name);
            setStage(Stage::Selling, ctx);
            return BehaviorStatus::Running;
        }

        if (!interactSent_)
        {
            interactSent_ = true;
            lastInteract_ = now;
            ctx.interactNpc(vendor_, v->position);

            if (ctx.actions.hasBinding(actions::Interact))
            {
                ctx.actions.execute(actions::Interact);
            }

            ctx.log.info(logcat::Loot, "opening {} (talking to NPC)", v->name);
            setStage(Stage::Selling, ctx);
            return BehaviorStatus::Running;
        }

        setStage(Stage::Selling, ctx);
        return BehaviorStatus::Running;
    }

    case Stage::Selling:
    {
        if (!ctx.canRunScripts() && ctx.provider->supportsUi())
        {
            return tickMerchantUi(s, ctx);
        }

        if (!ctx.canRunScripts())
        {
            // The addon works as the frame opens: done once the gear shows mended
            // and the bags gained room, or when the wait is up.
            const bool mended = !needsRepair(s, config);
            const bool sold = s.player.freeBagSlots > freeSlotsOnArrival_;

            if (!(mended && sold) && now - stageStarted_ < config.merchantAddonWait)
            {
                return BehaviorStatus::Running;
            }

            setStage(Stage::Repairing, ctx);
            return BehaviorStatus::Running;
        }

        // If a gossip frame is open (e.g. "I'd like to browse your goods"), select the vendor option first.
        if (ctx.uiFrameVisible("GossipFrame") && !ctx.uiFrameVisible("MerchantFrame"))
        {
            if (!gossipHandled_ && ctx.canRunScripts() && ctx.scriptIdle())
            {
                const std::string selectGossip = "if (not MerchantFrame or not MerchantFrame:IsVisible()) and GossipFrame and GossipFrame:IsVisible() then "
                                                 "local s=false "
                                                 "if C_GossipInfo and C_GossipInfo.GetOptions then "
                                                 "local o=C_GossipInfo.GetOptions() "
                                                 "for i,v in ipairs(o) do "
                                                 "local t=string.lower(v.name or '') local k=string.lower(v.type or '') "
                                                 "if k=='vendor' or string.find(t,'browse') or string.find(t,'goods') or string.find(t,'wares') or string.find(t,'buy') then "
                                                 "if C_GossipInfo.SelectOptionByIndex then C_GossipInfo.SelectOptionByIndex(i) "
                                                 "elseif C_GossipInfo.SelectOption then C_GossipInfo.SelectOption(v.gossipOptionID) "
                                                 "else SelectGossipOption(i) end "
                                                 "s=true break end end "
                                                 "if not s and #o>0 then "
                                                 "if C_GossipInfo.SelectOptionByIndex then C_GossipInfo.SelectOptionByIndex(1) "
                                                 "elseif C_GossipInfo.SelectOption then C_GossipInfo.SelectOption(o[1].gossipOptionID) "
                                                 "else SelectGossipOption(1) end end "
                                                 "elseif GetGossipOptions then "
                                                 "local o={GetGossipOptions()} "
                                                 "for i=1,#o,2 do "
                                                 "local t=string.lower(o[i] or '') local k=string.lower(o[i+1] or '') "
                                                 "local idx=math.floor((i+1)/2) "
                                                 "if k=='vendor' or string.find(t,'browse') or string.find(t,'goods') or string.find(t,'wares') or string.find(t,'buy') then "
                                                 "SelectGossipOption(idx) s=true break end end "
                                                 "if not s and #o>=2 then SelectGossipOption(1) end "
                                                 "end end";
                ctx.runScript(selectGossip);
                gossipHandled_ = true;
                stageStarted_ = now;
                return BehaviorStatus::Running;
            }

            if (now - stageStarted_ > config.stageTimeout)
            {
                return merchantNeverOpened(s, ctx);
            }

            return BehaviorStatus::Running;
        }

        // Give the client a moment to actually open the frame before selling.
        if (now - stageStarted_ < config.sellSettle)
        {
            return BehaviorStatus::Running;
        }

        if (!soldItems_)
        {
            soldItems_ = true;
            const LuaApi api = ctx.provider->vanillaLua() ? LuaApi::Vanilla : LuaApi::Wrath;
            const std::string script = buildSellScript(keepingQuestItems(config, s, ctx), gatherKeep(ctx), api);

            if (!script.empty())
            {
                if (!ctx.scriptIdle())
                {
                    return BehaviorStatus::Running;
                }

                if (!ctx.runScript(script))
                {
                    return finish(ctx, "could not run the sell script");
                }

                ctx.log.info(logcat::Loot, "selling at {}", s.findUnit(vendor_) ? s.findUnit(vendor_)->name : "vendor");
            }

            stageStarted_ = now;
            return BehaviorStatus::Running;
        }

        setStage(Stage::Repairing, ctx);
        return BehaviorStatus::Running;
    }

    case Stage::Repairing:
    {
        if (!repairedItems_)
        {
            repairedItems_ = true;
            const UnitState* v = s.findUnit(vendor_);
            const bool nameMatch = v && ((ctx.profile && !ctx.profile->vendorName.empty() && v->name == ctx.profile->vendorName) || (ctx.temporaryVendor && ctx.temporaryVendor->repairs && v->name == ctx.temporaryVendor->name));
            const bool canRepair = (v && v->canRepair) || nameMatch;
            const std::string script = buildRepairScript(config);

            if (!script.empty() && canRepair && ctx.canRunScripts())
            {
                if (!ctx.scriptIdle())
                {
                    return BehaviorStatus::Running;
                }

                ctx.runScript(script);
                ctx.log.info(logcat::Loot, "repairing at {}", v ? v->name : "vendor");
            }

            stageStarted_ = now;
            return BehaviorStatus::Running;
        }

        if (now - stageStarted_ < Duration(500))
        {
            return BehaviorStatus::Running;
        }

        freed_ = s.player.freeBagSlots - freeSlotsOnArrival_;

        // Close merchant and gossip frames cleanly
        if (ctx.canRunScripts() && ctx.scriptIdle())
        {
            ctx.runScript("if CloseMerchant then CloseMerchant() end; if CloseGossip then CloseGossip() end");
        }

        ctx.cancelInteraction();

        // Quest mode has no place to go back to: what comes next is its own
        // choice - the next quest's mobs, a hand-in - from the vendor on.
        if (ctx.config.quest.enabled)
        {
            ctx.log.info(logcat::Loot, "vendoring complete ({} bag slots freed) - quest mode goes on from here", freed_ > 0 ? std::to_string(freed_) : "0");
            const bool stillNeeded = bagsFull(s, config) || needsRepair(s, config);
            return finish(ctx, freed_ > 0 ? std::format("{} bag slots freed", freed_) : "nothing was sold", stillNeeded ? ctx.config.vendor.retryDelay : Duration(90000));
        }

        ctx.log.info(logcat::Loot, "vendoring complete ({} bag slots freed) - returning along vendor route", freed_ > 0 ? std::to_string(freed_) : "0");

        // Retrace the vendor route instead of cutting across country to the
        // nearest grind waypoint, which could be through anything.
        buildReturnPath(s, ctx);
        setStage(Stage::Returning, ctx);
        // The way home gets a time budget of its own: counted from the start
        // of the trip, it ran out halfway back.
        started_ = ctx.now();
        return BehaviorStatus::Running;
    }

    case Stage::Returning:
    {
        if (!walkPath(s, ctx))
        {
            return BehaviorStatus::Running;
        }

        ctx.navigator.stop();
        ctx.movement.stop();

        // Back at the grind/freeroam end of the vendor route.
        // Interlock: enforce at least a 90-second cooldown even on success, and retryDelay on failure,
        // so the bot never immediately turns around and restarts the vendor trip.
        const bool stillNeeded = bagsFull(s, config) || needsRepair(s, config);
        const Duration cooldown = stillNeeded ? ctx.config.vendor.retryDelay : Duration(90000);
        return finish(ctx, freed_ > 0 ? std::format("{} bag slots freed", freed_) : "nothing was sold", cooldown);
    }

    case Stage::Done:
        return BehaviorStatus::Success;
    }

    return BehaviorStatus::Running;
}

namespace
{
// What is left for the bags to be gone through item by item once Sell All Junk
// has taken every grey item (sellAllPoor), or everything when it has not.
bool sellsItemByItem(const VendorConfig& config)
{
    if (config.sellsNothing())
    {
        return false;
    }

    return (config.sellPoor && !config.sellAllPoor) || config.sellCommon || config.sellUncommon || !config.alwaysSell.empty() || config.sellsCategory();
}

} // namespace

// Sell All Junk first when every grey item goes. Then, on tooltip-capable
// clients, inspect each bag item that is left and sell it with a right-click.
// Other UI clients have only the Sell All Junk button.
BehaviorStatus VendorBehavior::tickMerchantUi(const GameSnapshot& s, BotContext& ctx)
{
    const VendorConfig& config = ctx.config.vendor;
    const TimePoint now = ctx.now();
    const auto next = [&](UiStep step)
    {
        uiStep_ = step;
        uiStepAt_ = now;
        return BehaviorStatus::Running;
    };
    const auto itemCount = [&]
    {
        int total = 0;

        for (const auto& item : s.player.bagItems)
        {
            total += item.count;
        }

        return total;
    };
    const auto moveTo = [&](const UiFrame& frame)
    {
        if (!frame.visible || !ctx.actions.mouseFree())
        {
            return false;
        }

        const auto size = ctx.actions.windowSize();

        if (!size)
        {
            return false;
        }

        ctx.actions.mouseMove(static_cast<int>(frame.centerX() * size->first + 0.5f), static_cast<int>(frame.centerY() * size->second + 0.5f));
        return true;
    };
    // After Sell All Junk: the bags item by item if anything besides grey goes.
    const UiStep afterJunk = ctx.provider->supportsUiBagSlots() && sellsItemByItem(config) ? UiStep::OpenBags : UiStep::Repair;

    // A vanished merchant must never turn a sale click into an item-use click.
    if (uiStep_ != UiStep::Open && uiStep_ != UiStep::Done && !ctx.uiFrameVisible("MerchantFrame"))
    {
        return finish(ctx, "the merchant window closed during the sale");
    }

    switch (uiStep_)
    {
    case UiStep::Open:
        if (!ctx.uiFrameVisible("MerchantFrame"))
        {
            if (ctx.uiFrameVisible("GossipFrame"))
            {
                // The line that opens the shop, by what it says: "I want to
                // browse your goods.", "I'd like to see what you have to
                // sell." (Farmer Saldean), "What do you have for sale?".
                for (std::string_view phrase : {"browse", "goods", "wares", "to sell", "for sale", "buy", "waren", "stöbern", "kaufen", "marchandise", "articles", "acheter", "mercanc", "mercador", "comprar", "товар", "купить", "물건", "상품", "貨物", "货物", "商品"})
                {
                    if (const auto opt = ctx.provider->uiFrameWithText("GossipFrame", phrase); opt && opt->visible)
                    {
                        moveTo(*opt);
                        ctx.actions.click(MouseButton::Left);
                        ctx.log.info(logcat::Loot, "clicked gossip option '{}' to open vendor", phrase);
                        uiStepAt_ = now;
                        return BehaviorStatus::Running;
                    }
                }

                if (ctx.uiFrameVisible("GossipTitleButton1"))
                {
                    ctx.clickUiFrame("GossipTitleButton1");
                    ctx.log.info(logcat::Loot, "clicked GossipTitleButton1 to open vendor");
                    uiStepAt_ = now;
                    return BehaviorStatus::Running;
                }
            }

            // A right-click that opened nothing is made again, the last time
            // with the camera all the way in (BotContext::zoomInForClick).
            if (!ctx.uiFrameVisible("GossipFrame") && rightClicks_ > 0 && rightClicks_ <= ctx.config.zoomInAfterClicks && now - stageStarted_ > kClickAnswerWait)
            {
                ctx.log.info(logcat::Loot, "the right-click opened nothing - clicking the vendor again");
                setStage(Stage::Opening, ctx);
                return BehaviorStatus::Running;
            }

            if (now - stageStarted_ > config.stageTimeout)
            {
                return merchantNeverOpened(s, ctx);
            }

            return BehaviorStatus::Running;
        }

        rightClicks_ = 0;

        // The window fills its item list a moment after it shows.
        if (now - uiStepAt_ < config.sellSettle)
        {
            return BehaviorStatus::Running;
        }

        {
            const bool tooltips = ctx.provider->supportsUiBagSlots();

            // Anything better than grey needs the bags gone through item by item.
            if (!tooltips && (config.sellCommon || config.sellUncommon))
            {
                ctx.log.info(logcat::Loot, "this client sells grey items only (Sell All Junk) - {} items are kept", config.sellCommon && config.sellUncommon ? "white and green" : config.sellCommon ? "white" : "green");
            }

            popupsBeforeJunk_ = 0;

            for (int i = 1; i <= 4; ++i)
            {
                popupsBeforeJunk_ |= ctx.uiFrameVisible("StaticPopup" + std::to_string(i)) ? 1u << (i - 1) : 0u;
            }

            // Every grey goes, whatever the never-sell list says: one click
            // does that, and the bags are then gone through for the rest only.
            // Without tooltips the button is the only way to sell at all.
            if ((config.sellAllPoor || (!tooltips && config.sellPoor)) && ctx.clickUiFrame("MerchantSellAllJunkButton"))
            {
                ctx.log.info(logcat::Loot, "selling the grey items at {}", s.findUnit(vendor_) ? s.findUnit(vendor_)->name : "vendor");
                return next(UiStep::Confirm);
            }

            return next(tooltips && sellsItemByItem(config) ? UiStep::OpenBags : UiStep::Repair);
        }

    case UiStep::Confirm:
    {
        // "Are you sure...junk": the one dialog to say yes to. None comes when
        // there was nothing to sell. Identified across client languages
        // (including zhTW's "你將會售出所有垃圾物品...") and only among popups
        // that came up after Sell All Junk was clicked: an invite or duel is
        // never said yes to.
        const auto isJunkConfirm = [&](const std::string& popup)
        {
            const auto text = ctx.uiText(popup, popup + "Text");

            if (!text)
            {
                return false;
            }

            const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };

            for (std::string_view word : {"junk", "ramsch", "camelote", "objets gris", "chatarra", "sucata", "cianfrusagli", "хлам", "잡동사니", "垃圾"})
            {
                if (std::search(text->begin(), text->end(), word.begin(), word.end(), [&](char a, char b) { return lower(a) == lower(b); }) != text->end())
                {
                    return true;
                }
            }

            return false;
        };

        for (int i = 1; i <= 4; ++i)
        {
            const std::string popup = "StaticPopup" + std::to_string(i);

            if ((popupsBeforeJunk_ & (1u << (i - 1))) == 0 && ctx.uiFrameVisible(popup) && isJunkConfirm(popup) && ctx.clickUiFrame(popup + "Button1"))
            {
                return next(afterJunk);
            }
        }

        if (now - uiStepAt_ > Duration(3000))
        {
            return next(afterJunk);
        }

        return BehaviorStatus::Running;
    }
    case UiStep::OpenBags:
        if (!ctx.provider->uiBagSlots().empty())
        {
            return next(UiStep::ScanBags);
        }

        if (!openedBags_ && ctx.clickUiFrame("MainMenuBarBackpackButton"))
        {
            openedBags_ = true;
            uiStepAt_ = now;
        }

        if (now - uiStepAt_ > config.stageTimeout)
        {
            ctx.log.warn(logcat::Loot, "could not read the open bags at the merchant");
            return next(UiStep::Repair);
        }

        return BehaviorStatus::Running;
    case UiStep::ScanBags:
    {
        const auto slots = ctx.provider->uiBagSlots();
        const auto slot = std::find_if(slots.begin(), slots.end(), [&](const UiBagSlot& candidate) { return !visitedBagButtons_.contains(candidate.token); });

        if (slot == slots.end())
        {
            return next(UiStep::Repair);
        }

        // Every full slot looked at: what is left is empty, and an empty slot
        // costs a wait for a tooltip that never comes. The full slots counted
        // are those of every bag, shown or not, so this never stops too soon.
        if (s.player.usedBagSlots >= 0 && keptBagSlots_ >= s.player.usedBagSlots)
        {
            ctx.log.debug(logcat::Loot, "every full bag slot looked at - the rest are empty");
            return next(UiStep::Repair);
        }

        // Straight on to the next slot: the tooltip's owner says which slot it
        // is for, so two stacks of the same name side by side are not mixed up.
        uiBagSlot_ = *slot;
        uiTooltip_.reset();
        return next(UiStep::HoverBag);
    }

    case UiStep::HoverBag:
    {
        const auto slots = ctx.provider->uiBagSlots();
        const auto slot = std::find_if(slots.begin(), slots.end(), [&](const UiBagSlot& candidate) { return uiBagSlot_ && candidate.token == uiBagSlot_->token; });

        if (slot == slots.end())
        {
            return next(UiStep::ScanBags);
        }

        if (!moveTo(slot->frame))
        {
            return BehaviorStatus::Running;
        }

        uiBagSlot_ = *slot;
        uiHoverStarted_ = now;
        return next(UiStep::ReadTooltip);
    }

    case UiStep::ReadTooltip:
    {
        if (now - uiHoverStarted_ > Duration(3000))
        {
            if (uiBagSlot_)
            {
                visitedBagButtons_.insert(uiBagSlot_->token);
            }

            return next(UiStep::ScanBags);
        }

        if (now - uiStepAt_ < Duration(100))
        {
            return BehaviorStatus::Running;
        }

        const auto item = ctx.provider->uiItemTooltip();

        // An empty slot shows no tooltip; the client shows one within a frame
        // or two of the mouse arriving.
        if (!item || !uiBagSlot_ || item->owner != uiBagSlot_->token || item->name.empty())
        {
            // Twice the slowest tooltip seen, so an empty slot is not waited
            // on five times as long as a full one takes; 500 ms until one is seen.
            const Duration emptyAfter = slowestTooltip_ ? std::clamp(*slowestTooltip_ * 2, Duration(200), Duration(500)) : Duration(500);

            if (now - uiStepAt_ < emptyAfter)
            {
                return BehaviorStatus::Running;
            }

            if (uiBagSlot_)
            {
                visitedBagButtons_.insert(uiBagSlot_->token);
            }

            return next(UiStep::ScanBags);
        }

        if (!uiTooltip_)
        {
            slowestTooltip_ = std::max(slowestTooltip_.value_or(Duration(0)), now - uiHoverStarted_);
        }

        if (!shouldSellTooltip(keepingQuestItems(config, s, ctx), *item))
        {
            visitedBagButtons_.insert(uiBagSlot_->token);
            ++keptBagSlots_;
            return next(UiStep::ScanBags);
        }

        // Require two matching samples on separate ticks before selling.
        if (!uiTooltip_ || uiTooltip_->name != item->name || uiTooltip_->quality != item->quality || uiTooltip_->lines != item->lines)
        {
            uiTooltip_ = *item;
            uiStepAt_ = now;
            return BehaviorStatus::Running;
        }

        const auto slots = ctx.provider->uiBagSlots();
        const auto slot = std::find_if(slots.begin(), slots.end(), [&](const UiBagSlot& candidate) { return candidate.token == uiBagSlot_->token; });

        if (slot == slots.end() || slot->frame.left != uiBagSlot_->frame.left || slot->frame.top != uiBagSlot_->frame.top || slot->frame.right != uiBagSlot_->frame.right || slot->frame.bottom != uiBagSlot_->frame.bottom)
        {
            return next(UiStep::ScanBags);
        }

        if (!ctx.actions.mouseFree() || !ctx.uiFrameVisible("MerchantFrame"))
        {
            return BehaviorStatus::Running;
        }

        const auto current = ctx.provider->uiItemTooltip();

        if (!current || current->owner != item->owner || current->name != item->name || current->quality != item->quality || current->lines != item->lines)
        {
            return next(UiStep::ScanBags);
        }

        slotsBeforeUiSale_ = s.player.freeBagSlots;
        itemsBeforeUiSale_ = itemCount();
        ctx.actions.click(MouseButton::Right);
        visitedBagButtons_.insert(uiBagSlot_->token);
        ctx.log.info(logcat::Loot, "selling {} (quality {}) with a right-click", item->name, item->quality);
        return next(UiStep::VerifySale);
    }

    case UiStep::VerifySale:
    {
        // On as soon as the bags show the sale; sellSettle is only the limit.
        const bool sold = (slotsBeforeUiSale_ >= 0 && s.player.freeBagSlots > slotsBeforeUiSale_) || itemCount() < itemsBeforeUiSale_;

        if (sold)
        {
            ctx.log.info(logcat::Loot, "sale of {} confirmed in the bags", uiTooltip_->name);
            return next(UiStep::ScanBags);
        }

        if (now - uiStepAt_ < config.sellSettle)
        {
            return BehaviorStatus::Running;
        }

        ctx.log.warn(logcat::Loot, "sale of {} was not confirmed; leaving that slot alone", uiTooltip_->name);
        ++keptBagSlots_;
        return next(UiStep::ScanBags);
    }

    case UiStep::Repair:
        if (now - uiStepAt_ < Duration(700))
        {
            return BehaviorStatus::Running;
        }

        if (config.repair && needsRepair(s, config) && ctx.clickUiFrame("MerchantRepairAllButton"))
        {
            ctx.log.info(logcat::Loot, "repairing at {}", s.findUnit(vendor_) ? s.findUnit(vendor_)->name : "vendor");
        }

        return next(UiStep::Close);
    case UiStep::Close:
        // Room in the bags and mended gear show a moment after the clicks.
        if (now - uiStepAt_ < config.sellSettle)
        {
            return BehaviorStatus::Running;
        }

        if (openedBags_)
        {
            ctx.clickUiFrame("MainMenuBarBackpackButton");
        }

        ctx.clickUiFrame("MerchantFrameCloseButton");

        if (ctx.uiFrameVisible("GossipFrameCloseButton"))
        {
            ctx.clickUiFrame("GossipFrameCloseButton");
        }

        uiStep_ = UiStep::Done;
        setStage(Stage::Repairing, ctx);
        repairedItems_ = true; // done through the window already
        stageStarted_ = now;
        return BehaviorStatus::Running;
    case UiStep::Done:
        break;
    }

    setStage(Stage::Repairing, ctx);
    return BehaviorStatus::Running;
}

} // namespace og
