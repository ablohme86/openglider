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
 *  File: Behaviors.h
 *
 *  Desc:
 *  Declarations and interface for Behaviors (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/core/Humanizer.h"
#include "og/core/IBotBehavior.h"
#include "og/core/TaxiTrip.h"
#include "og/state/IGameStateProvider.h"

#include <memory>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <vector>

namespace og
{

// 1000: health potion / emergency spells from the class' emergency block.
// 1050: swims up when the breath left will only just take the character to
// the surface, and stays up until it breathes again.
class SurfaceBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Surface";
    }

    int priority() const override
    {
        return priority::Surface;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    bool rising_ = false;
    std::optional<float> lastZ_; // the height a tick ago, rising
    int still_ = 0;              // ticks it has risen no further
    std::optional<TimePoint> retryAt_; // let go of the key until then
};

// 1040: a step out of a campfire (or a bonfire, a brazier) the character has
// stood in for a second: it burns for a dozen health every few seconds, and
// neither resting nor a fight would ever leave it. Walking through is let be.
class LeaveFireBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "LeaveFire";
    }

    int priority() const override
    {
        return priority::LeaveFire;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

    // The fire the character stands in, if it stands in one.
    static const GameObjectState* fireUnderfoot(const GameSnapshot& s);

private:
    std::optional<TimePoint> inSince_; // in the fire since
    std::optional<Vec3> fire_;         // the one being left
    std::optional<Vec3> to_;
    TimePoint startedAt_{};
};

// 610, Sit down at campfires (WoW Forever): sat by a campfire for a minute -
// Welcoming Campfire counts it down - the character gets Camp Benefits and a
// stat for an hour - of what players have put up by it (a tent, a Sharpening
// Wheel): a bare campfire is passed by. Without Camp Benefits and such a
// campfire in sight, the bot walks up to it, as near as it does not burn,
// types /sit, and waits for the minute to run out (or Welcoming Campfire to
// start over). A campfire that gives no Welcoming Campfire is left alone for
// half an hour.
class CampfireBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Campfire";
    }

    int priority() const override
    {
        return priority::Campfire;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

    // The nearest campfire in sight not left alone, with something put up by it.
    static const GameObjectState* campfireInSight(const GameSnapshot& s, const BotContext& ctx);
    // A campfire ("Basic Campfire", "Campfire"), by its name.
    static bool isCampfire(const GameObjectState& object);
    // What players have put up by the fire - a tent, a Sharpening Wheel:
    // Camp Benefits comes of those; a bare campfire gives none.
    static std::vector<const GameObjectState*> campItems(const GameSnapshot& s, const GameObjectState& fire);

private:
    enum class Phase
    {
        Walk,
        Sit,
        Wait
    };

    BehaviorStatus done(BotContext& ctx, std::string_view why, bool leaveAlone = false);

    Guid fire_ = 0;
    Phase phase_ = Phase::Walk;
    TimePoint startedAt_{};
    std::optional<TimePoint> sitSince_;  // first /sit typed
    std::optional<TimePoint> lastSit_;   // last /sit typed
    float lastWelcome_ = -1.0f;          // Welcoming Campfire's seconds left a tick ago
};

class EmergencyBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Emergency";
    }

    int priority() const override
    {
        return priority::Emergency;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
};

// 750: stands while a player follows us (FollowerWatch). Combat, above it,
// still fights back anything that attacks.
class HoldBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Hold";
    }

    int priority() const override
    {
        return priority::Hold;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
};

// Bags full enough, or gear broken enough, for a vendor trip (VendorBehavior).
bool vendorNeeded(const GameSnapshot& s, const VendorConfig& config);

// 590, Party system: a follower walks after its leader, near for Damage and
// further back for a healer; out of sight, to where it was last seen.
class FollowBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Follow";
    }

    int priority() const override
    {
        return priority::PartyFollow;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    // Walking: on in to the follow distance; standing: off again only once
    // the leader is this much further - not a step for every step it takes.
    bool walking_ = false;
    bool lostSaid_ = false;
    // Out of sight since; "lost" said in party chat when (once every ten minutes).
    std::optional<TimePoint> outOfSightSince_;
    std::optional<TimePoint> lostAskedAt_;
};

// 950, Party system: a healer heals the party members with the spells of its
// emergency block that can be cast on others (friendly in actions.yaml),
// `player` in its rules meaning the member: the lowest first, the leader
// before the others. /target <member>, the heal, and back to the target it had.
class PartyHealBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "PartyHeal";
    }

    int priority() const override
    {
        return priority::PartyHeal;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    // The member to heal now, chosen by canRun.
    Guid member_ = 0;
    std::string memberName_;
    // When "/target <member>" was typed for it, and for whom.
    std::optional<TimePoint> targetAsked_;
    TimePoint askedSince_{};
    Guid askedFor_ = 0;
    int targetsTaken_ = 0; // members taken as the target since it began
    // The target before the heal, to go back to once it is cast.
    Guid before_ = 0;
    bool healed_ = false;
    // A member not taken as the target in time: left a while.
    std::map<Guid, TimePoint> notTaken_;
};

// 580, Party system: the leader stands while a member is out of sight, dead,
// resting or further than the party range, out of combat. Vendor, quest
// hand-ins, pulls and the route wait; fights, loot and rest go on.
class PartyWaitBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "PartyWait";
    }

    int priority() const override
    {
        return priority::PartyWait;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;

private:
    std::string said_; // the wait last logged: said again when it changes
};

// Learns and replays the way round obstacles on a path walked point by point,
// such as a vendor trip or a corpse run. The grind route has its own corrector.
class PathDetours
{
public:
    // Splices remembered detours into `path`, on legs from `first` on.
    std::size_t apply(std::vector<Vec3>& path, BotContext& ctx, std::size_t first = 0) const;
    // Call every tick with the index of the point being walked to.
    void update(const GameSnapshot& s, BotContext& ctx, std::size_t index, bool recovering);
    // Call on reaching path[index]. Returns how many points went in before it.
    std::size_t reached(std::vector<Vec3>& path, std::size_t index, BotContext& ctx);
    void abandon()
    {
        corrector_.abandon();
    }

private:
    RouteCorrector corrector_;
    std::uint64_t movesAtRecording_ = 0;
};

// 900: release -> ghost run (ghost waypoints) -> safe spot -> resurrect.
class DeathBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Death";
    }

    int priority() const override
    {
        return priority::Death;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

    const std::vector<Vec3>& ghostPath() const
    {
        return ghostPath_;
    }

    bool isCorpseRun() const
    {
        return deadSince_ != TimePoint{} && ghostSince_.has_value();
    }

    float corpseRunTotalDistance() const
    {
        return corpseRunTotalDistance_;
    }

    float remainingCorpseDistance(const Vec3& pos, const std::optional<Vec3>& corpse) const
    {
        if (!ghostPath_.empty() && ghostIndex_ + 1 < ghostPath_.size())
        {
            return remainingPathDistance(pos, ghostPath_, ghostIndex_);
        }
        if (corpse)
        {
            return distance2D(pos, *corpse);
        }
        return 0.0f;
    }

private:
    void buildGhostPath(const Vec3& from, const Vec3& corpse, BotContext& ctx);
    Vec3 findSafeSpot(const Vec3& corpse, const GameSnapshot& s, BotContext& ctx) const;
    Vec3 findEscapeSpot(const Vec3& from, const Vec3& corpse, const GameSnapshot& s, BotContext& ctx) const;
    std::vector<Vec3> corpseApproaches(const Vec3& from, const Vec3& corpse, BotContext& ctx) const;
    BehaviorStatus approachCorpse(const GameSnapshot& s, const Vec3& corpse, BotContext& ctx);

    float corpseRunTotalDistance_ = 0.0f;
    TimePoint deadSince_{};
    PathDetours detours_;
    std::vector<Vec3> approaches_; // spots to retrieve the corpse from, best first
    std::size_t approachIndex_ = 0;
    std::optional<TimePoint> approachRetryAt_;
    std::optional<TimePoint> lastRelease_;
    std::optional<TimePoint> lastRetrieve_;
    std::vector<Vec3> ghostPath_;
    std::size_t ghostIndex_ = 0;
    std::optional<Vec3> safeSpot_;
    TimePoint safeSpotStarted_{}; // set out for safeSpot_ at
    TimePoint safeSpotThought_{}; // last looked for a better one at
    bool safeSpotGivenUp_ = false;
    std::optional<TimePoint> campedSince_; // in range with a hostile near the ghost since
    bool warnedNoCorpse_ = false;
    std::optional<TimePoint> ghostSince_;
    std::optional<Vec3> lastGhostPosition_;
    // Where the ghost stood still with the forward key down, and since when.
    std::optional<Vec3> stillAt_;
    float stillHeading_ = 0.0f; // and facing this way
    TimePoint stillSince_{};
    bool keysRefreshed_ = false; // once per standstill
};

// 780: runs away along planned escape route after resurrecting near hostiles, before resting.
class ResurrectEscapeBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "ResurrectEscape";
    }

    int priority() const override
    {
        return priority::ResurrectEscape;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;
};

// 800: acquire target (Target.Next), approach, pull, fight.
class CombatBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Combat";
    }

    int priority() const override
    {
        return priority::Combat;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    Guid chooseTarget(const GameSnapshot& s, BotContext& ctx) const;
    BehaviorStatus acquire(const UnitState& unit, const GameSnapshot& s, BotContext& ctx);
    BehaviorStatus pull(const UnitState& unit, const GameSnapshot& s, BotContext& ctx);
    BehaviorStatus fight(const UnitState& unit, const GameSnapshot& s, BotContext& ctx);
    bool backFromBehind(const UnitState& unit, const GameSnapshot& s, BotContext& ctx);
    void stopBacking(BotContext& ctx);
    bool targetClosingIn(const UnitState& unit, const GameSnapshot& s, TimePoint now);

    Guid pullGuid_ = 0;
    // When "target hp=" was last logged, and of which target: once a second.
    TimePoint hpSaidAt_{};
    Guid hpSaidOf_ = 0;
    // The same for "taking the target".
    TimePoint takingSaidAt_{};
    Guid takingSaidOf_ = 0;
    bool pullFinished_ = false;
    bool pullActionExecuted_ = false;
    TimePoint pullActionAt_{}; // when the last pull rule went off
    // The target that fled, until it is dead or gone: fought first again
    // once it is back, before the mob fought meanwhile.
    Guid fled_ = 0;
    Guid tappedGuid_ = 0;      // the target seen tagged by another player, since
    TimePoint tappedSince_{};
    std::optional<TimePoint> autoRepeatStartedAt_;
    // The wand's Shoot, pressed by us, and stopped in the game by a step
    // (a dodge, Add Avoidance): the client still says it shoots.
    // Walking in for a line of sight: once the mob shows again, on a few
    // yards more - past the edge of the tree it was behind (walkForSight).
    // Walking in to take the target: on to the margin once started, as the
    // chase does - a mob wandering off at the edge was followed a step at a time.
    bool acquireWalking_ = false;
    Guid sightGuid_ = 0;
    bool sightLost_ = false;
    std::optional<float> sightWalkTo_;
    bool walkForSight(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float dist);
    bool wandShooting_ = false;
    bool wandStoppedByStep_ = false;
    std::optional<Vec3> lastFightPosition_;
    TimePoint pullStarted_{};
    bool inPullRange_ = false;
    bool pullWalking_ = false; // closing in on the pull target last tick
    // Chasing a target that is out of reach: in to kChaseMargin of the reach,
    // not just over its edge, so one that keeps walking away is not followed
    // a step at a time (the key down one tick and up the next). Chasing again
    // only once it is out of reach again.
    bool pullChase_ = false;
    bool fightChase_ = false;
    std::optional<float> pullCloseTo_; // walking on to this distance before pulling
    TimePoint inPullRangeSince_{};
    Guid tabTarget_ = 0;
    // Taking the target with a click: tried once a mob, before Tab.
    bool clickTried_ = false;
    TimePoint clickSince_{};
    TimePoint lastStrayClear_{};
    int tabAttempts_ = 0;
    std::optional<TimePoint> lastTab_;
    // Party system: when "/assist <leader>" was last typed, for the leader's target.
    std::optional<TimePoint> lastAssist_;
    // Backing up to bring the mobs hitting us from behind round in front.
    bool backing_ = false;
    Vec3 backFrom_{};
    TimePoint backSince_{};
    TimePoint backRestUntil_{};
    Guid approachGuid_ = 0;
    Vec3 approachFrom_{};
    TimePoint approachSampledAt_{};
    bool closingIn_ = false;
    // Stepping away, mid-fight, from a mob too strong to fight that stands or
    // comes within reach of noticing us.
    // adds: away from a mob that would join the fight too, not only from one
    // too strong to fight.
    bool dodgeMobs(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, bool adds = true);
    void stopDodging(BotContext& ctx);
    bool dodging_ = false;
    int dodgeStrafe_ = 0; // 1 left, -1 right
    int dodgeAlong_ = 0;  // 1 forward, -1 backward
    TimePoint dodgeSince_{};
    TimePoint dodgeSteeredAt_{};
    TimePoint dodgeRestUntil_{};
    // The step a script asked for (player.WalkBackward(10)) under way.
    bool takeScriptMove(const UnitState& unit, const GameSnapshot& s, BotContext& ctx);
    void stopScriptMove(BotContext& ctx);
    std::optional<ScriptMove> scriptMove_;
    // Between melee reach and a minimum range (a hunter at 5-8 yards), where
    // nothing in the rotation fires: stepped out of (leaveDeadZone).
    void leaveDeadZone(const UnitState& unit, const GameSnapshot& s, BotContext& ctx, float dist);
    Guid deadZoneOf_ = 0;
    TimePoint deadZoneAt_{};
    std::optional<float> deadZoneIn_; // walking in to this distance
    std::optional<Vec3> deadZoneBackFrom_; // where the last step back started
    Vec3 scriptMoveFrom_;
    TimePoint scriptMoveSince_{};
    Vec3 scriptMoveProgressAt_;
    TimePoint scriptMoveProgressSince_{};
    TimePoint lastDodgeLog_{};
};

// 700: walk to each pending corpse, loot, optionally skin.
class LootBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Loot";
    }

    int priority() const override
    {
        return priority::Loot;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    void discoverNearby(const GameSnapshot& s, BotContext& ctx);
    void selectNext(const GameSnapshot& s, BotContext& ctx);
    void finish(const UnitState* unit, BotContext& ctx);
    void giveUp(const std::string& reason, BotContext& ctx);

    Guid current_ = 0;
    bool skinOnly_ = false; // another player's kill: never looted
    // A skin counts only once its cast was seen: loot still arriving from the
    // corpse would otherwise pass for the skin.
    bool skinCastSeen_ = false;
    // When the bags last changed at this corpse: skinning waits for the loot
    // to finish arriving.
    int lastCarried_ = -1;
    std::optional<TimePoint> bagsChangedAt_;
    struct CorpseProgress
    {
        bool lootHandled = false;
        bool skinHandled = false;
    };
    std::map<Guid, CorpseProgress> completed_;
    std::optional<TimePoint> lastNavigationScan_;
    TimePoint moveStarted_{};
    // Purse and bags before the first interact: loot arriving in them ends the
    // corpse, since the 2.5.6 client keeps a looted corpse flagged lootable.
    std::int64_t moneyBefore_ = -1;
    std::vector<ItemStack> itemsBefore_;
    int freeSlotsBefore_ = -1;
    bool lootReceived_ = false;
    bool flagSeen_ = false; // the corpse was flagged lootable at some point
    TimePoint lastInteract_{};
    // The skin wait drawn at the last interact, and the loot wait drawn on
    // arriving at the corpse (lootReadySince_): no two alike.
    Duration skinWait_{2500};
    Duration lootWait_{};
    std::optional<TimePoint> lootReadySince_;
    int lootAttempts_ = 0;
    // When "/target <name>" was typed for the corpse (see targetsByName).
    std::optional<TimePoint> targetTypedAt_;
    // True once the corpse is the client's target, or needs not be; false
    // while waiting for it; nullopt when it cannot be targeted (gives up).
    std::optional<bool> targetCorpse(const GameSnapshot& s, const UnitState& corpse, BotContext& ctx);
    int skinAttempts_ = 0;
    int skinItemsBefore_ = 0;
    bool skinLootReceived_ = false;
    bool skinCasting_ = false;
    bool skinRetryMoving_ = false;
    std::optional<TimePoint> skinFinishedAt_;
    std::optional<TimePoint> lootDoneAt_; // looting over; waiting on the skinnable flag
};

// 650: an enabled herb/ore node inside the area comes before any pull: the bot
// walks a winding detour to it (and on through the nodes beyond), fights only
// a mob standing in the way, gathers, then resumes.
class GatherBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Gather";
    }

    int priority() const override
    {
        return priority::Gather;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;
    void onYield(BotContext& ctx, std::string_view next) override;
    std::optional<Vec3> approachDestination(const GameSnapshot& s, BotContext& ctx) const override;

private:
    // Every node worth going for, nearest (counting height twice) first.
    std::vector<const GameObjectState*> candidates(const GameSnapshot& s, BotContext& ctx) const;
    const GameObjectState* selectTarget(const GameSnapshot& s, BotContext& ctx) const;
    bool safe(const GameObjectState& node, const GameSnapshot& s, const BotContext& ctx, bool questNode) const;
    // A mob to pull standing by the way still to walk to the node, nearer than
    // the node: fought first rather than walked into.
    const UnitState* mobInTheWay(const GameSnapshot& s, BotContext& ctx) const;
    // The winding way to the node and on through the nodes beyond it.
    void planWay(const GameSnapshot& s, BotContext& ctx);
    std::vector<Vec3> way_;
    std::size_t wayIndex_ = 0;
    std::size_t wayToNode_ = 0; // the node's index in way_
    std::uint64_t wayRevision_ = 0;
    std::uint64_t wayRandom_ = 0x9E3779B97F4A7C15ull;
    // Why the node being gathered is given up now; empty to carry on.
    std::string stopReason(const GameSnapshot& s, BotContext& ctx) const;
    std::string stopReason_; // logged when the detour ends early
    std::string yieldingTo_;
    BehaviorStatus finish(BotContext& ctx, const std::string& reason);
    void retry(const GameSnapshot& s, BotContext& ctx);

    std::optional<GameObjectState> target_;
    Guid preferredTargetGuid_ = 0;
    GatheringProfessionConfig profession_;
    std::vector<QuestObjectTarget> questItems_;
    TimePoint started_{};
    TimePoint phaseStarted_{};
    std::optional<TimePoint> missingSince_;
    std::optional<TimePoint> lootReceivedAt_;
    std::optional<TimePoint> settledSince_; // stopped at the node, facing it
    Duration settleWait_{};                 // drawn when it stopped there
    std::vector<ItemStack> bagsBefore_;
    std::optional<Vec3> reposition_;
    // Where to walk to before the node: the node itself, or where a node lying
    // by an obstacle or No Go zone is reached from, nearest first. The last
    // yards from such a spot are walked straight at the node.
    void planApproach(const GameSnapshot& s, BotContext& ctx);
    std::vector<Vec3> approaches_;
    std::size_t approachIndex_ = 0;
    bool approachReached_ = false;
    // The approach gives up only after approachTimeout without progress: a
    // node far off takes longer than that to walk to.
    float closest_ = 0.0f;
    Vec3 progressAt_;
    std::uint64_t approachRevision_ = 0;
    bool interacting_ = false;
    bool gathered_ = false;
    bool castStarted_ = false;
    bool interactedOnce_ = false;
    std::vector<PlayerState::QuestProgress> questProgressBefore_;
    std::map<std::string, std::vector<PlayerState::TrackedObjective>> trackedQuestsBefore_;
    bool questProgressAdvanced(const GameSnapshot& s, const BotContext& ctx) const;
    int failures_ = 0;
    int harvests_ = 0;
};

// 300: a fishing profile - walk to a fishing spot, face the water, keep a
// lure on the pole, cast, wait for the bite and hook the fish (Glider's
// "Fishing" mode). Above Pull: fishing does not go looking for fights, but
// anything that attacks is fought (Combat) and looted as usual.
class FishBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Fish";
    }

    int priority() const override
    {
        return priority::Fish;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    enum class Stage
    {
        Travel,
        Face,
        Lure,
        Cast,
        WaitBobber,
        WaitBite,
        Hook,
        Collect,
        Pause
    };

    struct Spot
    {
        Vec3 position;
        std::optional<float> heading; // the profile's; none when fishing where the character stood
        float safeRadius = 15.0f;
    };
    bool fishable(const GameSnapshot& s, BotContext& ctx);
    BehaviorStatus step(const GameSnapshot& s, BotContext& ctx);
    std::optional<Spot> chooseSpot(const GameSnapshot& s, const BotContext& ctx) const;
    const GameObjectState* findBobber(const GameSnapshot& s) const;
    bool lureDue(const GameSnapshot& s, BotContext& ctx) const;
    bool applyLure(const GameSnapshot& s, BotContext& ctx);
    void setStage(Stage next, BotContext& ctx);
    void pause(BotContext& ctx, Duration min, Duration max);
    Duration between(Duration min, Duration max);

    Stage stage_ = Stage::Travel;
    std::optional<Spot> spot_;
    TimePoint stageStarted_{};
    TimePoint pauseUntil_{};
    TimePoint hookAt_{};
    // The interact key held back by the chat is pressed again for this long.
    static constexpr Duration kHookRetry{1500};
    std::optional<TimePoint> biteOverAt_; // while collecting: when the bobber stopped biting
    std::set<Guid> bobbersBefore_;        // bobbers in the water before our cast
    Guid bobber_ = 0;
    Vec3 bobberPosition_{};
    // Where on the screen to look for the bobber under the mouse (1.60.1,
    // which has no memory interaction): found while waiting for the bite, and
    // right-clicked when it comes.
    BotContext::SeekSpots bobberSpots_;
    bool bobberNotFound_ = false; // every spot tried this cast
    std::vector<ItemStack> bagsBefore_;
    // After a fish counted as got away: until when a catch reaching the bags
    // still turns it into a catch (the bags can update after the bobber goes).
    std::optional<TimePoint> lateCatchUntil_;
    int failedCasts_ = 0;
    bool stopped_ = false; // gave up after maxFailedCasts
    std::optional<TimePoint> lureAppliedAt_;
    bool lureUnavailable_ = false;
    // Until then a "replace the lure?" dialog is looked for and said yes to, on
    // a client whose UI can be read.
    std::optional<TimePoint> lureConfirmUntil_;
    bool warnedNoBite_ = false;
    bool warnedNoSpell_ = false;
    std::uint64_t random_ = 0x9E3779B97F4A7C15ull;
};

// 560: on the way somewhere - roaming, to a quest spot, on a vendor trip or a
// hand-in - a hostile mob ahead within its aggro radius is fought first,
// rather than walked into: it would come for us anyway, with our back turned.
// Whatever the target list, Questing or a trip's defend-only allow.
class ClearPathBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "ClearPath";
    }

    int priority() const override
    {
        return priority::ClearPath;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;

private:
    // The mob canRun found: fought in tick as found. Asked again there, with
    // the way changed in between, the answer was another, and the bot went
    // back and forth between ClearPath and the hand-in every few ticks.
    Guid found_ = 0;
};

// 450: Questing - a quest in the log is done: walk to the NPC that takes it
// back, talk to it, pick the quest, Continue, choose the reward the settings
// ask for and Complete Quest. Before it goes it takes the quests the NPC
// offers (a yellow !) that Questie says are for the character, up to a full
// log. With nothing to hand in, an NPC in sight with a yellow ! is visited for
// its quests alone. The way there and back out, into buildings too, is the
// navigator's (Pathfinder). Without Pathfinder, an NPC standing in one of the
// profile's No Go zones is left alone: the bot would most likely get stuck in
// there. A quest (or NPC) that goes wrong is left for 10 minutes.
class QuestHandInBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "QuestHandIn";
    }

    int priority() const override
    {
        return priority::QuestHandIn;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

    bool isTravelling() const
    {
        return stage_ == Stage::Travelling;
    }

    float totalTripDistance() const
    {
        return totalTripDistance_;
    }

    float remainingTripDistance(const Vec3& pos) const
    {
        return quest_ && quest_->handInAt ? distance2D(pos, *quest_->handInAt) : 0.0f;
    }

    std::string destinationName() const
    {
        return pickingUp_ ? "Quest Giver" : "Quest Hand-in";
    }

private:
    enum class Stage
    {
        Travelling,
        Opening,
        Dialog,
        Accepting
    };
    const QuestTarget* pick(const GameSnapshot& s, BotContext& ctx) const;
    // An NPC in sight with quests for the character (a yellow !), nearest first.
    const UnitState* questGiver(const GameSnapshot& s, BotContext& ctx) const;
    bool pickingUp_ = false; // visiting for quests to take, nothing to hand in
    // In a No Go zone, without Pathfinder: not to be gone to. Said once per NPC.
    bool shutIn(BotContext& ctx, const std::string& npc, const Vec3& at) const;
    mutable std::set<std::string> shutInSaid_;
    // Quest givers visited lately, and what they offered then: one is left
    // alone only while it offers nothing new. A level up or a quest handed in
    // can open a new one (Deputy Willem's next), and that is gone for at once.
    struct Visited
    {
        TimePoint until{};
        std::vector<std::string> offered;
    };
    std::map<std::uint32_t, Visited> leaveNpcUntil_;
    BehaviorStatus finish(BotContext& ctx, const std::string& reason, bool failed);
    // Handed in (or given up): done here.
    BehaviorStatus leave(BotContext& ctx, const std::string& reason, bool failed);
    BehaviorStatus dialog(const GameSnapshot& s, BotContext& ctx);
    BehaviorStatus chooseReward(const GameSnapshot& s, BotContext& ctx);
    // Handed in: take what else the NPC offers while there, then walk back.
    BehaviorStatus accept(const GameSnapshot& s, BotContext& ctx, const UnitState* npc);
    Stage afterOpening_ = Stage::Dialog;
    int accepted_ = 0;
    bool refreshOffers_ = false; // recheck the NPC after a hand-in or acceptance
    bool handedIn_ = false;
    bool clickAt(BotContext& ctx, const UiFrame& frame);

    std::optional<QuestTarget> quest_;
    Stage stage_ = Stage::Travelling;
    float totalTripDistance_ = 0.0f;
    // Where the last 200 yards of the way began: each 200 walked gives the
    // hand-in its time again (a long way, Westfall to Lakeshire, ran out).
    std::optional<Vec3> progressFrom_;
    TimePoint started_{};
    TimePoint stageStarted_{};
    TimePoint nextStep_{};
    TimePoint leftAt_{};
    Guid npc_ = 0;
    bool mouseTried_ = false;
    int rightClicks_ = 0; // on the NPC under the mouse, since a window last opened
    bool nothingToClickSaid_ = false;
    bool otherOfferSaid_ = false;
    // An offer turned out to be a quest turned off in the Questing panel, and
    // was declined: this visit clicks no more rows it cannot name.
    bool declinedIgnored_ = false;
    int opens_ = 0;
    // Talking from where the bot stood opened nothing: it goes right up to
    // the NPC. A door frame between them hid the NPC from the mouse and the
    // interact key, though it stood within reach.
    bool closeIn_ = false;
    // The reward panel: each reward's tooltip as hovered, and the ones tried.
    std::vector<std::optional<UiItemTooltip>> rewards_;
    std::size_t hovering_ = 0;
    // The reward the mouse was put on, and since when; the tooltip's owner
    // then, and the owners of the rewards read, none of which the hovered
    // reward's tooltip may have.
    std::optional<std::size_t> hoverAt_;
    TimePoint hoverSince_{};
    std::uint64_t ownerAtMove_ = 0;
    std::vector<std::uint64_t> readOwners_;
    std::vector<std::size_t> rewardsTried_;
    bool rewardClicked_ = false;
    // The reward clicked, how often, and when Complete Quest was clicked after it.
    std::optional<std::size_t> picked_;
    int pickClicks_ = 0;
    std::optional<TimePoint> completeAt_;
    bool completed_ = false; // Complete Quest clicked
    int progressPanelClicks_ = 0;
    int completeClicks_ = 0;
    // Mobs of a quest still to do were in sight just now: a hand-in far off
    // waits for them. With one scale missing among the whelps, the bot left
    // for Darkshire to hand in a letter.
    void lookForQuestMobs(const GameSnapshot& s, BotContext& ctx);
    std::optional<TimePoint> questMobsSeenAt_;
    bool questMobsHere_ = false;
    mutable std::set<std::uint32_t> waitSaid_;
};

struct QuestBuff;

// 660: Questing - a quest whose objectives need a buff from an item it gave
// (Shhh! We're Hunting Kobolds: the Kobold Tracking Kit's Hunting Kobolds,
// which shows the Kobold Tracks): the item used - "/use item:<id>", then by
// its name - whenever the buff is not on. One that gives none is left for a
// minute.
class QuestBuffBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "QuestBuff";
    }

    int priority() const override
    {
        return priority::QuestBuff;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    const QuestBuff* wanted(const GameSnapshot& s, BotContext& ctx) const;
    static std::string questName(BotContext& ctx, std::uint32_t questId);

    const QuestBuff* current_ = nullptr;
    int tries_ = 0;
    std::optional<TimePoint> usedAt_;
    std::map<std::uint32_t, TimePoint> retryAt_;
};

// 470: Questing - an item in bags starts a quest (e.g. Captain Sanders' Treasure
// Map, Westfall Deed): right-click/use the item, wait for the quest detail
// window, accept the quest into the quest log. If accepting or opening fails,
// the item is left on cooldown for 5 minutes.
class InventoryQuestBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "InventoryQuest";
    }

    int priority() const override
    {
        return priority::InventoryQuest;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    enum class Stage
    {
        Idle,
        OpeningItem,
        WaitingDetail,
        Accepting,
        Confirming
    };

    std::optional<std::pair<std::uint32_t, std::uint32_t>> pickCandidate(const GameSnapshot& s, BotContext& ctx) const;
    void useItem(std::uint32_t itemId, BotContext& ctx);
    BehaviorStatus finish(BotContext& ctx, const std::string& reason, bool failed);

    Stage stage_ = Stage::Idle;
    std::uint32_t currentItemId_ = 0;
    std::uint32_t currentQuestId_ = 0;
    std::string itemName_;
    TimePoint stageStarted_{};
    TimePoint nextStep_{};
    int attempts_ = 0;
    std::map<std::uint32_t, TimePoint> failedUntil_;
};

// 690: out of where a mob comes back within seconds of its kill (FastRespawn)
// once the fight is over and what is needed is looted: away from the place,
// clear of the mobs about, to rest there - not killing the same mob for ever.
class LeaveRespawnBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "LeaveRespawn";
    }

    int priority() const override
    {
        return priority::LeaveRespawn;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    std::optional<Vec3> from_; // the place left
    std::optional<Vec3> to_;
    // The other ways out, tried in turn where the one gone for has no way to it.
    std::vector<Vec3> otherWays_;
    TimePoint startedAt_{};
    // No way out of it found at all: not tried again at once, every tick.
    std::optional<Vec3> gaveUpOn_;
    TimePoint gaveUpUntil_{};
};

// 440, Train Abilities: on reaching an even level, to the nearest trainer of
// the character's class (by the quest data), through its gossip line to the
// trainer window, and every spell it lists learned that the money allows -
// a row chosen, Train clicked, and the money seen to go down.
// A flight master in sight whose flight path the character has not learnt
// (the green "!" over its head): walked to and talked to, which learns it,
// and the flight map that opens closed again.
class FlightMasterBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Learn Flight Path";
    }

    int priority() const override
    {
        return priority::LearnFlightPath;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    BehaviorStatus finish(BotContext& ctx, const std::string& reason, bool failed);

    Guid npc_ = 0;
    std::string npcName_;
    TimePoint started_{}, talkedAt_{};
    int talks_ = 0;
    bool gossipChosen_ = false;
    // Without Lua (1.60.1) talked to as a trainer is: a right-click on it
    // under the mouse, the interact key where the mouse finds nothing, and
    // right up to it after a talk that came to nothing.
    int rightClicks_ = 0;
    bool mouseTried_ = false;
    bool closeIn_ = false;
    // Flight masters tried and given up on, not again until then.
    std::map<Guid, TimePoint> notBefore_;
};

class TrainerBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Train";
    }

    int priority() const override
    {
        return priority::Train;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

    // A trip due: an even level reached since the last one trained at.
    bool due() const
    {
        return due_;
    }

private:
    enum class Stage
    {
        Travelling,
        Opening,
        Gossip,
        Training,
        Returning
    };

    BehaviorStatus finish(BotContext& ctx, const std::string& reason, bool failed);
    bool clickAt(BotContext& ctx, const UiFrame& frame);

    int levelSeen_ = 0; // the level last looked at; 0: none yet
    bool due_ = false;
    TimePoint notBefore_{}; // after a failed trip, not again until then
    // No money: a vendor trip asked for first (BotContext::vendorBeforeTraining).
    bool sellAsked_ = false;
    TimePoint sellAskedAt_{};
    int failures_ = 0;
    std::optional<TrainerSpot> trainer_;
    Stage stage_ = Stage::Travelling;
    Guid npc_ = 0;
    TimePoint started_{}, stageStarted_{}, stepAt_{};
    std::optional<Vec3> progressFrom_;
    std::optional<Vec3> tripFrom_;
    std::optional<TaxiTrip> taxiTrip_;
    bool taxiChecked_ = false;
    std::size_t taxiKnown_ = 0;
    int opens_ = 0;
    int rightClicks_ = 0;
    bool mouseTried_ = false;
    bool closeIn_ = false;
    // In the trainer window: the row tried now, top first, and the Train
    // click made for it, waiting on the money to go down.
    std::size_t row_ = 0;
    bool rowClicked_ = false;
    bool trainClicked_ = false;
    std::int64_t moneyBefore_ = -1;
    int trainClicks_ = 0;
    int learned_ = 0;
};

// 430, Auto Equip New Items: gear that came into the bags (loot, a quest's
// reward) looked at in the open bags - its tooltip, and beside it what is
// worn in its slot (the client's comparison tooltips) - and put on with a
// right-click when it is better for the class (gearRank). 64-bit clients
// whose bags the bot can read (1.60.1).
class AutoEquipBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "AutoEquip";
    }

    int priority() const override
    {
        return priority::AutoEquip;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

private:
    enum class Step
    {
        OpenBags,
        Find,
        Read,
        Equip,
        Confirm,
        Close
    };

    // The item looked at now, done with: on to the next, or the bags shut.
    BehaviorStatus next(BotContext& ctx);

    // What the bags held last tick, by item: what is more now came in.
    std::map<std::uint32_t, int> seen_;
    bool seenOnce_ = false;
    std::vector<ItemStack> pending_;
    // Names looked at already and not gear: not hovered for again.
    std::set<std::string> notGear_;
    // What was taken off for something better: back in the bags, not new,
    // and never put on again over it.
    std::set<std::string> takenOff_;
    std::vector<std::string> takingOff_; // what the piece going on now takes the place of
    Step step_ = Step::OpenBags;
    TimePoint stepAt_{};
    bool compareSet_ = false; // alwaysCompareItems typed this session
    bool bagsOpened_ = false; // opened by us, to be shut after
    std::set<std::uint64_t> looked_; // bag buttons hovered for the item now
    std::optional<UiBagSlot> slot_;
    int countBefore_ = 0;
};

// 500: bags are full - walk the profile's vendor route, sell what the rules
// allow, then hand back to Navigation, which resumes at the nearest waypoint.
class VendorBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Vendor";
    }

    int priority() const override
    {
        return priority::Vendor;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;

    bool isTravelling() const
    {
        return (stage_ == Stage::Travelling || stage_ == Stage::Returning) && !path_.empty();
    }

    float totalTripDistance() const
    {
        return totalTripDistance_;
    }

    float remainingTripDistance(const Vec3& pos) const
    {
        return remainingPathDistance(pos, path_, pathIndex_);
    }

    std::string destinationName() const
    {
        return stage_ == Stage::Returning ? "Grind Route" : "Vendor";
    }

private:
    enum class Stage
    {
        Travelling,
        Approaching,
        Opening,
        Selling,
        Repairing,
        Returning,
        Done
    };

    void setStage(Stage next, BotContext& ctx);
    const UnitState* findVendor(const GameSnapshot& s, BotContext& ctx) const;
    // The leg out: along the grind route to where the vendor route joins it,
    // then along the vendor route to the vendor.
    void buildOutboundPath(const GameSnapshot& s, BotContext& ctx);
    // The leg home: back along the vendor route from wherever we are standing.
    void buildReturnPath(const GameSnapshot& s, BotContext& ctx);
    // Steers along path_. True once its last point has been reached.
    bool walkPath(const GameSnapshot& s, BotContext& ctx);
    void logPathDistance(const GameSnapshot& s, BotContext& ctx);
    // `backoff` blocks further trips for a while. Set it whenever the bags are
    // still full at the end of a trip: going straight back would just repeat
    // the same trip forever and the grind route would never resume.
    BehaviorStatus finish(BotContext& ctx, const std::string& reason, Duration cooldown);
    BehaviorStatus finish(BotContext& ctx, const std::string& reason, bool backoff = true);

    Stage stage_ = Stage::Travelling;
    float totalTripDistance_ = 0.0f;
    std::vector<Vec3> path_; // the leg being walked, first point first
    std::size_t pathIndex_ = 0;
    std::size_t linkPoints_ = 0; // leading points of path_ that are grind waypoints
    PathDetours detours_;
    bool enteredAtFront_ = true; // which end of the vendor route meets the grind route
    bool entryReached_ = false;  // outbound leg must visit the outer endpoint before approaching the NPC
    // Where the bot stood when the trip began: the way back ends there. The
    // points of path_ from homeFrom_ on lead from the vendor route to it, and
    // go round No Go zones and obstacles, unlike the route itself.
    std::optional<Vec3> tripFrom_;
    std::size_t homeFrom_ = 0;
    Guid vendor_ = 0;
    TimePoint started_{};
    TimePoint stageStarted_{};
    TimePoint leftAt_{};    // when the trip last lost control, so time away is not charged to it
    bool timedOut_ = false; // out of time and walking the route home on a last budget
    int freeSlotsOnArrival_ = -1;
    int freed_ = 0;     // bag slots the sale actually freed, measured before walking back
    bool trip_ = false; // a vendor run is in progress; survives combat interruptions
    TimePoint noTripUntil_{};
    std::optional<TimePoint> lastDistanceLog_;
    std::optional<TimePoint> retryPathAt_; // an unreachable vendor path point is tried again then
    std::optional<TimePoint> lastInteract_;
    bool interactSent_ = false;
    bool mouseTried_ = false; // the vendor was looked for under the mouse
    // Right-clicks on the vendor under the mouse that opened nothing yet.
    Guid clickedVendor_ = 0;
    int rightClicks_ = 0;
    bool gossipHandled_ = false;
    bool soldItems_ = false;
    // Without Lua on a client whose UI can be read: the merchant window's
    // buttons are clicked, Sell All Junk, its confirmation, Repair All.
    enum class UiStep
    {
        Open,
        Confirm,
        OpenBags,
        ScanBags,
        HoverBag,
        ReadTooltip,
        VerifySale,
        Repair,
        Close,
        Done
    };
    UiStep uiStep_ = UiStep::Open;
    TimePoint uiStepAt_{};
    // The StaticPopups open before Sell All Junk was clicked (bit i-1 for
    // StaticPopup<i>): its confirmation is the one that was not.
    unsigned popupsBeforeJunk_ = 0;
    TimePoint uiHoverStarted_{};
    std::set<std::uint64_t> visitedBagButtons_;
    // Slots looked at that still hold something (kept, or a sale that did not
    // take): once they are all the full slots there are, the rest are empty
    // and are not hovered at all.
    int keptBagSlots_ = 0;
    // How long the slowest tooltip so far took to show: an empty slot (no
    // tooltip ever) is left after a little more than that, not a fixed 500 ms.
    std::optional<Duration> slowestTooltip_;
    std::optional<UiBagSlot> uiBagSlot_;
    std::optional<UiItemTooltip> uiTooltip_;
    int slotsBeforeUiSale_ = -1;
    int itemsBeforeUiSale_ = 0;
    bool openedBags_ = false;
    BehaviorStatus tickMerchantUi(const GameSnapshot& s, BotContext& ctx);
    bool repairedItems_ = false;
    int unreachableCount_ = 0;
    bool giveUpTrip_ = false;
    std::map<std::uint32_t, TimePoint> unreachableVendors_;
    std::map<std::string, TimePoint> unreachableVendorNames_;
    bool switchToNextVendor(const GameSnapshot& s, BotContext& ctx, const std::string& why = "unreachable after 3 attempts");
    // At the vendor and no merchant window came (a mount vendor's text, say):
    // on to another vendor where there is one.
    BehaviorStatus merchantNeverOpened(const GameSnapshot& s, BotContext& ctx);

public:
    // True while the bot is on a vendor trip, so Navigation stays out of the way.
    bool onTrip() const
    {
        return trip_;
    }
};

// 600: eat/drink/bandage via the class' rest block, then buffs.
class RestBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Rest";
    }

    int priority() const override
    {
        return priority::Rest;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;

    // Stays true when resting is interrupted (combat) so it resumes afterwards.
    bool isResting() const
    {
        return resting_;
    }

private:
    // Re-uses food/drink whose buff never appeared, bypassing the item's
    // cooldown. Without it a missed press idles the whole rest.
    void retryConsumables(const GameSnapshot& s, BotContext& ctx, TimePoint now);

    bool resting_ = false;
    TimePoint lastConsumable_{}; // spaces eating and drinking apart
    std::optional<TimePoint> foodMissingSince_;
    std::optional<TimePoint> drinkMissingSince_;
    TimePoint restStarted_{};
    TimePoint lastTick_{}; // a gap in the ticks is a rest broken off (a fight, a death)
    TimePoint noRestUntil_{};
    bool unsafeLogged_ = false;
};

// 200: choose the next mob to pull (hands it to CombatBehavior via ctx.combat.engaged).
class PullBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Pull";
    }

    int priority() const override
    {
        return priority::Pull;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    std::optional<Vec3> approachDestination(const GameSnapshot& s, BotContext& ctx) const override;

private:
    // A mob standing by a corpse waiting to be looted, to fight first.
    const UnitState* corpseGuard(const GameSnapshot& s, BotContext& ctx) const;
};

// 100: follow the profile's waypoint route.
class NavigationBehavior final : public IBotBehavior
{
public:
    std::string_view name() const override
    {
        return "Navigation";
    }

    int priority() const override
    {
        return priority::Navigation;
    }

    bool canRun(const GameSnapshot& s, BotContext& ctx) override;
    BehaviorStatus tick(const GameSnapshot& s, BotContext& ctx) override;
    void onEnter(const GameSnapshot& s, BotContext& ctx) override;
    void onExit(BotContext& ctx) override;
    void onYield(BotContext& ctx, std::string_view next) override;

    Humanizer& humanizer()
    {
        return humanizer_;
    }

    const RouteCorrector& corrector() const
    {
        return corrector_;
    }

    // The spot roaming is walking to, if any.
    // Build Route: the circuit point being walked to.
    std::optional<Vec3> roamTarget() const
    {
        if (questSpot_)
        {
            return questSpot_;
        }

        if (!circuit_.empty() && circuitIndex_ < circuit_.size())
        {
            return circuit_[circuitIndex_];
        }

        return roamTarget_;
    }

    // The legs still to walk there: corners it goes round to stay inside the
    // route, ending at the spot.
    std::vector<Vec3> roamLegs() const;
    void resetCircuit()
    {
        circuit_.clear();
        circuitLeadIn_.clear();
        circuitLeadInIndex_ = circuitIndex_ = 0;
        circuitRetryAt_ = {};
        roamTarget_.reset();
        roamPath_.clear();
        questSpot_.reset();
        questPath_.clear();
        questSpotTotalDistance_ = 0.0f;
        questOffSurfaceSince_.reset();
        questSurfaceCheckAt_ = TimePoint{};
    }

    bool isTravellingToQuestSpot() const
    {
        return questSpot_.has_value();
    }

    float questSpotTotalDistance() const
    {
        return questSpotTotalDistance_;
    }

    float remainingQuestSpotDistance(const Vec3& pos) const
    {
        if (!questSpot_)
        {
            return 0.0f;
        }
        if (!questPath_.empty() && questLeg_ < questPath_.size())
        {
            return remainingPathDistance(pos, questPath_, questLeg_);
        }
        return distance2D(pos, *questSpot_);
    }

private:
    // Questing with Pathfinder and no profile route: the quests alone say
    // where to go.
    static bool questingWithoutRoute(const BotContext& ctx);
    // On the first start of a session the character may be parked on the vendor
    // or ghost route. Heading straight for the nearest grind waypoint could cut
    // across anything, so walk whichever of those routes is closest back to
    // where it meets the grind route first.
    void planLeadIn(const GameSnapshot& s, BotContext& ctx);
    // Roaming inside the route (RoamConfig): false when there is no clear spot
    // to walk to, and the route is walked as usual instead.
    bool roamTick(const GameSnapshot& s, BotContext& ctx);
    // Build Route: walks circuit_, and builds the next one after each lap.
    // False when no circuit fits the area; roaming takes over then.
    bool circuitTick(const GameSnapshot& s, BotContext& ctx);
    // Back on the circuit at its point nearest the character, the way there
    // taken round the area's corners.
    void rejoinCircuit(const GameSnapshot& s, BotContext& ctx);
    // Quest Mode: walks to the nearest spot where a quest's mobs stand, inside
    // the profile's area and not looked at lately. False when there is none.
    bool questTick(const GameSnapshot& s, BotContext& ctx);
    std::optional<Vec3> questSpot_;
    std::uint32_t questSpotQuest_ = 0;
    float questSpotTotalDistance_ = 0.0f;
    std::vector<Vec3> questPath_;
    std::size_t questLeg_ = 0;
    std::vector<std::pair<Vec3, TimePoint>> questVisited_;
    // Quests whose every place lies within a pack's reach (Avoid packed mobs),
    // set aside until then: Break Sharptusk!'s chief stands in a camp of
    // Bristleback Quilboars, and the bot stood there with nowhere to go and
    // every other quest waiting on it.
    std::map<std::uint32_t, TimePoint> questInPacks_;
    // Waiting at the spot of a quest after one mob (Garrick Padfoot), since.
    std::optional<TimePoint> questWaitSince_;
    // A cliff fall can briefly leave the player off the navigation mesh. Keep
    // the selected quest destination and retry from the landing position.
    std::optional<TimePoint> questOffSurfaceSince_;
    TimePoint questSurfaceCheckAt_{};
    // The quest mob on another level the spot is the place of (its GUID), or
    // 0 for a spot of the data's; and those no way was found to, and when.
    std::uint64_t questSpotMob_ = 0;
    std::map<std::uint64_t, TimePoint> questMobNoWay_;
    TimePoint questWaitSaid_{};

    Humanizer humanizer_;
    std::optional<Vec3> roamTarget_;
    std::vector<Vec3> roamPath_;      // roamPath() to roamTarget_
    std::size_t roamLeg_ = 0;         // the point of roamPath_ being walked to
    std::optional<Vec3> roamStuckAt_; // where the current recovery started
    // A new session gets a new sequence; subsequent laps advance the same state.
    std::uint64_t roamRandom_ = []
    {
        std::random_device random;
        return (static_cast<std::uint64_t>(random()) << 32) | random();
    }();
    std::vector<Vec3> circuit_;
    std::size_t circuitIndex_ = 0;
    std::vector<Vec3> circuitLeadIn_; // from where the bot stands onto the circuit
    std::size_t circuitLeadInIndex_ = 0;
    std::size_t circuitZones_ = 0; // No Go zones and obstacles it was built round
    int circuitLaps_ = 0;
    // A leg the navigator will not plan (a gap between two obstacles narrower
    // than its margin) is walked straight: the circuit keeps it clear already.
    bool circuitStraight_ = false;
    TimePoint circuitRetryAt_{};
    TimePoint circuitPointSince_{};
    RouteCorrector corrector_;
    TimePoint waypointSince_{};
    // When the navigator began recovering on the way to this waypoint.
    std::optional<TimePoint> recoveringSince_;
    std::size_t currentWaypointIndex_ = 0;
    // How many moves the obstacle memory had learned when the corrector started
    // recording, so a detour for a jam a jump already fixed can stand down.
    std::uint64_t movesAtRecording_ = 0;
    std::vector<Vec3> leadIn_;
    std::size_t leadInIndex_ = 0;
    bool leadInConsidered_ = false;
};

// The standard Classic Glider behavior set.
std::vector<std::unique_ptr<IBotBehavior>> createDefaultBehaviors();

} // namespace og
