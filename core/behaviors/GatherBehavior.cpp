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
 *  File: GatherBehavior.cpp
 *
 *  Desc:
 *  Implementation of GatherBehavior (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/behaviors/Behaviors.h"

#include "og/actions/ActionBinding.h"
#include "og/core/Events.h"
#include "og/core/GatheringData.h"
#include "og/navigation/Roaming.h"
#include "og/state/IGameStateProvider.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <cmath>
#include <limits>
#include <numbers>

namespace og
{
namespace
{

const GatheringProfessionConfig* professionFor(GameObjectKind kind, const GatheringConfig& config)
{
    if (kind == GameObjectKind::Herb)
    {
        return &config.herbalism;
    }

    if (kind == GameObjectKind::Mineral)
    {
        return &config.mining;
    }

    return nullptr;
}

int itemCount(const std::vector<ItemStack>& bags, std::uint32_t itemId)
{
    int count = 0;

    for (const auto& stack : bags)
    {
        if (stack.itemId == itemId)
        {
            count += stack.count;
        }
    }

    return count;
}

std::vector<QuestObjectTarget> questItemsFor(const GameObjectState& node, const GameSnapshot& s, const BotContext& ctx)
{
    std::vector<QuestObjectTarget> items;

    if (!ctx.config.quest.enabled || ctx.combat.defendOnly || node.entry == 0)
    {
        return items;
    }

    for (const auto& quest : ctx.questTargets)
    {
        if (!isLowestLevelQuest(quest, ctx.questTargets))
        {
            continue;
        }

        if (quest.done || std::find(s.player.questLog.begin(), s.player.questLog.end(), quest.questId) == s.player.questLog.end())
        {
            continue;
        }

        for (const auto& object : quest.objects)
        {
            // (With no item, one to use for credit: listed only while wanted.)
            if (object.entry == node.entry && (object.itemId == 0 || object.wanted <= 0 || itemCount(s.player.bagItems, object.itemId) < object.wanted))
            {
                auto target = object;
                target.questId = quest.questId;
                items.push_back(target);
            }
        }
    }

    return items;
}

// Close enough to gather: within reach across the ground, and not far above or
// below. The navigator arrives by the distance across the ground, so a node on
// a slope or a rock must not need the straight-line distance as well: the bot
// stood right under one until the approach timed out.
constexpr float kReachHeight = 5.0f;

bool inReach(const Vec3& player, const Vec3& node, float reach)
{
    return distance2D(player, node) <= reach && std::abs(player.z - node.z) <= kReachHeight;
}

// An object used for quest credit - a Tarantula Egg to destroy - gives nothing
// into the bags: it is done when it is gone.
bool usedForCredit(const std::vector<QuestObjectTarget>& items)
{
    return std::any_of(items.begin(), items.end(), [](const QuestObjectTarget& item) { return item.itemId == 0; });
}

bool bagIncreased(const std::vector<ItemStack>& before, const std::vector<ItemStack>& after)
{
    for (const auto& item : after)
    {
        const auto old = std::find_if(before.begin(), before.end(), [&](const ItemStack& candidate) { return item.itemId != 0 ? candidate.itemId == item.itemId : candidate.name == item.name; });

        if (item.count > (old == before.end() ? 0 : old->count))
        {
            return true;
        }
    }

    return false;
}

// Distance across the ground from `p` to the leg a-b.
float distanceToLeg(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float lengthSquared = dx * dx + dy * dy;

    if (lengthSquared <= 0.0f)
    {
        return distance2D(p, a);
    }

    const float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared, 0.0f, 1.0f);
    return distance2D(p, Vec3{a.x + dx * t, a.y + dy * t, a.z});
}

// How many nodes beyond the one gone for the detour is planned on through.
constexpr std::size_t kChainedNodes = 4;
// Closer to the node than this, the approach takes over from the detour.
constexpr float kDetourEnds = 8.0f;

// The tracker's titles are kept in lower case.
bool sameTitle(std::string_view a, std::string_view b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

} // namespace

bool GatherBehavior::questProgressAdvanced(const GameSnapshot& s, const BotContext& ctx) const
{
    for (const auto& item : questItems_)
    {
        if (item.questId == 0)
        {
            continue;
        }

        const auto it = std::find_if(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const QuestTarget& q) {
            return q.questId == item.questId;
        });

        if (it != ctx.questTargets.end())
        {
            if (it->done)
            {
                return true;
            }

            const bool stillWanted = std::any_of(it->objects.begin(), it->objects.end(), [&](const QuestObjectTarget& obj) {
                return obj.entry == item.entry;
            });
            if (!stillWanted)
            {
                return true;
            }
        }
    }

    for (const auto& item : questItems_)
    {
        if (item.questId == 0)
        {
            continue;
        }

        const auto prev = std::find_if(questProgressBefore_.begin(), questProgressBefore_.end(), [&](const auto& p) {
            return p.questId == item.questId;
        });
        const auto cur = std::find_if(s.player.questProgress.begin(), s.player.questProgress.end(), [&](const auto& p) {
            return p.questId == item.questId;
        });

        if (cur != s.player.questProgress.end())
        {
            if (prev == questProgressBefore_.end())
            {
                return true;
            }

            for (std::size_t i = 0; i < cur->kills.size(); ++i)
            {
                if (cur->kills[i] > prev->kills[i])
                {
                    return true;
                }
            }
        }
    }

    for (const auto& [title, objectives] : s.player.trackedQuests)
    {
        // Another quest moving on in the meantime says nothing about this object.
        const bool gathered = std::any_of(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const QuestTarget& q) {
            return sameTitle(q.questName, title) && std::any_of(questItems_.begin(), questItems_.end(), [&](const auto& item) { return item.questId == q.questId; });
        });

        const auto it = trackedQuestsBefore_.find(title);
        if (gathered && it != trackedQuestsBefore_.end())
        {
            for (std::size_t i = 0; i < std::min(objectives.size(), it->second.size()); ++i)
            {
                if (objectives[i].have > it->second[i].have)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool GatherBehavior::safe(const GameObjectState& node, const GameSnapshot& s, const BotContext& ctx, bool questNode) const
{
    return std::isfinite(node.position.x) && std::isfinite(node.position.y) && std::isfinite(node.position.z) && std::abs(node.position.z - s.player.position.z) <= ctx.config.gathering.maxVerticalDifference && !ctx.areaBlacklist.contains(node.position) && (questNode || !ctx.config.gathering.skipHostileNodes || s.hostilesNear(node.position, ctx.config.gathering.dangerRadius).empty());
}

std::vector<const GameObjectState*> GatherBehavior::candidates(const GameSnapshot& s, BotContext& ctx) const
{
    std::vector<std::pair<float, const GameObjectState*>> scored;

    for (const auto& node : s.objects)
    {
        const bool questNode = !questItemsFor(node, s, ctx).empty();
        const auto* profession = professionFor(node.kind, ctx.config.gathering);

        if ((!questNode && (!profession || !profession->enabled || !canGatherNode(node, s.player))) || node.guid == 0 || !node.canInteract || ctx.objectBlacklist.contains(node.guid) || !safe(node, s, ctx, questNode))
        {
            continue;
        }

        if (!questNode && !profession->nodes.empty() && std::find(profession->nodes.begin(), profession->nodes.end(), node.name) == profession->nodes.end())
        {
            continue;
        }

        // As far as the profile's gather scan range (0: no limit; without a
        // profile, the profession's own), and only inside the area its route
        // outlines, clear of No Go zones and learned obstacles. Inside it, a
        // node is always gone for: the lap passing it later is no reason to wait.
        const float reach = !ctx.profile ? (questNode ? 150.0f : profession->maxDetourDistance) : ctx.profile->options.gatherDistance > 0 ? ctx.profile->options.gatherDistance : std::numeric_limits<float>::max();
        const float dist = distance(s.player.position, node.position);

        if (dist > reach)
        {
            continue;
        }

        if (!(ctx.config.usePathfinding && ctx.config.quest.enabled) && ctx.route && outlinesArea(ctx.route->points()) && !insideRoute(ctx.route->points(), node.position))
        {
            continue;
        }

        // Roam this zone: a node over the border is left where it is.
        if (ctx.config.roam.enabled && !insideZone(&ctx.navigator.provider(), ctx.config.roam.zone, node.position))
        {
            continue;
        }

        if (ctx.obstacles && ctx.obstacles->avoids(node.position))
        {
            continue;
        }

        scored.emplace_back(dist + 2.0f * std::abs(node.position.z - s.player.position.z), &node);
    }

    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second->guid < b.second->guid; });
    std::vector<const GameObjectState*> out;
    out.reserve(scored.size());

    for (const auto& [score, node] : scored)
    {
        out.push_back(node);
    }

    return out;
}

const GameObjectState* GatherBehavior::selectTarget(const GameSnapshot& s, BotContext& ctx) const
{
    const auto nodes = candidates(s, ctx);
    if (nodes.empty())
    {
        return nullptr;
    }

    if (preferredTargetGuid_ != 0)
    {
        const auto it = std::find_if(nodes.begin(), nodes.end(), [this](const GameObjectState* n) {
            return n->guid == preferredTargetGuid_;
        });
        if (it != nodes.end())
        {
            return *it;
        }
    }

    return nodes.front();
}

const UnitState* GatherBehavior::mobInTheWay(const GameSnapshot& s, BotContext& ctx) const
{
    // Near enough that walking by it would pull it anyway.
    constexpr float kInTheWay = 12.0f;

    // Travelling with a purpose (a vendor trip): no fights looked for.
    if (!ctx.combatClass || ctx.combat.defendOnly)
    {
        return nullptr;
    }

    // Quest containers can be guarded by mobs outside the quest's kill list.
    // Use the same aggro-range check as travelling to a quest giver/vendor.
    if (!questItems_.empty() && s.player.isMoving)
    {
        if (const auto* threat = ctx.threatInTheWay(s))
        {
            return threat;
        }
    }

    const float toNode = distance(s.player.position, target_->position);
    std::vector<Vec3> ahead{s.player.position};

    for (std::size_t i = wayIndex_; i <= wayToNode_ && i < way_.size(); ++i)
    {
        ahead.push_back(way_[i]);
    }

    ahead.push_back(target_->position);

    for (const auto& candidate : ctx.targets.rank(s))
    {
        const UnitState* mob = candidate.unit;

        // A neutral mob does not come for us on the way: a Great Goretusk was
        // "in the way to Bruiseweed" and fought first. Pulled when grinding,
        // not on the way to a node.
        if (mob->reaction != Reaction::Hostile || distance(s.player.position, mob->position) >= toNode)
        {
            continue;
        }

        for (std::size_t i = 0; i + 1 < ahead.size(); ++i)
        {
            if (distanceToLeg(mob->position, ahead[i], ahead[i + 1]) <= kInTheWay)
            {
                return mob;
            }
        }
    }

    return nullptr;
}

void GatherBehavior::planWay(const GameSnapshot& s, BotContext& ctx)
{
    way_.clear();
    wayIndex_ = wayToNode_ = 0;
    wayRevision_ = ctx.obstacles ? ctx.obstacles->revision() : 0;

    // Client geometry already supplies the walkable way around a tree. The
    // profile detour's interpolated points can lie inside it or below ground,
    // causing a failed route and a stop for each point on successive ticks.
    if (ctx.navigator.provider().usesClientGeometry())
    {
        return;
    }

    // The node, then on from each to the nearest of the rest not yet in the chain.
    std::vector<Vec3> stops{target_->position};
    auto rest = candidates(s, ctx);
    std::erase_if(rest, [&](const GameObjectState* n) { return n->guid == target_->guid; });

    while (stops.size() <= kChainedNodes && !rest.empty())
    {
        const auto next = std::min_element(rest.begin(), rest.end(), [&](const auto* a, const auto* b) { return distance(stops.back(), a->position) < distance(stops.back(), b->position); });
        stops.push_back((*next)->position);
        rest.erase(next);
    }

    static const std::vector<Vec3> noArea;
    const auto& area = ctx.route ? ctx.route->points() : noArea;
    std::vector<std::size_t> ends;
    way_ = detourPath(area, s.player.position, stops, ctx.obstacles, wayRandom_, &ends);

    if (way_.empty() || ends.empty())
    {
        way_.clear();
        return;
    }

    wayToNode_ = ends.front();
    ctx.log.info("GATHER", "the way to {}: {} points{}", target_->name, wayToNode_ + 1, stops.size() > 1 ? std::format(", then on through {} more node(s)", stops.size() - 1) : std::string());
}

std::string GatherBehavior::stopReason(const GameSnapshot& s, BotContext& ctx) const
{
    if (s.isDead())
    {
        return "dead";
    }

    if (s.inCombat() || !s.attackers().empty() || ctx.combat.engaged != 0)
    {
        return "in combat";
    }

    auto cc = ctx.combatContext(s);

    if (ctx.needsRest(s) || (!interacting_ && ctx.combatClass && ctx.combatClass->wantsStationaryBuff(cc)))
    {
        return "needs rest";
    }

    if (ctx.fishingEnabled())
    {
        return "fishing";
    }

    const auto* profession = professionFor(target_->kind, ctx.config.gathering);

    if (!questItems_.empty())
    {
        if (!ctx.config.quest.enabled)
        {
            return "questing turned off";
        }

        if (!interacting_ && questItemsFor(*target_, s, ctx).empty())
        {
            return "quest item no longer needed";
        }
    }
    else if (!profession || !profession->enabled)
    {
        return "profession turned off";
    }

    // Not the height: that picks nodes, but a slope on the way there changes it.
    if (ctx.areaBlacklist.contains(target_->position))
    {
        return "the spot is blacklisted";
    }

    if (const auto near = s.hostilesNear(target_->position, ctx.config.gathering.dangerRadius); questItems_.empty() && ctx.config.gathering.skipHostileNodes && !near.empty())
    {
        return std::format("{} (level {}) within {:.0f} yd of it", near.front()->name, near.front()->level, ctx.config.gathering.dangerRadius);
    }

    if (questItems_.empty() && !canGatherNode(*target_, s.player))
    {
        return "skill too low";
    }

    return {};
}

bool GatherBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes, and nowhere on its own.
    if (ctx.partyFollower())
    {
        return false;
    }

    // Through memory, the node under the mouse (1.60.1), or the interact key.
    if (!ctx.provider || (!ctx.provider->supportsObjectInteraction() && !ctx.provider->mouseoverGuid() && !ctx.config.gathering.interactKey))
    {
        return false;
    }

    if (target_)
    {
        stopReason_ = stopReason(s, ctx);
        return stopReason_.empty();
    }

    // Not before a rest, and not in the middle of one: it goes on to where it ends.
    auto cc = ctx.combatContext(s);

    if (ctx.fishingEnabled() || s.isDead() || s.inCombat() || !s.attackers().empty() || ctx.combat.engaged != 0 || ctx.needsRest(s) || ctx.midRest(s) || (ctx.combatClass && ctx.combatClass->wantsStationaryBuff(cc)))
    {
        return false;
    }

    if (s.player.isCasting)
    {
        return false;
    }

    // A visit to a quest NPC is left for a node on the way there, not while
    // talking to it: the detour would shut its window.
    if (ctx.objectives.contains("QuestHandIn") && !ctx.questTravel)
    {
        return false;
    }

    // Let an interrupted rest finish before taking another detour.
    if (s.player.isSitting && !ctx.restComplete(s))
    {
        return false;
    }

    const auto* node = selectTarget(s, ctx);

    // Full bags take nothing more - but an object used for quest credit
    // gives nothing to carry.
    return node && (s.player.freeBagSlots != 0 || usedForCredit(questItemsFor(*node, s, ctx)));
}

std::optional<Vec3> GatherBehavior::approachDestination(const GameSnapshot& s, BotContext& ctx) const
{
    const auto* node = selectTarget(s, ctx);
    return node ? std::optional(node->position) : std::nullopt;
}

void GatherBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    const auto* node = selectTarget(s, ctx);

    if (!node)
    {
        return;
    }

    target_ = *node;
    preferredTargetGuid_ = node->guid;
    questItems_ = questItemsFor(*node, s, ctx);
    stopReason_.clear();
    approaches_.clear();
    profession_ = questItems_.empty() ? *professionFor(node->kind, ctx.config.gathering) : GatheringProfessionConfig{};
    started_ = phaseStarted_ = ctx.now();
    closest_ = distance2D(s.player.position, node->position);
    progressAt_ = s.player.position;
    failures_ = harvests_ = 0;
    gathered_ = interacting_ = false;
    castStarted_ = false;
    interactedOnce_ = false;
    questProgressBefore_ = s.player.questProgress;
    trackedQuestsBefore_ = s.player.trackedQuests;
    missingSince_.reset();
    lootReceivedAt_.reset();
    settledSince_.reset();
    reposition_.reset();
    planWay(s, ctx);
    ctx.objectives.push(Objective{"GatherNode", node->guid, node->position});
    ctx.log.info("GATHER", "detouring to {} {}", node->name, formatGuid(node->guid));

    if (!questItems_.empty())
    {
        ctx.log.info(logcat::Profile, "quest mode: {} {}", usedForCredit(questItems_) ? "using the quest object" : "collecting from the quest object", node->name);
    }

    ctx.events.publish(GatherStartedEvent{node->guid, node->kind, node->name});
}

void GatherBehavior::onYield(BotContext& ctx, std::string_view next)
{
    yieldingTo_ = std::string(next);
    onExit(ctx);
    yieldingTo_.clear();
}

void GatherBehavior::onExit(BotContext& ctx)
{
    if (target_)
    {
        ctx.log.info("GATHER", "gathering {} interrupted{}", target_->name, stopReason_.empty() ? std::string() : ": " + stopReason_);
        ctx.events.publish(GatherCompletedEvent{target_->guid, target_->kind, gathered_, "interrupted"});

        const bool hostileNearby = !stopReason_.empty() && stopReason_.find("within") != std::string::npos && stopReason_.find("yd of it") != std::string::npos;
        const bool ungatherable = hostileNearby || stopReason_ == "the spot is blacklisted" || stopReason_ == "skill too low";
        const bool fightingMob = yieldingTo_ == "Combat" || yieldingTo_ == "Pull";

        if (ungatherable && !fightingMob)
        {
            const Duration duration = profession_.blacklistDuration.count() > 0 ? profession_.blacklistDuration : ctx.config.blacklistDuration;
            ctx.objectBlacklist.add(target_->guid, duration, stopReason_);
            ++ctx.stats.gatheringFailures;
            ++ctx.stats.blacklisted;
            preferredTargetGuid_ = 0;
        }
        else if (fightingMob || stopReason_ == "in combat" || stopReason_ == "needs rest" || stopReason_.empty())
        {
            preferredTargetGuid_ = target_->guid;
        }
        else
        {
            preferredTargetGuid_ = 0;
        }
    }

    if (ctx.provider)
    {
        ctx.provider->cancelObjectInteraction();
    }

    ctx.stopMouseSeek();
    ctx.restoreCamera();
    ctx.navigator.stop();
    ctx.objectives.removeAll("GatherNode");
    target_.reset();
    questItems_.clear();
    questProgressBefore_.clear();
    trackedQuestsBefore_.clear();
    castStarted_ = false;
    interactedOnce_ = false;
}

BehaviorStatus GatherBehavior::finish(BotContext& ctx, const std::string& reason)
{
    preferredTargetGuid_ = 0;
    const bool success = gathered_;
    ctx.log.info("GATHER", "{} {}: {}", target_->name, success ? "gathered" : "not gathered", reason);
    ctx.events.publish(GatherCompletedEvent{target_->guid, target_->kind, success, reason});
    ctx.objectBlacklist.add(target_->guid, profession_.blacklistDuration, reason);

    if (!success)
    {
        ++ctx.stats.gatheringFailures;
        ++ctx.stats.blacklisted;
    }

    target_.reset();
    onExit(ctx);
    return success ? BehaviorStatus::Success : BehaviorStatus::Failure;
}

void GatherBehavior::planApproach(const GameSnapshot& s, BotContext& ctx)
{
    approaches_.clear();
    approachIndex_ = 0;
    approachReached_ = false;
    approachRevision_ = ctx.obstacles ? ctx.obstacles->revision() : 0;
    const Vec3& node = target_->position;

    if (ctx.navigator.canStandAt(node))
    {
        approaches_.push_back(node);
        return;
    }

    // Rings round the node, the closest first and each nearest the bot first:
    // getting stuck on the way leaves an obstacle right by the node, and the
    // way round it comes from another side.
    const float reach = ctx.config.gathering.interactionDistance;

    for (const float radius : {reach * 0.8f, 4.0f, 6.0f, 9.0f, 12.0f})
    {
        std::vector<Vec3> ring;

        for (int i = 0; i < 16; ++i)
        {
            const float a = static_cast<float>(i) * 2.0f * std::numbers::pi_v<float> / 16.0f;
            const Vec3 spot = node + Vec3{std::cos(a) * radius, std::sin(a) * radius, 0.0f};

            if (ctx.navigator.canStandAt(spot))
            {
                ring.push_back(spot);
            }
        }

        std::sort(ring.begin(), ring.end(), [&](const Vec3& a, const Vec3& b) { return distance2D(s.player.position, a) < distance2D(s.player.position, b); });
        approaches_.insert(approaches_.end(), ring.begin(), ring.end());
    }

    if (approaches_.empty())
    {
        approaches_.push_back(node);
    }
    else
    {
        ctx.log.info("GATHER", "{} lies by an obstacle - going round to {} ({:.0f} yd from it)", target_->name, approaches_.front(), distance2D(approaches_.front(), node));
    }
}

void GatherBehavior::retry(const GameSnapshot& s, BotContext& ctx)
{
    ctx.provider->cancelObjectInteraction();
    ctx.navigator.stop();
    ++failures_;
    interacting_ = false;
    castStarted_ = false;
    settledSince_.reset();
    phaseStarted_ = ctx.now();
    // Retry from the side, then from the opposite side of the node.
    const Vec3 delta = s.player.position - target_->position;
    const float angle = std::atan2(delta.y, delta.x) + std::numbers::pi_v<float> / 2.0f;
    const float radius = ctx.config.gathering.interactionDistance * 0.65f;
    reposition_ = target_->position + Vec3{std::cos(angle) * radius, std::sin(angle) * radius, 0};
    ctx.log.info("GATHER", "retrying {} from another angle (retry {})", target_->name, failures_);
}

BehaviorStatus GatherBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    // The engine may keep this behavior selected between consecutive nodes.
    if (!target_)
    {
        onEnter(s, ctx);
    }

    if (!target_)
    {
        return BehaviorStatus::Success;
    }

    const auto now = ctx.now();
    const auto& config = ctx.config.gathering;
    const auto* node = s.findObject(target_->guid);

    const bool received = questItems_.empty() ? bagIncreased(bagsBefore_, s.player.bagItems) : std::any_of(questItems_.begin(), questItems_.end(), [&](const auto& item) { return itemCount(s.player.bagItems, item.itemId) > itemCount(bagsBefore_, item.itemId); });

    if (interacting_ && !lootReceivedAt_ && received)
    {
        lootReceivedAt_ = now;
        ctx.seekTook(target_->guid);
        ++harvests_;
        failures_ = 0;

        if (!gathered_)
        {
            gathered_ = true;

            if (questItems_.empty())
            {
                if (target_->kind == GameObjectKind::Herb)
                {
                    ++ctx.stats.herbsGathered;
                }
                else
                {
                    ++ctx.stats.mineralsGathered;
                }
            }
        }
    }

    if (now - started_ >= config.nodeTimeout)
    {
        return finish(ctx, "node time limit reached");
    }

    if (!node || !node->canInteract)
    {
        if ((interacting_ || interactedOnce_) && usedForCredit(questItems_))
        {
            gathered_ = true;
        }

        if (!missingSince_)
        {
            missingSince_ = now;
            ctx.navigator.stop();
            ctx.provider->cancelObjectInteraction();
        }

        if (!gathered_ && !usedForCredit(questItems_) && ctx.provider && ctx.provider->supportsLua() && ctx.provider->luaIdle())
        {
            ctx.provider->executeLua("if GetNumLootItems()>0 then for i=GetNumLootItems(),1,-1 do LootSlot(i) end end");
        }

        // Bag updates can arrive a little later than node despawn.
        if (now - *missingSince_ < config.lootGrace)
        {
            return BehaviorStatus::Running;
        }

        return finish(ctx, !gathered_ ? "node disappeared without verified loot" : usedForCredit(questItems_) ? "used, and gone" : "loot verified in bags");
    }

    missingSince_.reset();

    if (s.player.freeBagSlots == 0 && !usedForCredit(questItems_))
    {
        return finish(ctx, "bags are full");
    }

    if (interacting_)
    {
        if (usedForCredit(questItems_))
        {
            // Never move or send another interaction while a gathering cast is active.
            if (s.player.isCasting)
            {
                castStarted_ = true;
                ctx.seekTook(target_->guid);
                return BehaviorStatus::Running;
            }

            // An object used for credit completes when its cast finishes, when quest
            // progress is observed, or after a short delay for instant interactions.
            if (castStarted_)
            {
                gathered_ = true;
                return finish(ctx, "used");
            }

            if (questProgressAdvanced(s, ctx))
            {
                gathered_ = true;
                return finish(ctx, "used");
            }

            constexpr Duration kInstantInteractionDelay = std::chrono::milliseconds(1200);
            if (now - phaseStarted_ < kInstantInteractionDelay)
            {
                return BehaviorStatus::Running;
            }

            gathered_ = true;
            return finish(ctx, "used");
        }

        // Never move or send another interaction while a gathering cast is active.
        if (s.player.isCasting)
        {
            ctx.seekTook(target_->guid);
            return BehaviorStatus::Running;
        }

        if (lootReceivedAt_)
        {
            if (now - *lootReceivedAt_ < config.lootGrace)
            {
                return BehaviorStatus::Running;
            }

            if (harvests_ >= config.maxHarvestsPerNode)
            {
                return finish(ctx, "harvest limit reached");
            }

            // An herb gives its loot once. On 1.60.1 the picked node stays in the
            // object list, and interacting with it again only timed out.
            if (!questItems_.empty() || target_->kind == GameObjectKind::Herb)
            {
                return finish(ctx, "loot verified in bags");
            }

            // A Classic mining vein can remain after a successful harvest.
            ctx.provider->cancelObjectInteraction();
            interacting_ = false;
            lootReceivedAt_.reset();
            settledSince_.reset();
            phaseStarted_ = now;
        }
        else
        {
            if (ctx.provider && ctx.provider->supportsLua() && ctx.provider->luaIdle())
            {
                ctx.provider->executeLua("if GetNumLootItems()>0 then for i=GetNumLootItems(),1,-1 do LootSlot(i) end end");
            }

            if (now - phaseStarted_ < config.interactionTimeout)
            {
                return BehaviorStatus::Running;
            }

            if (failures_ >= profession_.retryCount)
            {
                return finish(ctx, "gathering failed after retries");
            }

            retry(s, ctx);
        }
    }

    if (s.player.isCasting)
    {
        return BehaviorStatus::Running;
    }

    // Getting closer, or at least moving (round an obstacle) is progress; the
    // approach only gives up after approachTimeout without any.
    if (!interacting_ && !reposition_)
    {
        const float toNode = distance2D(s.player.position, target_->position);

        if (toNode < closest_ - 1.0f || distance2D(s.player.position, progressAt_) >= 3.0f)
        {
            closest_ = std::min(closest_, toNode);
            progressAt_ = s.player.position;
            phaseStarted_ = now;
        }
    }

    // Turning round for the mouse to find it ends by itself, once round.
    if (now - phaseStarted_ >= config.approachTimeout && !ctx.turningRound(target_->guid))
    {
        return finish(ctx, inReach(s.player.position, target_->position, config.interactionDistance) ? "could not face the node" : std::format("got no closer to the node for {:.0f} s", toSeconds(config.approachTimeout)));
    }

    if (reposition_)
    {
        // Right by the node: straight there when it lies by an obstacle.
        const auto status = ctx.navigator.moveTo(s, *reposition_, 0.5f, true, !ctx.navigator.canStandAt(*reposition_));

        if (status == NavStatus::Unreachable)
        {
            return finish(ctx, "retry position is unreachable");
        }

        if (status != NavStatus::Arrived)
        {
            return BehaviorStatus::Running;
        }

        reposition_.reset();
    }

    if (!inReach(s.player.position, target_->position, config.interactionDistance))
    {
        settledSince_.reset();

        // A mob to pull by the way still to walk is fought first. Once the
        // harvest has started, it is finished: dropping a node because
        // something wandered near would waste the whole detour.
        if (harvests_ == 0)
        {
            if (const UnitState* mob = mobInTheWay(s, ctx))
            {
                ctx.log.info("GATHER", "{} (level {}) stands in the way to {} - fighting it first", mob->name, mob->level, target_->name);
                ctx.combat.engaged = mob->guid;

                if (!questItems_.empty())
                {
                    ctx.combat.clearing = mob->guid;
                }

                ++ctx.stats.pulls;
                return BehaviorStatus::Running;
            }
        }

        // The winding way there, until the last yards; planned again when an
        // obstacle is learned on it.
        if (ctx.obstacles && ctx.obstacles->revision() != wayRevision_)
        {
            planWay(s, ctx);
        }

        while (wayIndex_ <= wayToNode_ && wayIndex_ < way_.size() && distance2D(way_[wayIndex_], target_->position) <= kDetourEnds)
        {
            ++wayIndex_;
        }

        if (wayIndex_ <= wayToNode_ && wayIndex_ < way_.size() && distance2D(s.player.position, target_->position) > kDetourEnds)
        {
            const auto status = ctx.navigator.moveTo(s, way_[wayIndex_], ctx.config.waypointRadius, false);

            if (status == NavStatus::Arrived || status == NavStatus::Unreachable)
            {
                ++wayIndex_;
            }

            return BehaviorStatus::Running;
        }

        // An obstacle learned on the way (stuck by the node) plans the approach again.
        if (approaches_.empty() || (ctx.obstacles && ctx.obstacles->revision() != approachRevision_))
        {
            planApproach(s, ctx);
        }

        const Vec3 spot = approaches_[approachIndex_];

        if (distance2D(spot, target_->position) < 0.1f)
        {
            if (ctx.navigator.moveTo(s, target_->position, config.interactionDistance * 0.7f) == NavStatus::Unreachable)
            {
                return finish(ctx, "node is unreachable");
            }

            return BehaviorStatus::Running;
        }

        if (!approachReached_)
        {
            const auto status = ctx.navigator.moveTo(s, spot, 1.0f, false);

            if (status == NavStatus::Unreachable)
            {
                if (++approachIndex_ < approaches_.size())
                {
                    return BehaviorStatus::Running;
                }

                return finish(ctx, "no way to anywhere next to the node");
            }

            if (status != NavStatus::Arrived)
            {
                return BehaviorStatus::Running;
            }

            approachReached_ = true;
        }

        ctx.navigator.moveTo(s, target_->position, config.interactionDistance * 0.7f, true, true);
        return BehaviorStatus::Running;
    }

    ctx.navigator.halt();

    if (!ctx.turningRound(target_->guid) && !ctx.movement.face(s.player, target_->position, 0.4f))
    {
        settledSince_.reset();
        return BehaviorStatus::Running;
    }

    if (!settledSince_)
    {
        settledSince_ = now;
        settleWait_ = ctx.randomBetween(config.settleDelayMin, config.settleDelayMax);
    }

    if (now - *settledSince_ < settleWait_)
    {
        return BehaviorStatus::Running;
    }

    // Without object interaction through memory, the node under the mouse is
    // right-clicked (1.60.1), or else the interact key used on the node faced
    // (2.5.6).
    const bool keyOnly = !ctx.provider->supportsObjectInteraction();

    if (keyOnly && ctx.zoomInForClick(failures_))
    {
        return BehaviorStatus::Running;
    }

    const auto seek = keyOnly ? ctx.seekTurningRound(s.player, target_->guid, target_->position) : BotContext::MouseSeek::Unsupported;

    if (seek == BotContext::MouseSeek::Searching)
    {
        return BehaviorStatus::Running;
    }

    if (seek == BotContext::MouseSeek::NotFound)
    {
        ctx.stopMouseSeek();
        return finish(ctx, "the node was never under the mouse");
    }

    bagsBefore_ = s.player.bagItems;
    phaseStarted_ = now;
    interacting_ = true;
    interactedOnce_ = true;

    if (seek == BotContext::MouseSeek::Found)
    {
        ctx.actions.click(MouseButton::Right);
        ctx.stopMouseSeek();
    }
    else if (keyOnly)
    {
        ctx.actions.execute(actions::Interact);
    }

    if (!keyOnly && !ctx.provider->interactObject(target_->guid, target_->position))
    {
        if (failures_ >= profession_.retryCount)
        {
            return finish(ctx, "object interaction failed");
        }

        retry(s, ctx);
    }
    else
    {
        ctx.log.info("GATHER", "gathering {}", target_->name);
    }

    return BehaviorStatus::Running;
}

} // namespace og
