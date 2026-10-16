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
 *  File: BotInstance.h
 *
 *  Desc:
 *  Declarations and interface for BotInstance (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once
#include <unordered_set>

#include "og/actions/ActionService.h"
#include "og/combat/ICombatClass.h"
#include "og/common/EventBus.h"
#include "og/common/Log.h"
#include "og/core/BotContext.h"
#include "og/core/BotEngine.h"
#include "og/core/BuffThanks.h"
#include "og/core/FollowerWatch.h"
#include "og/core/MovementTrace.h"
#include "og/core/PlayerProximityAlert.h"
#include "og/input/IInputBackend.h"
#include "og/navigation/Navigator.h"
#include "og/profiles/GliderProfile.h"
#include "og/state/IGameStateProvider.h"
#include "og/targets/TargetSelector.h"

#include <map>
#include <set>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace og
{

// Thread-safe copy of an instance's state for UIs and the daemon API.
// A quest in Quest Mode, as the GUI's Questing panel shows it.
struct QuestStatus
{
    std::uint32_t id = 0;
    std::string name;
    int level = 0;
    std::vector<std::string> progress; // "Crag Boar Rib 3/6", one to each objective
    bool done = false;                 // every objective reached: to hand in
    bool current = false;              // being worked on now (the focus)
    std::string handInTo;
    bool ignored = false;
};

constexpr float kLongTravelDistanceThreshold = 250.0f;

struct TravelStatus
{
    bool active = false;
    float totalDistance = 0.0f;     // Total route distance from A to Z in yards
    float remainingDistance = 0.0f; // Remaining distance along route to Z in yards
    float estimatedSpeed = 7.0f;    // Estimated movement speed in yards/sec
    float totalSeconds = 0.0f;      // Total duration for entire route in seconds
    float remainingSeconds = 0.0f;  // Remaining duration to arrival in seconds
    std::string destinationName;    // "Vendor", "Corpse", "Quest", "Go To", etc.

    float totalMinutes() const
    {
        return totalSeconds / 60.0f;
    }

    float remainingMinutes() const
    {
        return remainingSeconds / 60.0f;
    }

    std::string formattedEta() const
    {
        if (!active)
        {
            return "—";
        }
        if (remainingSeconds < 60.0f)
        {
            return "< 1 min";
        }
        const int mins = std::max(1, static_cast<int>(std::round(remainingSeconds / 60.0f)));
        return std::format("~{} min", mins);
    }

    std::string formattedTotalDuration() const
    {
        if (!active)
        {
            return "—";
        }
        if (totalSeconds < 60.0f)
        {
            return "< 1 min";
        }
        const int mins = std::max(1, static_cast<int>(std::round(totalSeconds / 60.0f)));
        return std::format("~{} min", mins);
    }

    std::string tooltip() const
    {
        if (!active)
        {
            return {};
        }
        const std::string dest = destinationName.empty() ? "Destination" : destinationName;
        return std::format("Travel to: {}\nWhole route (A to Z): {:.0f} yd ({})\nRemaining: {:.0f} yd ({})\nSpeed: {:.1f} yd/s",
                           dest, totalDistance, formattedTotalDuration(), remainingDistance, formattedEta(), estimatedSpeed);
    }
};

struct InstanceStatus
{
    std::string name;
    std::string behavior;
    std::string objective;
    std::string lastAction;
    std::string profile;
    std::string combatClass;
    std::optional<GameSnapshot> snapshot;
    BotStats stats;
    std::string fishingProblem; // see BotContext::fishingProblem
    TimePoint now{};
    bool paused = false;
    // No game state: the client is at the login or a loading screen.
    bool waitingForGame = false;
    // The combat class's pull rules' actions, and whether one of them has a
    // button (an action bar slot or key) to press.
    std::vector<std::string> pullActions;
    bool pullOnBar = true;
    // Party system: the members in sight, and the leader among them (0 none),
    // for the radar.
    std::vector<Guid> partyMembers;
    Guid partyLeader = 0;
    std::size_t routeIndex = 0;
    // Ways round obstacles the bot has learned this session. They are already
    // part of route(); writing them to the profile is the user's call.
    std::size_t routeCorrections = 0;
    // The profile's stopAtLevel was reached and the bot has stopped for good.
    bool finished = false;
    // What the bot has learned about obstacles, and a counter that moves
    // whenever that changes (so an owner knows when to write it to the profile).
    LearnedObstacles learned;
    std::uint64_t learnedRevision = 0;
    // Players the nearby-player warning announced in the last few seconds, for
    // the radar to flash.
    std::vector<Guid> alertedPlayers;
    // Roaming inside the route: the spot the bot is walking to.
    std::optional<Vec3> roamTarget;
    // Fixed at Start, independent of the player's current radar position.
    std::optional<Vec3> pathfindingRoamOrigin;
    float pathfindingRoamRadius = 0;
    bool pathfinding = false;
    std::vector<Vec3> navigationPath; // Remaining client-map path, for every movement behavior.
    // The points still to walk there, ending at roamTarget: corners it goes
    // round to stay inside the route.
    std::vector<Vec3> roamPath;
    // Avoid packed mobs: the ground round each mob of a pack in sight that the
    // way keeps off (its reach plus BotConfig::packMargin), for the radar.
    std::vector<AvoidZone> packZones;
    // The mob the bot fights or closes in on, or the node it is going to
    // gather; 0 for neither. The radar makes it blink until it is targeted.
    Guid goingFor = 0;
    // Quest Mode on, and the quests it works on: the current ones first, then
    // by level. questLogSize counts every quest in the log, these or not.
    bool questing = false;
    std::vector<QuestStatus> quests;
    std::size_t questLogSize = 0;
    TravelStatus travel;
};

struct BotInstanceOptions
{
    std::string name = "bot";
    ActionMap actions;
    BotConfig config;
    MovementConfig movement;
    NavigatorConfig navigator;
};

// One bot = state provider + input backend + engine + profile + combat class
// + navigator + logger. The daemon runs several of these side by side.
class BotInstance
{
public:
    BotInstance(BotInstanceOptions options, std::unique_ptr<IGameStateProvider> provider, std::unique_ptr<IInputBackend> input, std::shared_ptr<const IClock> clock);
    ~BotInstance();

    BotInstance(const BotInstance&) = delete;
    BotInstance& operator=(const BotInstance&) = delete;

    void setProfile(GliderProfile profile);
    // Update scan/target options without discarding the session's generated loop.
    void setProfileOptions(GliderProfile::Options options);
    // An edited profile for the running session: like setProfile(), but the route
    // carries on from the waypoint nearest the character.
    void reloadProfile(GliderProfile profile);
    // The target filter again from the config: after General Settings change.
    void refreshTargetFilter();
    void setCombatClass(std::unique_ptr<ICombatClass> combatClass);
    // Quest Mode's knowledge of the quests; without one Questing pauses.
    void setQuestSource(std::unique_ptr<IQuestSource> source)
    {
        questSource_ = std::move(source);
        context_.questSource = questSource_.get();
        if (questSource_)
        {
            if (eliteEntries_)
            {
                questSource_->setEliteEntries(eliteEntries_);
            }
            questSource_->setIgnoredQuests(context_.ignoredQuests);
        }
    }

    IQuestSource* questSource() const
    {
        return questSource_.get();
    }
    // Called on the bot thread; refreshes selection on the next tick.
    void setIgnoredQuests(std::set<std::uint32_t> quests);

    // Replaces the behavior set (default: createDefaultBehaviors()).
    void addBehavior(std::unique_ptr<IBotBehavior> behavior)
    {
        engine_.addBehavior(std::move(behavior));
    }

    void tick();
    // Blocking loop on the real clock (run it on its own thread).
    void run(std::stop_token stop);
    // True once the character reached the profile's stopAtLevel and the bot
    // stopped, logging out where the client lets it. tick() does nothing after.
    bool finished() const
    {
        return finished_;
    }

    // Paused because the character reached BotConfig::stopAfterDeaths.
    bool stoppedAfterDeaths() const
    {
        return stoppedAfterDeaths_;
    }

    bool questingComplete() const
    {
        return questingComplete_;
    }

    // Called from tick() whenever the learned obstacles change, for the owner to
    // write them into the profile file.
    void setLearnedObserver(std::function<void(const LearnedObstacles&)> observer)
    {
        learnedObserver_ = std::move(observer);
    }

    // Drops everything learned about obstacles; the observer then sees it empty.
    void forgetLearned()
    {
        obstacles_.forget();
    }

    ObstacleMemory& obstacles()
    {
        return obstacles_;
    }

    void pause()
    {
        pausedByQuesting_ = false;
        engine_.pause();
    }

    void resume()
    {
        if (stoppedAfterDeaths_)
        {
            deathsAtLastLimit_ = context_.stats.deaths;
            stoppedAfterDeaths_ = false;
        }
        pausedByQuesting_ = false;
        engine_.resume();
        findTemporaryVendor_ = true;
    }

    // Keys that make the client take on, and give back, the bindings the bot
    // presses action bar slots with (the BarKeys addon). They are
    // claimed while the bot runs in the world, and released when it pauses,
    // stops or goes away.
    void setClientBindingKeys(KeyChord claim, KeyChord release)
    {
        clientBindingKeys_ = {claim, release};
    }

    // Client setup without the addon: lines typed into the chat once the
    // bot runs in the world, out of combat. All but the /console lines, which
    // the client saves, are typed again every kClientSetupInterval (a /reload
    // drops an override binding).
    void setClientSetupChat(std::vector<std::string> lines)
    {
        clientSetupChat_ = std::move(lines);
    }

    // The line that binds the interact key (interactBindCommand), where the
    // player left the key free. The key is the way in only where the mouse
    // found nothing to right-click, so the line is typed when the key is first
    // pressed, not at every attach - and again kClientSetupInterval later,
    // since a /reload drops the binding.
    void setInteractBindChat(std::string line)
    {
        actions_.setPreparation(std::string(actions::Interact), std::move(line), kClientSetupInterval);
    }

    static constexpr Duration kClientSetupInterval{300000};
    // Commands to set the client up: soft target interact and auto loot.
    static std::vector<std::string> clientSetupCommands();
    // Interact With Target on the interact key, if the player left that key free.
    static std::string interactBindCommand(const KeyChord& interactKey);

    InstanceStatus status() const;

    const std::string& name() const
    {
        return name_;
    }

    Logger& log()
    {
        return log_;
    }

    EventBus& events()
    {
        return events_;
    }

    BotEngine& engine()
    {
        return engine_;
    }

    BotContext& context()
    {
        return context_;
    }

    ActionService& actions()
    {
        return actions_;
    }

    IInputBackend& input()
    {
        return *input_;
    }

    IGameStateProvider& provider()
    {
        return *provider_;
    }

    Navigator& navigator()
    {
        return navigator_;
    }

    void setClientNavigation(std::unique_ptr<INavigationProvider> provider)
    {
        clientNavigation_ = std::move(provider);
    }

    TargetSelector& targets()
    {
        return targets_;
    }

    const std::optional<GliderProfile>& profile() const
    {
        return profile_;
    }

    const WaypointRoute& route() const
    {
        return route_;
    }

    const IClock& clock() const
    {
        return *clock_;
    }

    // The combat class's pull actions, when none of them has a button to
    // press; empty when one has, or the class pulls with nothing.
    std::vector<std::string> pullActionsOffBars() const;

    // The NPC entries the quest data knows to be elite (data/quests/elites.json).
    void setEliteEntries(std::shared_ptr<const std::unordered_set<std::uint32_t>> entries)
    {
        eliteEntries_ = std::move(entries);
        if (questSource_ && eliteEntries_)
        {
            questSource_->setEliteEntries(eliteEntries_);
        }
        refreshTargetFilter();
    }

private:
    void updateStatus();
    void checkStopAtLevel();
    // Under water, and the breath left (BotContext::breath).
    void watchBreath(const GameSnapshot& s);
    void checkStopAfterDeaths();
    void syncClientBindings(bool wanted);

    std::shared_ptr<const std::unordered_set<std::uint32_t>> eliteEntries_;
    std::string name_;
    std::shared_ptr<const IClock> clock_;
    Logger log_;
    EventBus events_;
    std::unique_ptr<IGameStateProvider> provider_;
    std::unique_ptr<IInputBackend> input_;
    ActionService actions_;
    MovementController movement_;
    DirectNavigationProvider directNavigation_;
    std::unique_ptr<INavigationProvider> clientNavigation_;
    std::optional<Vec3> pathfindingOrigin_;
    bool deadAtStartSaid_ = false; // started dead: the origin waits for life
    Navigator navigator_;
    TargetBlacklist targetBlacklist_;
    ObjectBlacklist objectBlacklist_;
    AreaBlacklist areaBlacklist_;
    TargetSelector targets_;
    ObjectiveStack objectives_;
    BotStats stats_;
    BotConfig config_;
    std::optional<GliderProfile> profile_;
    WaypointRoute route_;
    std::unique_ptr<ICombatClass> combatClass_;
    std::unique_ptr<IQuestSource> questSource_;
    std::vector<QuestTarget> questDisplayTargets_;
    std::map<std::uint32_t, std::string> ignoredQuestNames_; // of context_.ignoredQuests, where seen in the log
    void noteIgnoredQuestNames();
    bool questDataMissingSaid_ = false;
    // A profile with no vendor route: look for a vendor in sight on the next tick.
    bool findTemporaryVendor_ = true;
    bool temporaryVendorMissingSaid_ = false;
    void findTemporaryVendor(const GameSnapshot& s);
    TimePoint questTargetsAt_{};
    std::optional<int> questFocusLevel_; // the quest level being worked on (focusQuests())
    bool questFocusKept_ = false;        // said once that a lower quest waits
    // With Pathfinder (focusQuestsByArea()): the group of quests under way,
    // and the one it was last said to be grouped round.
    std::set<std::uint32_t> questUnderWay_;
    MovementTrace movementTrace_; // BotConfig::movementTrace
    bool movementTraceTried_ = false;
    std::uint32_t questLead_ = 0;
    // What was last said about each quest's progress, to log only what changed.
    std::map<std::uint32_t, std::pair<std::vector<std::string>, bool>> questReported_;
    void updateQuestTargets(const GameSnapshot& s);
    // Says what came into the bags since the last snapshot: loot, a herb, a
    // quest's item, a purchase.
    void reportReceivedItems(const GameSnapshot& s);
    // Says when the mob being fought starts a cast, and which spell: what
    // target.isCasting goes by (a Fleshripper was kicked and gouged).
    void reportTargetCasts(const GameSnapshot& s);
    // Says when a mob in the fight with us starts to flee, and stops.
    void reportFleeing(const GameSnapshot& s);
    Guid castTarget_ = 0;
    std::uint32_t castSpell_ = 0;
    TimePoint castSince_{};
    std::string lastActionSeen_;
    std::vector<Guid> fleeing_; // the mobs of our fight seen fleeing
    TimePoint interruptedAt_{}; // when one of ours that stops a cast last went off
    // Follows the mobs above the maximum mob level and tells the target
    // selector the ground they hold.
    void watchStrongMobs(const GameSnapshot& s);
    // A mob we killed back within seconds where it fell (FastRespawn).
    void watchRespawns(const GameSnapshot& s);
    std::unordered_set<Guid> respawnSeen_; // the units in sight last tick
    // Which hostile mobs stand under a roof (a mine, a cave), for the target
    // selector: a pack there is not kept clear of (TargetSelector::packed).
    void markUnderRoof(const GameSnapshot& s);
    std::unordered_map<Guid, std::pair<Vec3, bool>> roofSeen_;
    bool underRoofSaid_ = false;
    // Party system, Damage: the leader's target in a fight becomes ours.
    void assistLeader(const GameSnapshot& s);
    // Party system: the members' party chat lines in, ours out (one every two
    // seconds), and the leader's answer to "lost".
    void partyChat(const GameSnapshot& s);
    // Party system: a vendor trip as a party - asking for it, the queue, a
    // follower's turn at the vendor the leader stands by.
    void partyVendor(const GameSnapshot& s);
    bool partyVendorBegan_ = false;
    TimePoint partySaidAt_{};
    std::vector<std::string> partyLostFrom_; // members the leader owes its position
    std::map<std::uint32_t, int> bagCounts_;
    Guid bagsOf_ = 0; // whose bags bagCounts_ are; 0 before the first look
    Guid handInHeldBackOf_ = 0; // whose failed hand-ins context_.handInHeldBack are
    bool questingPausedSaid_ = false;
    // The engine is paused by questing itself (nothing to work on), not by the
    // user or a limit: setIgnoredQuests() may resume it.
    bool pausedByQuesting_ = false;
    BotContext context_;
    BotEngine engine_;
    std::vector<EventBus::Subscription> subscriptions_;

    bool finished_ = false;
    bool questingComplete_ = false;
    bool stopLevelAnnounced_ = false;
    bool stoppedAfterDeaths_ = false;
    int deathsAtLastLimit_ = 0;
    ObstacleMemory obstacles_;
    std::function<void(const LearnedObstacles&)> learnedObserver_;
    std::uint64_t observedRevision_ = 0;

    PlayerProximityAlert playerAlert_;
    FollowerWatch followerWatch_;
    BuffThanks buffThanks_;
    // Why a hostile mob close by is not pulled, said once for each mob and
    // reason: walking past one otherwise looks like a fault.
    void reportSkippedTargets(const GameSnapshot& s);
    std::unordered_map<Guid, std::string> skipReported_;
    std::optional<TimePoint> skipCheckedAt_;

    std::optional<std::pair<KeyChord, KeyChord>> clientBindingKeys_; // claim, release
    bool clientBindingsClaimed_ = false;
    TimePoint lastClaim_{};
    void runClientSetupChat(bool wanted);
    // Says yes when the client asks whether custom scripts may run, and types
    // the /script lines it dropped meanwhile again.
    void allowChatScripts();
    std::vector<std::string> scriptsToRetype_;
    std::vector<std::string> clientSetupChat_;
    std::optional<TimePoint> clientSetupAt_;

    std::optional<Vec3> generalTravelDest_;
    Vec3 generalTravelStart_{};
    float generalTravelTotalDist_ = 0.0f;

    mutable std::mutex statusMutex_;
    InstanceStatus status_;
};

} // namespace og
