/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-30
 *  File: QuestHandInBehavior.cpp
 *
 *  Desc:
 *  Implementation of QuestHandInBehavior (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/BotContext.h"
#include "og/core/QuestRewards.h"
#include "og/core/behaviors/Behaviors.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace og
{

namespace
{

// How near the bot goes when talking from farther off opened nothing.
constexpr float kCloseIn = 1.5f;
// Walking there, talking and choosing: long enough for the other end of a zone.
const Duration kTimeout{300000};
// A hand-in that went wrong is not tried again for a while.
const Duration kLeaveFor{600000};
// A hand-in nearer than this is done at once, quest mobs about or not.
constexpr float kHandInNearBy = 100.0f;
// So long after the last quest mob in sight (the next spawn, the walk to the
// next of them), the hand-in far off is gone for.
const Duration kQuestMobsGone{90000};
// Between two steps in the quest window, so each click lands on what the last one opened.
const Duration kStep{700};

bool inLog(const GameSnapshot& s, std::uint32_t quest)
{
    return std::find(s.player.questLog.begin(), s.player.questLog.end(), quest) != s.player.questLog.end();
}

// A yellow ! or ? over the NPC: another quest to take or hand in.
bool offersMore(const UnitState& npc)
{
    return npc.questMarker == QuestMarker::Available || npc.questMarker == QuestMarker::Reward;
}

// The greeting window's "Available Quests" heading: the quests taken stand
// above it, the ones offered below. nullopt where it cannot be read.
std::optional<UiFrame> offersHeading(BotContext& ctx, std::string_view list)
{
    if (list != "QuestFrameGreetingPanel")
    {
        return std::nullopt;
    }

    return ctx.provider->uiRegion("QuestGreetingScrollChildFrame", "AvailableQuestsText");
}

// A quest's row in the NPC's list, among the quests taken or among the ones
// offered. By its title alone the wrong one was clicked where both share it:
// with part one of The People's Militia in the log and The Defias Brotherhood
// to take, the bot kept opening the part it already had.
std::optional<UiFrame> questRow(BotContext& ctx, std::string_view list, std::string_view title, bool offered)
{
    const auto heading = offersHeading(ctx, list);
    constexpr float kSlack = 0.002f;

    for (const UiFrame& row : ctx.provider->uiFramesWithText(list, title))
    {
        if (!row.visible)
        {
            continue;
        }

        if (heading)
        {
            // No heading shown: nothing is offered, every row is a quest taken.
            const bool below = heading->visible && row.top >= heading->bottom - kSlack;

            if (below != offered)
            {
                continue;
            }
        }

        return row;
    }

    return std::nullopt;
}

std::string rewardFrame(std::size_t i)
{
    return "QuestInfoRewardsFrameQuestInfoItem" + std::to_string(i + 1);
}

} // namespace

bool QuestHandInBehavior::shutIn(BotContext& ctx, const std::string& npc, const Vec3& at) const
{
    if (ctx.config.usePathfinding || !ctx.profile)
    {
        return false;
    }

    const auto& zones = ctx.profile->noGoZones;
    const bool inside = std::any_of(zones.begin(), zones.end(), [&](const AvoidZone& z) { return distance2D(z.center, at) <= z.radius; });

    if (inside && shutInSaid_.insert(npc).second)
    {
        ctx.log.info(logcat::Profile, "quest mode: leaving {} alone - they stand in a No Go zone (turn Pathfinder on to go in)", npc);
    }

    return inside;
}

void QuestHandInBehavior::lookForQuestMobs(const GameSnapshot& s, BotContext& ctx)
{
    const bool working = std::any_of(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const QuestTarget& t) { return !t.done && !t.mobs.empty() && isLowestLevelQuest(t, ctx.questTargets); });

    // Ranked: the ones that would be pulled, so none out of bounds or blacklisted.
    if (working && !ctx.targets.rank(s).empty())
    {
        questMobsSeenAt_ = ctx.now();
    }

    questMobsHere_ = working && questMobsSeenAt_ && ctx.now() - *questMobsSeenAt_ < kQuestMobsGone;
}

const QuestTarget* QuestHandInBehavior::pick(const GameSnapshot& s, BotContext& ctx) const
{
    const QuestTarget* best = nullptr;

    // A finished quest handed in to someone in this zone goes first, whatever
    // the level being worked on and the quest mobs about: Kobold Camp Cleanup
    // and Investigate Echo Ridge waited on the next quests, with Marshal
    // McBride a hundred yards off.
    const auto first = [](const QuestTarget& t) { return t.done && t.handInInZone; };

    for (const auto& t : ctx.questTargets)
    {
        if (!first(t) && !isLowestLevelQuest(t, ctx.questTargets))
        {
            continue;
        }

        if (!t.done || !t.handInAt || t.handInEntry == 0 || !inLog(s, t.questId))
        {
            continue;
        }

        if (shutIn(ctx, t.handInTo, *t.handInAt))
        {
            continue;
        }

        if (const auto left = ctx.handInHeldBack.find(t.questId); left != ctx.handInHeldBack.end() && ctx.now() < left->second)
        {
            continue;
        }

        if (questMobsHere_ && !first(t) && distance2D(*t.handInAt, s.player.position) > kHandInNearBy)
        {
            if (waitSaid_.insert(t.questId).second)
            {
                ctx.log.info(logcat::Profile, "quest mode: {} is handed in to {} ({:.0f} yd) once the quest mobs here are done with", t.questName, t.handInTo, distance2D(*t.handInAt, s.player.position));
            }

            continue;
        }

        if (!best || first(t) > first(*best) || (first(t) == first(*best) && distance2D(*t.handInAt, s.player.position) < distance2D(*best->handInAt, s.player.position)))
        {
            best = &t;
        }
    }

    return best;
}

const UnitState* QuestHandInBehavior::questGiver(const GameSnapshot& s, BotContext& ctx) const
{
    constexpr float kInSight = 150.0f;
    constexpr std::size_t kLogFull = 20;

    if (!ctx.questSource || s.player.questLog.size() >= kLogFull)
    {
        return nullptr;
    }

    const UnitState* best = nullptr;

    for (const auto& u : s.units)
    {
        if (u.isPlayer || !u.isAlive() || u.questMarker != QuestMarker::Available || u.entry == 0 || u.name.empty())
        {
            continue; // wait for a fully loaded NPC before starting a visit
        }

        if (distance(s.player.position, u.position) > kInSight)
        {
            continue;
        }

        if (best && distance(s.player.position, u.position) >= distance(s.player.position, best->position))
        {
            continue;
        }

        // Only where Questie names the quests: they are picked by their titles.
        const auto offered = ctx.questSource->offeredBy(s, u.entry);

        if (offered.empty())
        {
            continue;
        }

        // Visited lately: again only for a quest it did not offer then.
        if (const auto left = leaveNpcUntil_.find(u.entry); left != leaveNpcUntil_.end() && ctx.now() < left->second.until && std::all_of(offered.begin(), offered.end(), [&](const std::string& quest) { return std::find(left->second.offered.begin(), left->second.offered.end(), quest) != left->second.offered.end(); }))
        {
            continue;
        }

        if (shutIn(ctx, u.name, u.position))
        {
            continue;
        }

        best = &u;
    }

    return best;
}

bool QuestHandInBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    // A party follower goes where its leader goes, and nowhere on its own.
    if (ctx.partyFollower())
    {
        return false;
    }

    if (quest_ && (!ctx.config.quest.enabled || ctx.ignoredQuests.contains(quest_->questId)))
    {
        // Questing switched off halfway: drop the hand-in, and pull again.
        quest_.reset();
        ctx.objectives.pop("QuestHandIn");
        ctx.combat.defendOnly = false;
    }

    if (!ctx.config.quest.enabled || s.isDead() || s.inCombat() || !s.attackers().empty())
    {
        return false;
    }

    if (!quest_)
    {
        lookForQuestMobs(s, ctx);
    }

    return quest_.has_value() || pick(s, ctx) != nullptr || questGiver(s, ctx) != nullptr;
}

void QuestHandInBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    if (quest_)
    {
        ctx.combat.defendOnly = true;
        ctx.combat.engaged = 0;

        // Back from a fight: the time away was not spent on the hand-in.
        if (leftAt_ != TimePoint{})
        {
            const Duration away = ctx.now() - leftAt_;
            started_ += away;
            stageStarted_ += away;
        }

        // Talking resumes from the start: the window closed in the fight. A
        // quest handed in stays handed in: on with taking the new ones.
        if (stage_ != Stage::Travelling)
        {
            stage_ = afterOpening_ == Stage::Accepting ? Stage::Accepting : Stage::Travelling;
        }

        return;
    }

    if (const QuestTarget* target = pick(s, ctx))
    {
        quest_ = *target;
        pickingUp_ = false;
        waitSaid_.erase(target->questId);
    }
    else if (const UnitState* giver = questGiver(s, ctx))
    {
        const QuestTarget* handIn = nullptr;

        for (const auto& t : ctx.questTargets)
        {
            // Not one a hand-in just failed for: the NPC's yellow ! (a quest to
            // take) brought the bot back to Legging It, "not ready to hand in",
            // every four seconds.
            const auto left = ctx.handInHeldBack.find(t.questId);
            const bool heldBack = left != ctx.handInHeldBack.end() && ctx.now() < left->second;

            if (t.done && !heldBack && (t.handInInZone || isLowestLevelQuest(t, ctx.questTargets)) && inLog(s, t.questId) && (t.handInEntry == giver->entry || (!t.handInTo.empty() && t.handInTo == giver->name)))
            {
                handIn = &t;
                break;
            }
        }

        if (handIn)
        {
            quest_ = *handIn;
            pickingUp_ = false;
        }
        else
        {
            // Nothing to hand in: a visit for the quests it offers.
            QuestTarget visit;
            visit.handInTo = giver->name;
            visit.handInEntry = giver->entry;
            visit.handInAt = giver->position;
            quest_ = std::move(visit);
            pickingUp_ = true;
            leaveNpcUntil_[giver->entry] = Visited{ctx.now() + kLeaveFor, ctx.questSource->offeredBy(s, giver->entry)};
        }
    }
    else
    {
        return;
    }

    ctx.combat.defendOnly = true;
    ctx.combat.engaged = 0;
    stage_ = Stage::Travelling;
    afterOpening_ = pickingUp_ ? Stage::Accepting : Stage::Dialog;
    accepted_ = 0;
    refreshOffers_ = false;
    handedIn_ = false;
    started_ = stageStarted_ = ctx.now();
    progressFrom_.reset();
    leftAt_ = TimePoint{};
    npc_ = 0;
    opens_ = 0;
    mouseTried_ = false;
    rightClicks_ = 0;
    nothingToClickSaid_ = false;
    otherOfferSaid_ = false;
    declinedIgnored_ = false;
    closeIn_ = false;
    rewards_.clear();
    rewardsTried_.clear();
    rewardClicked_ = false;
    completed_ = false;
    progressPanelClicks_ = 0;
    completeClicks_ = 0;
    ctx.objectives.push(Objective{"QuestHandIn", 0, *quest_->handInAt});
    totalTripDistance_ = quest_ && quest_->handInAt ? distance2D(s.player.position, *quest_->handInAt) : 0.0f;

    if (pickingUp_)
    {
        ctx.log.info(logcat::Profile, "quest mode: {} has quests for us ({:.0f} yd) - going to take them", quest_->handInTo, distance2D(s.player.position, *quest_->handInAt));
    }
    else
    {
        ctx.log.info(logcat::Profile, "quest mode: handing in {} to {} ({:.0f} yd)", quest_->questName, quest_->handInTo, distance2D(s.player.position, *quest_->handInAt));
    }
}

void QuestHandInBehavior::onExit(BotContext& ctx)
{
    // The window is closed below: its rewards are looked at afresh next time,
    // the mouse put on each again - not waited on where it was left.
    rewards_.clear();
    hovering_ = 0;
    hoverAt_.reset();
    progressPanelClicks_ = 0;
    completeClicks_ = 0;
    leftAt_ = ctx.now();
    ctx.questTravel = false;
    ctx.navigator.stop();
    ctx.cancelInteraction();
    ctx.stopMouseSeek();
    ctx.restoreCamera();
}

BehaviorStatus QuestHandInBehavior::leave(BotContext& ctx, const std::string& reason, bool failed)
{
    ctx.navigator.stop();
    ctx.movement.stop();
    ctx.stopMouseSeek();

    if (failed)
    {
        for (const auto* close : {"QuestFrameCloseButton", "GossipFrameCloseButton"})
        {
            if (ctx.uiFrameVisible(close))
            {
                ctx.clickUiFrame(close);
            }
        }
    }

    // The way on from here is the navigator's, out of a building too.
    return finish(ctx, reason, failed);
}

BehaviorStatus QuestHandInBehavior::finish(BotContext& ctx, const std::string& reason, bool failed)
{
    ctx.navigator.stop();
    ctx.movement.stop();
    ctx.stopMouseSeek();
    ctx.restoreCamera();
    ctx.objectives.pop("QuestHandIn");
    ctx.combat.defendOnly = false;
    ctx.questTravel = false;
    totalTripDistance_ = 0.0f;

    if (quest_ && pickingUp_)
    {
        if (failed)
        {
            ctx.log.warn(logcat::Profile, "quest mode: could not take {}'s quests: {}", quest_->handInTo, reason);
        }
    }
    else if (quest_)
    {
        if (failed)
        {
            ctx.handInHeldBack[quest_->questId] = ctx.now() + kLeaveFor;

            // Whatever window is left open would stand in the way of the grind.
            for (const auto* close : {"QuestFrameCloseButton", "GossipFrameCloseButton"})
            {
                if (ctx.uiFrameVisible(close))
                {
                    ctx.clickUiFrame(close);
                }
            }

            ctx.log.warn(logcat::Profile, "quest mode: could not hand in {}: {} - trying again in {:.0f} minutes", quest_->questName, reason, toSeconds(kLeaveFor) / 60.0);
        }
    }

    quest_.reset();
    return failed ? BehaviorStatus::Failure : BehaviorStatus::Success;
}

bool QuestHandInBehavior::clickAt(BotContext& ctx, const UiFrame& frame)
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

BehaviorStatus QuestHandInBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    // One visit over and the next NPC waiting: the engine keeps this behavior
    // running without entering it again, so the next visit starts here.
    if (!quest_)
    {
        onEnter(s, ctx);
    }

    if (!quest_)
    {
        return BehaviorStatus::Success;
    }

    ctx.questTravel = stage_ == Stage::Travelling;
    const TimePoint now = ctx.now();
    // The NPC, once it is in sight.
    const UnitState* npc = nullptr;

    for (const auto& u : s.units)
    {
        if (!u.isPlayer && u.isAlive() && u.entry == quest_->handInEntry && (!npc || distance(s.player.position, u.position) < distance(s.player.position, npc->position)))
        {
            npc = &u;
        }
    }

    // Getting on: 200 yards from where the last stretch began, the time
    // starts again. Five minutes ran out on the way from Westfall to
    // Lakeshire, walked all along; stuck or circling, it still runs out.
    constexpr float kProgressStretch = 200.0f;

    if (!progressFrom_)
    {
        progressFrom_ = s.player.position;
    }
    else if (distance2D(s.player.position, *progressFrom_) >= kProgressStretch)
    {
        progressFrom_ = s.player.position;
        started_ = now;
    }

    if (now - started_ > kTimeout)
    {
        // Not left standing about, jumping now and then: the whole bot waits
        // for the player.
        ctx.pauseRequest = std::format("could not hand in {} to {} in time", quest_->questName, quest_->handInTo.empty() ? std::string("its NPC") : quest_->handInTo);
        return leave(ctx, "it took too long", true);
    }

    if (stage_ == Stage::Accepting)
    {
        return accept(s, ctx, npc);
    }

    if (!pickingUp_ && !handedIn_ && stage_ != Stage::Opening && !inLog(s, quest_->questId))
    {
        handedIn_ = true;
        ctx.log.info(logcat::Profile, "quest mode: handed in {} to {}", quest_->questName, quest_->handInTo);
        stage_ = Stage::Accepting;
        afterOpening_ = Stage::Accepting;
        stageStarted_ = now;
        nextStep_ = now + kStep;
        opens_ = 0;
        refreshOffers_ = true;
        return BehaviorStatus::Running;
    }

    switch (stage_)
    {
    case Stage::Travelling:
    {
        if (npc)
        {
            const float reach = closeIn_ ? kCloseIn : ctx.humanReach(ctx.config.interactDistance, npc->guid);

            if (distance(s.player.position, npc->position) <= reach)
            {
                ctx.navigator.stop();
                ctx.movement.stop();
                npc_ = npc->guid;
                mouseTried_ = false;
                stage_ = Stage::Opening;
                stageStarted_ = now;
                return BehaviorStatus::Running;
            }

            const NavStatus status = ctx.navigator.moveTo(s, npc->position, reach - 0.5f, true);

            if (status == NavStatus::Unreachable)
            {
                return leave(ctx, "the NPC could not be reached", true);
            }

            // Gone as near as the way goes: talk from there.
            if (closeIn_ && status == NavStatus::Arrived && distance(s.player.position, npc->position) <= ctx.config.interactDistance)
            {
                ctx.navigator.stop();
                ctx.movement.stop();
                npc_ = npc->guid;
                mouseTried_ = false;
                stage_ = Stage::Opening;
                stageStarted_ = now;
                return BehaviorStatus::Running;
            }

            // At the end of the way and still out of reach above or below:
            // the map had no way up to them (or down). Standing there, the bot
            // turned on the spot under the NPC until the timeout.
            if (status == NavStatus::Arrived && std::abs(npc->position.z - s.player.position.z) > 3.0f)
            {
                return leave(ctx, quest_->handInTo + " stands on a floor no way was found to", true);
            }

            return BehaviorStatus::Running;
        }

        const NavStatus status = ctx.navigator.moveTo(s, *quest_->handInAt, 3.0f, true);

        if (status == NavStatus::Unreachable)
        {
            return leave(ctx, "where it stands could not be reached", true);
        }

        // There, and nobody about: it walks about, or stands somewhere else now.
        if (status == NavStatus::Arrived && now - stageStarted_ > Duration(15000) && distance2D(s.player.position, *quest_->handInAt) < 5.0f)
        {
            return leave(ctx, quest_->handInTo + " is not where Questie has them", true);
        }

        return BehaviorStatus::Running;
    }

    case Stage::Opening:
    {
        if (npc)
        {
            const float reach = closeIn_ ? kCloseIn : ctx.humanReach(ctx.config.interactDistance, npc->guid);

            if (distance(s.player.position, npc->position) > reach + 0.5f)
            {
                ctx.log.info(logcat::Profile, "quest mode: moved out of reach of {} - replanning the way there", quest_->handInTo);
                stage_ = Stage::Travelling;
                stageStarted_ = now;
                return BehaviorStatus::Running;
            }
        }

        if (!npc || npc->guid != npc_)
        {
            if (afterOpening_ == Stage::Accepting)
            {
                return leave(ctx, pickingUp_ ? "visited" : "handed in", false);
            }

            stage_ = Stage::Travelling;
            return BehaviorStatus::Running;
        }

        // Talked once from where it stands and nothing opened: right up to
        // the NPC first, then again.
        if (opens_ >= 1 && !closeIn_ && distance(s.player.position, npc->position) > kCloseIn + 0.5f)
        {
            closeIn_ = true;
            ctx.log.info(logcat::Profile, "quest mode: {} did not answer from {:.1f} yd - going right up to them", quest_->handInTo, distance(s.player.position, npc->position));
            stage_ = Stage::Travelling;
            stageStarted_ = now;
            return BehaviorStatus::Running;
        }

        if (++opens_ > 4)
        {
            if (afterOpening_ == Stage::Accepting && !pickingUp_)
            {
                return leave(ctx, "handed in", false);
            }

            return leave(ctx, "talking to " + quest_->handInTo + " opened nothing", true);
        }

        if (!ctx.turningRound(npc_) && !ctx.movement.face(s.player, npc->position, 0.4f))
        {
            --opens_;
            return BehaviorStatus::Running;
        }

        if (!ctx.provider->supportsLua())
        {
            // After a hand-in the NPC is usually still selected. The interact
            // key avoids trusting a stale mouseover GUID from the closed window.
            // Alternate with a mouse search if that key does not open a dialog.
            const bool useTarget = s.targetGuid == npc_ && opens_ % 2 == 1 && ctx.actions.hasBinding(actions::Interact);
            // No Lua (1.60.1): right-click the NPC under the mouse, as the vendor is.
            if (!useTarget && !mouseTried_ && ctx.zoomInForClick(rightClicks_))
            {
                --opens_;
                return BehaviorStatus::Running;
            }

            const UnitState* npcUnit = s.findUnit(npc_);
            const auto seek = useTarget || mouseTried_ ? BotContext::MouseSeek::Unsupported : (npcUnit ? ctx.seekNpcTurning(s.player, npc_, npcUnit->position) : ctx.seekUnderMouse(npc_));

            if (seek == BotContext::MouseSeek::Searching)
            {
                --opens_;
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
                    --opens_;
                    return BehaviorStatus::Running;
                }
            }
        }
        else
        {
            ctx.interactNpc(npc_, npc->position);
        }

        ctx.log.info(logcat::Profile, "quest mode: talking to {}", quest_->handInTo);
        refreshOffers_ = false;
        stage_ = afterOpening_;
        stageStarted_ = now;
        nextStep_ = now + kStep;
        return BehaviorStatus::Running;
    }

    case Stage::Dialog:
        return dialog(s, ctx);
    case Stage::Accepting:
        break;
    }

    return BehaviorStatus::Running;
}

BehaviorStatus QuestHandInBehavior::dialog(const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();

    if (now < nextStep_)
    {
        return BehaviorStatus::Running;
    }

    nextStep_ = now + kStep;

    if (ctx.uiFrameVisible("QuestFrameRewardPanel"))
    {
        progressPanelClicks_ = 0;
        return chooseReward(s, ctx);
    }

    if (ctx.uiFrameVisible("QuestFrameProgressPanel"))
    {
        // "Continue": the items are handed over on the next page.
        // If the quest requirements are not met, the button is disabled by the client.
        if (++progressPanelClicks_ > 3)
        {
            progressPanelClicks_ = 0;
            return leave(ctx, "the quest is not ready to hand in (incomplete)", true);
        }

        ctx.clickUiFrame("QuestFrameCompleteButton");
        return BehaviorStatus::Running;
    }

    progressPanelClicks_ = 0;

    if (ctx.uiFrameVisible("QuestFrameDetailPanel"))
    {
        // Just completed: the next quest of a chain opens by itself: accept it!
        // Or if an offered quest is shown, accept it as long as there is room in the log.
        constexpr std::size_t kLogFull = 20;
        constexpr int kMostToTake = 10;

        if (s.player.questLog.size() < kLogFull && accepted_ < kMostToTake)
        {
            const auto title = ctx.uiText("QuestDetailScrollChildFrame", "QuestInfoTitleHeader").value_or("a quest");

            // One turned off in the Questing panel (and abandoned since) is not taken again.
            if (ctx.ignoredQuestNames.contains(title))
            {
                if (ctx.clickUiFrame("QuestFrameDeclineButton") && !std::exchange(declinedIgnored_, true))
                {
                    ctx.log.info(logcat::Profile, "quest mode: {} offers {}, which is turned off - not taking it", quest_->handInTo, title);
                }

                return BehaviorStatus::Running;
            }

            if (ctx.clickUiFrame("QuestFrameAcceptButton"))
            {
                ++accepted_;
                refreshOffers_ = true;
                opens_ = 0;
                stageStarted_ = now;
                ctx.log.info(logcat::Profile, "quest mode: took {} from {}", title, quest_->handInTo);
            }
        }
        else
        {
            ctx.clickUiFrame("QuestFrameDeclineButton");
        }

        return BehaviorStatus::Running;
    }

    // A list of the NPC's quests (and gossip):
    for (const auto* list : {"QuestFrameGreetingPanel", "GossipFrame"})
    {
        if (!ctx.uiFrameVisible(list))
        {
            continue;
        }

        // 1. Check if our targeted hand-in quest is in the list:
        if (quest_ && !quest_->questName.empty())
        {
            if (const auto title = questRow(ctx, list, quest_->questName, false))
            {
                if (clickAt(ctx, *title))
                {
                    ctx.log.info(logcat::Profile, "quest mode: picked {} in the list", quest_->questName);
                    stageStarted_ = now;
                }

                return BehaviorStatus::Running;
            }
        }

        // 2. Check if any OTHER completed quest for this NPC is in the list:
        bool pickedHandIn = false;

        for (const auto& t : ctx.questTargets)
        {
            if (t.done && (t.handInInZone || isLowestLevelQuest(t, ctx.questTargets)) && inLog(s, t.questId) && (t.handInEntry == quest_->handInEntry || (!t.handInTo.empty() && t.handInTo == quest_->handInTo)))
            {
                if (const auto left = ctx.handInHeldBack.find(t.questId); left != ctx.handInHeldBack.end() && ctx.now() < left->second)
                {
                    continue;
                }

                if (const auto title = questRow(ctx, list, t.questName, false))
                {
                    if (clickAt(ctx, *title))
                    {
                        ctx.log.info(logcat::Profile, "quest mode: picked {} in the list", t.questName);
                        quest_ = t;
                        handedIn_ = false;
                        afterOpening_ = Stage::Dialog;
                        stageStarted_ = now;
                        pickedHandIn = true;
                        return BehaviorStatus::Running;
                    }
                }
            }
        }

        if (pickedHandIn)
        {
            return BehaviorStatus::Running;
        }

        // 3. No completed quests found in greeting panel: transition to accepting offered quests!
        stage_ = Stage::Accepting;
        stageStarted_ = now;
        return BehaviorStatus::Running;
    }

    // Nothing open: talk again, a few times.
    if (now - stageStarted_ > Duration(3000))
    {
        stage_ = Stage::Opening;
        mouseTried_ = false;
    }

    return BehaviorStatus::Running;
}

BehaviorStatus QuestHandInBehavior::accept(const GameSnapshot& s, BotContext& ctx, const UnitState* npc)
{
    const TimePoint now = ctx.now();

    if (now < nextStep_)
    {
        return BehaviorStatus::Running;
    }

    nextStep_ = now + kStep;
    constexpr std::size_t kLogFull = 20;
    constexpr int kMostToTake = 10;
    const auto done = [&](const std::string& why)
    {
        for (const auto* close : {"QuestFrameCloseButton", "GossipFrameCloseButton"})
        {
            if (ctx.uiFrameVisible(close))
            {
                ctx.clickUiFrame(close);
            }
        }

        if (!why.empty())
        {
            ctx.log.info(logcat::Profile, "quest mode: {}", why);
        }

        return leave(ctx, pickingUp_ ? "visited" : "handed in", false);
    };

    // If RewardPanel or ProgressPanel opens while in accepting, switch to Dialog to handle it!
    if (ctx.uiFrameVisible("QuestFrameRewardPanel") || ctx.uiFrameVisible("QuestFrameProgressPanel"))
    {
        // The quest log can update before the old reward panel disappears.
        // Do not re-enter its hand-in or count the same completion again.
        if (handedIn_ || pickingUp_)
        {
            if (now - stageStarted_ >= Duration(1500))
            {
                ctx.clickUiFrame("QuestFrameCloseButton");
            }

            return BehaviorStatus::Running;
        }

        stage_ = Stage::Dialog;
        return dialog(s, ctx);
    }

    // A quest shown to take - the next of a chain opens by itself: Accept.
    if (ctx.uiFrameVisible("QuestFrameDetailPanel"))
    {
        if (s.player.questLog.size() >= kLogFull)
        {
            return done("the quest log is full");
        }

        if (accepted_ >= kMostToTake)
        {
            return done("");
        }

        const auto title = ctx.uiText("QuestDetailScrollChildFrame", "QuestInfoTitleHeader").value_or("a quest");

        if (ctx.ignoredQuestNames.contains(title))
        {
            if (ctx.clickUiFrame("QuestFrameDeclineButton") && !std::exchange(declinedIgnored_, true))
            {
                ctx.log.info(logcat::Profile, "quest mode: {} offers {}, which is turned off - not taking it", quest_->handInTo, title);
            }

            return BehaviorStatus::Running;
        }

        if (ctx.clickUiFrame("QuestFrameAcceptButton"))
        {
            ++accepted_;
            refreshOffers_ = true;
            opens_ = 0;
            stageStarted_ = now;
            ctx.log.info(logcat::Profile, "quest mode: took {} from {}", title, quest_->handInTo);
        }

        return BehaviorStatus::Running;
    }

    // The NPC's list:
    for (const auto* list : {"QuestFrameGreetingPanel", "GossipFrame"})
    {
        if (!ctx.uiFrameVisible(list))
        {
            continue;
        }

        // 1. Check if ANY completed quests for this NPC are present: hand them in first!
        for (const auto& t : ctx.questTargets)
        {
            if (t.done && (t.handInInZone || isLowestLevelQuest(t, ctx.questTargets)) && inLog(s, t.questId) && (t.handInEntry == quest_->handInEntry || (!t.handInTo.empty() && t.handInTo == quest_->handInTo)))
            {
                if (const auto left = ctx.handInHeldBack.find(t.questId); left != ctx.handInHeldBack.end() && ctx.now() < left->second)
                {
                    continue;
                }

                if (const auto title = questRow(ctx, list, t.questName, false))
                {
                    if (clickAt(ctx, *title))
                    {
                        ctx.log.info(logcat::Profile, "quest mode: picked {} to hand in", t.questName);
                        quest_ = t;
                        pickingUp_ = false;
                        handedIn_ = false;
                        afterOpening_ = Stage::Dialog;
                        stage_ = Stage::Dialog;
                        stageStarted_ = now;
                        return BehaviorStatus::Running;
                    }
                }
            }
        }

        // 2. Check offered quests:
        if (s.player.questLog.size() < kLogFull && accepted_ < kMostToTake)
        {
            // The client can offer server-specific quests missing from Questie.
            // In the greeting window, active quests are above AvailableQuestsText;
            // only the QuestTitleButtons below that heading are offers.
            if (std::string_view(list) == "QuestFrameGreetingPanel")
            {
                const auto heading = ctx.provider->uiRegion("QuestGreetingScrollChildFrame", "AvailableQuestsText");
                const auto panel = ctx.provider->uiFrame(list);

                // Rows are clicked here without their titles being known: not
                // after one of them was a quest turned off, or it is clicked again.
                if (heading && heading->visible && panel && !declinedIgnored_)
                {
                    for (int i = 1; i <= 25; ++i)
                    {
                        const auto button = ctx.provider->uiFrame("QuestTitleButton" + std::to_string(i));

                        if (!button || !button->visible || button->top < heading->bottom || button->top < panel->top || button->bottom > panel->bottom || button->left < panel->left || button->right > panel->right)
                        {
                            continue;
                        }

                        if (clickAt(ctx, *button))
                        {
                            stageStarted_ = now;
                        }

                        return BehaviorStatus::Running;
                    }

                    // Where the rows have no names of their own (1.60.1, where
                    // they hang off the panel itself): the topmost frame
                    // showing a text below the heading, inside the scrolling
                    // part - the Goodbye button stands under that.
                    std::optional<UiFrame> first;
                    const auto scroll = ctx.provider->uiFrame("QuestGreetingScrollFrame");
                    const UiFrame& within = scroll && scroll->visible ? *scroll : *panel;

                    for (const UiFrame& row : ctx.provider->uiFramesWithText(list, ""))
                    {
                        // The rows reach the list's edges to the pixel: a
                        // little slack, less than the Goodbye button's height.
                        constexpr float kEdge = 0.004f;

                        if (!row.visible || row.top < heading->bottom - 0.002f || row.top < within.top - kEdge || row.bottom > within.bottom + kEdge || row.left < within.left - kEdge || row.right > within.right + kEdge)
                        {
                            continue;
                        }

                        if (!first || row.top < first->top)
                        {
                            first = row;
                        }
                    }

                    if (first)
                    {
                        if (clickAt(ctx, *first))
                        {
                            stageStarted_ = now;
                        }

                        return BehaviorStatus::Running;
                    }
                }
            }

            const auto offered = ctx.questSource ? ctx.questSource->offeredBy(s, quest_->handInEntry) : std::vector<std::string>{};

            for (const auto& name : offered)
            {
                if (ctx.ignoredQuestNames.contains(name))
                {
                    continue;
                }

                if (const auto title = questRow(ctx, list, name, true))
                {
                    if (clickAt(ctx, *title))
                    {
                        stageStarted_ = now;
                    }

                    return BehaviorStatus::Running;
                }
            }

            // One the data has for other races or classes, offered all the
            // same: the window says so (A Scribbled Letter, a warrior's in the
            // data, to a human hunter). Marshal McBride was walked back to
            // again and again for it, its yellow ! never going.
            for (const auto& name : ctx.questSource ? ctx.questSource->mayOffer(s, quest_->handInEntry) : std::vector<std::string>{})
            {
                if (std::find(offered.begin(), offered.end(), name) != offered.end() || ctx.ignoredQuestNames.contains(name))
                {
                    continue;
                }

                if (const auto title = questRow(ctx, list, name, true))
                {
                    if (!std::exchange(otherOfferSaid_, true))
                    {
                        ctx.log.info(logcat::Profile, "quest mode: {} offers {}, for another race or class by the data - taking it", quest_->handInTo, name);
                    }

                    if (clickAt(ctx, *title))
                    {
                        stageStarted_ = now;
                    }

                    return BehaviorStatus::Running;
                }
            }

            // Left without a click: said once a visit, with what the data
            // offers and how many lines of text the window shows, to tell a
            // quest the data lacks from a row that could not be read.
            if (pickingUp_ && !nothingToClickSaid_ && now - stageStarted_ >= Duration(1500))
            {
                nothingToClickSaid_ = true;
                std::string names;

                for (const auto& name : offered)
                {
                    names += (names.empty() ? "" : ", ") + name;
                }

                ctx.log.info(logcat::Profile, "quest mode: no quest of {}'s to click in its {} - the data offers [{}], the window shows {} line(s) of text", quest_->handInTo, list, names, ctx.provider->uiFramesWithText(list, "").size());
            }
        }

        // A greeting can still be rebuilding just after accepting a quest.
        if (now - stageStarted_ < Duration(1500))
        {
            return BehaviorStatus::Running;
        }

        if (refreshOffers_ && npc && opens_ < 4 && offersMore(*npc) && distance(s.player.position, npc->position) <= ctx.config.interactDistance + 1.0f)
        {
            npc_ = npc->guid;
            mouseTried_ = false;
            rightClicks_ = 0;
            afterOpening_ = Stage::Accepting;
            stage_ = Stage::Opening;
            return BehaviorStatus::Running;
        }

        return done("");
    }

    // Nothing open. Give the marker a moment to change after the hand-in, then
    // talk again while it still shows a yellow ! or ?.
    if (now - stageStarted_ < Duration(1500))
    {
        return BehaviorStatus::Running;
    }

    // Only while it still has something for us: talking again after every
    // quest taken opened the window once more for nothing.
    if (!npc || !offersMore(*npc))
    {
        return done("");
    }

    if (distance(s.player.position, npc->position) > ctx.config.interactDistance + 1.0f || opens_ >= 4)
    {
        return done("");
    }

    npc_ = npc->guid;
    mouseTried_ = false;
    rightClicks_ = 0;
    afterOpening_ = Stage::Accepting;
    stage_ = Stage::Opening;
    return BehaviorStatus::Running;
}

BehaviorStatus QuestHandInBehavior::chooseReward(const GameSnapshot& s, BotContext& ctx)
{
    const TimePoint now = ctx.now();
    // The rewards shown: the choices and what comes anyway, in one list.
    std::vector<UiFrame> shown;

    for (std::size_t i = 0; i < 10; ++i)
    {
        const auto frame = ctx.provider->uiFrame(rewardFrame(i));

        if (!frame || !frame->visible)
        {
            break;
        }

        shown.push_back(*frame);
    }

    if (rewardClicked_ || shown.empty())
    {
        if (++completeClicks_ > 5)
        {
            completeClicks_ = 0;
            return leave(ctx, "could not complete quest (inventory full?)", true);
        }

        // "Complete Quest" - after the reward, or straight away with none to choose.
        if (ctx.clickUiFrame("QuestFrameCompleteQuestButton"))
        {
            // Still here next step: that reward was not one to choose; try the next.
            rewardClicked_ = false;
            completed_ = true;
            stageStarted_ = now;
        }

        return BehaviorStatus::Running;
    }

    completeClicks_ = 0;

    if (rewards_.size() != shown.size())
    {
        rewards_.assign(shown.size(), std::nullopt);
        hovering_ = 0;
        hoverAt_.reset();
        readOwners_.clear();
    }

    // Look at each: hover it and read its tooltip - once it is that reward's.
    // Read the moment the mouse was put on it, the tooltip was still the one
    // before: a warrior's Mail reward was taken for the Leather one beside
    // it, and the Leather one chosen (Break Sharptusk!).
    constexpr Duration kTooltipWait{1500};

    if (hovering_ < shown.size())
    {
        if (hoverAt_ == hovering_)
        {
            const auto tooltip = ctx.provider->uiItemTooltip();
            // The hovered reward's own: not the one up when the mouse moved, nor
            // one already read - a late one for the reward before, given up on,
            // passed for this one - and named as the reward is, where its name
            // can be read.
            const std::string frame = rewardFrame(hovering_);
            const auto named = ctx.uiText(frame, frame + "Name");
            const bool fresh = ctx.actions.mouseFree() && tooltip && tooltip->owner != 0 && tooltip->owner != ownerAtMove_ && std::find(readOwners_.begin(), readOwners_.end(), tooltip->owner) == readOwners_.end() && !tooltip->name.empty() && (!named || named->empty() || *named == tooltip->name);

            if (!fresh && now - hoverSince_ < kTooltipWait)
            {
                return BehaviorStatus::Running;
            }

            if (fresh)
            {
                rewards_[hovering_] = tooltip;
                readOwners_.push_back(tooltip->owner);
            }

            ++hovering_;
            hoverAt_.reset();
            return BehaviorStatus::Running;
        }

        const auto size = ctx.actions.windowSize();

        if (!size)
        {
            return leave(ctx, "the window's size is not known", true);
        }

        ctx.actions.mouseMove(static_cast<int>(shown[hovering_].centerX() * static_cast<float>(size->first) + 0.5f), static_cast<int>(shown[hovering_].centerY() * static_cast<float>(size->second) + 0.5f));
        hoverAt_ = hovering_;
        hoverSince_ = now;
        const auto before = ctx.provider->uiItemTooltip();
        ownerAtMove_ = before ? before->owner : 0;
        return BehaviorStatus::Running;
    }

    // What was read of each, once: why one was chosen is in the log.
    if (rewardsTried_.empty())
    {
        std::string read;

        for (std::size_t i = 0; i < rewards_.size(); ++i)
        {
            const auto& r = rewards_[i];
            read += std::format("{}{}", i ? ", " : "", r ? std::format("{} ({}{}c{})", r->name, [&]
            {
                for (const auto& line : r->lines)
                {
                    if (line == "Cloth" || line == "Leather" || line == "Mail" || line == "Plate")
                    {
                        return line + ", ";
                    }
                }

                return std::string();
            }(), r->sellPrice, r->hasRedLine() ? ", not usable" : "") : std::string("not read"));
        }

        ctx.log.info(logcat::Profile, "quest mode: rewards: {}", read);
    }

    auto candidates = rewards_;

    for (const auto tried : rewardsTried_)
    {
        if (tried < candidates.size())
        {
            candidates[tried].reset();
        }
    }

    auto choice = chooseQuestReward(candidates, ctx.config.quest.reward, s.player.playerClass, s.player.level);

    if (!choice)
    {
        // No tooltip to go by: the first not tried yet.
        for (std::size_t i = 0; i < shown.size() && !choice; ++i)
        {
            if (std::find(rewardsTried_.begin(), rewardsTried_.end(), i) == rewardsTried_.end())
            {
                choice = i;
            }
        }

        if (!choice)
        {
            return leave(ctx, "none of the rewards could be chosen", true);
        }
    }

    rewardsTried_.push_back(*choice);

    if (!clickAt(ctx, shown[*choice]))
    {
        return BehaviorStatus::Running;
    }

    const auto& item = rewards_[*choice];
    ctx.log.info(logcat::Profile, "quest mode: choosing {}{}", item ? item->name : rewardFrame(*choice), item && item->sellPrice >= 0 ? std::format(" (sells for {}c)", item->sellPrice) : "");
    rewardClicked_ = true;
    return BehaviorStatus::Running;
}

} // namespace og
