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
 *  File: BotContext.h
 *
 *  Desc:
 *  Declarations and interface for BotContext (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/actions/IActionService.h"
#include "og/combat/ICombatClass.h"
#include "og/common/Blacklist.h"
#include "og/common/EventBus.h"
#include "og/common/Log.h"
#include "og/core/BotConfig.h"
#include "og/core/BotStats.h"
#include "og/core/BuffTracker.h"
#include "og/core/ObjectiveStack.h"
#include "og/core/QuestTargets.h"
#include "og/core/PartyChat.h"
#include "og/core/PartyWatch.h"
#include "og/core/StrongMobWatch.h"
#include "og/input/IInputBackend.h"
#include "og/navigation/Navigator.h"
#include "og/navigation/WaypointRoute.h"
#include "og/profiles/GliderProfile.h"
#include "og/targets/TargetSelector.h"
#include "og/world/TaxiNetwork.h"

#include <memory>

namespace og
{

class IGameStateProvider;

struct PendingLoot
{
    Guid guid = 0;
    Vec3 position;
    TimePoint addedAt{};
    bool inProgress = false; // LootBehavior is working on it; only it may drop the entry
    // Where the wait for the lootable flag starts: the kill, pushed on while we
    // are still fighting - the 2.5.6 client flags a corpse only once combat ends.
    TimePoint graceFrom{};
    // Another player's kill: its loot is theirs, but its skin is anyone's once
    // the corpse is flagged skinnable.
    bool skinOnly = false;
};

// Runtime combat state shared between behaviors.
struct CombatState
{
    Guid engaged = 0; // the unit we decided to fight (pull target or chosen attacker)
    bool inFight = false;
    TimePoint fightStarted{};
    // Set while the bot is travelling with a purpose (a vendor trip). It still
    // fights back when attacked, but must not start a fight or chase anything.
    bool defendOnly = false;
    // A hostile mob in the way that would aggro as we pass (ClearPathBehavior):
    // fought even while defendOnly, before it comes for us.
    Guid clearing = 0;
};

// A mob we killed, for a while after: one of its kind showing up there again
// at once is a respawn (FastRespawn).
struct RecentKill
{
    Guid guid = 0;
    std::uint32_t entry = 0;
    std::string name;
    Vec3 at;
    TimePoint when{};
};

// Where a mob comes back within seconds of its kill (Garrick Padfoot by his
// camp in Elwynn Forest): what is needed is looted, and the bot leaves, rather
// than killing it over and over. Gone round and not pulled at until `until`,
// unless a quest still needs that mob.
struct FastRespawn
{
    Vec3 at;
    std::uint32_t entry = 0;
    std::string name;
    TimePoint until{};
    static constexpr float kRadius = 30.0f;
};

// Planned escape after resurrecting near hostiles.
struct ResurrectEscape
{
    Vec3 destination;
    Vec3 origin;
    TimePoint startedAt{};
    int replans = 0; // new destinations chosen on the way, the first one not clear
};

// Everything a behavior can use. Owned and wired by BotInstance; behaviors
// never talk to the game or the input backend directly.
struct BotContext
{
    const IClock& clock;
    Logger& log;
    EventBus& events;
    IActionService& actions;
    MovementController& movement;
    Navigator& navigator;
    TargetSelector& targets;
    TargetBlacklist& targetBlacklist;
    ObjectBlacklist& objectBlacklist;
    AreaBlacklist& areaBlacklist;
    ObjectiveStack& objectives;
    BotStats& stats;
    BotConfig& config;
    IGameStateProvider* provider = nullptr;

    ICombatClass* combatClass = nullptr;
    SpellLookup spellLookup; // set for the live game; see CombatContext
    const GliderProfile* profile = nullptr;
    WaypointRoute* route = nullptr;
    // What the bot has learned about the obstacles on this profile's routes.
    ObstacleMemory* obstacles = nullptr;

    CombatState combat;
    BuffTracker buffTracker;
    // The mobs above the maximum mob level in sight: where they are and walk.
    StrongMobWatch strongMobs;
    // Every hostile mob in sight, followed the same way, while Add Avoidance
    // is on: the ones that would join a fight (CombatBehavior::dodgeMobs).
    StrongMobWatch nearMobs;
    // Avoid packed mobs: round each mob of a pack in sight, its reach plus
    // packMargin - ground the way keeps off (BotInstance::watchStrongMobs).
    std::vector<AvoidZone> packZones;
    // The party members (Party system): where they are, from what the client sees.
    PartyWatch party;
    // What the party says in party chat: the lines (party_messages.txt), the
    // ones heard this tick from the members, and the ones to say, sent one
    // every two seconds (BotInstance).
    PartyChat partyChat;
    struct PartyLine
    {
        std::string from;
        PartyChat::Heard heard;
    };
    std::vector<PartyLine> partyHeard;
    std::vector<std::string> partyOutbox;
    // Where the leader said it is, answering "lost": walked to while it is out of sight.
    std::optional<Vec3> partyRegroup;
    // A vendor trip as a party (BotInstance::partyVendor): the members that
    // said "vendor", in the order they said it, and when; whether we said it;
    // whether it is our turn at the vendor (a follower's vendor trip runs only
    // then); and for the leader, a trip asked for by a member.
    struct PartyVendorAsk
    {
        std::string name;
        TimePoint at;
    };
    std::vector<PartyVendorAsk> partyVendorQueue;
    bool partyVendorAsked = false;
    bool partyVendorTurn = false;
    bool partyVendorForMember = false;
    // Train Abilities with no money: a vendor trip first, as if the bags
    // were full, to have something to pay the trainer with. Cleared when the
    // trip is over (VendorBehavior::finish).
    bool vendorBeforeTraining = false;
    void partySay(PartyChat::Kind kind, const Vec3& place = {})
    {
        if (std::string line = partyChat.say(kind, partyRandom, place); !line.empty())
        {
            partyOutbox.push_back(std::move(line));
        }
    }
    std::mt19937 partyRandom{std::random_device{}()};
    // Spells mobs cast without a cast time: seen to end too soon, with nothing
    // of ours interrupting them (BotInstance::reportTargetCasts). Greater
    // Fleshripper's 12166 to begin with.
    std::set<std::uint32_t> instantSpells{12166};
    // A step the combat script asked for (player.WalkBackward(10)), for the
    // fight to take; mutable, being asked for through a const context.
    mutable std::optional<ScriptMove> scriptMove;
    // Profile navigation is active, including navigation inside a grind plan.
    bool navigating = false;
    // On the way to a quest NPC, not yet talking to it: herbs, veins and
    // corpses by the way are still taken.
    bool questTravel = false;
    // A behaviour that cannot go on asks the whole engine to pause (why, for
    // the log): a quest hand-in out of time stood there jumping otherwise.
    std::optional<std::string> pauseRequest;
    // Build Route: the circuit being walked and the index of the next point.
    // Gathering waits for a node the rest of the lap passes by, rather than
    // heading straight off to it. Null when no circuit is walked.
    const std::vector<Vec3>* circuitAhead = nullptr;
    std::size_t circuitAheadFrom = 0;
    std::vector<PendingLoot> pendingLoot;
    std::optional<Vec3> lastDeathPosition;
    std::optional<ResurrectEscape> resurrectEscape;
    std::vector<RecentKill> recentKills;
    std::vector<FastRespawn> fastRespawns;
    // An unfinished quest objective has the character kill or loot this mob.
    bool questNeeds(std::uint32_t entry) const;
    // The fast respawn place `at` lies in, for that mob (0: any); nullptr when none.
    const FastRespawn* fastRespawnAt(const Vec3& at, std::uint32_t entry = 0) const;
    // standUp(): when the character was first asked to stand, and when last.
    std::optional<TimePoint> standingUpSince;
    TimePoint lastStandUp{};

    TimePoint now() const
    {
        return clock.now();
    }

    // A wait drawn evenly from [low, high], for waits that should not all be alike.
    // With the humanizer on (config.humanizer.enabled) it is drawn as a person's
    // waits fall: most of them short, in the lower part of the range, a few
    // long, and now and then one past `high` - looking away for a moment -
    // all of it at the session's own pace (humanPace()).
    Duration randomBetween(Duration low, Duration high);
    // The generator behind it (xorshift64*); the GUI seeds it per session.
    std::uint64_t randomState = 0x9E3779B97F4A7C15ull;
    // How near to go to something to use it - a corpse, a node, an NPC - of
    // `range`, the farthest that will do. With the humanizer on, somewhere
    // between 60% of it and all of it, the same every time for one `key` (its
    // GUID) and another for the next: not the same step short of every corpse.
    float humanReach(float range, std::uint64_t key);
    // This session's pace, 0.85 (brisk) to 1.3 (unhurried): no two sessions wait alike.
    double humanPace();
    // What the session's habits are drawn from; 0 until first asked for.
    std::uint64_t humanSeed = 0;
    double combatSeconds() const
    {
        return combat.inFight ? toSeconds(clock.now() - combat.fightStarted) : 0.0;
    }

    CombatContext combatContext(const GameSnapshot& snapshot) const
    {
        return CombatContext{snapshot, actions, log, combatSeconds(), spellLookup ? &spellLookup : nullptr, &instantSpells, &scriptMove};
    }

    // How far ahead a mob on the way is looked for (TargetSelector::
    // threatInTheWay): as far as the class pulls from, its fighting range at
    // the most - a hunter takes it on from 30 yards, not from the 15 of the
    // protect range. Hunter's Mark's 100 yards are no pull.
    float protectLookAhead(const GameSnapshot& snapshot)
    {
        if (!combatClass)
        {
            return 0.0f;
        }

        // The script's combatRange: worked out from the combat rules, Hunter's
        // Mark made it 100 yards, and a mob 92 yards off was "in the way".
        auto cc = combatContext(snapshot);
        return std::min(combatClass->pullReach(cc), combatClass->settings().combatRange);
    }

    // The way ahead as planned, from the character on (threatInTheWay).
    std::vector<Vec3> wayAhead(const GameSnapshot& snapshot) const
    {
        std::vector<Vec3> way{snapshot.player.position};
        const Path& path = navigator.path();

        for (std::size_t i = navigator.pathIndex(); i < path.points.size(); ++i)
        {
            way.push_back(path.points[i]);
        }

        return way.size() > 1 ? way : std::vector<Vec3>{};
    }

    // Avoid all mobs during travels: the way, where it goes round them, says
    // which mob is in the way; without it the line of travel does.
    const UnitState* threatInTheWay(const GameSnapshot& snapshot)
    {
        const auto way = config.avoidMobsTravelling ? wayAhead(snapshot) : std::vector<Vec3>{};
        // Going round mobs, but with no way planned this moment (planned
        // again, or left between behaviours): only what is near counts. The
        // line of travel, as far as the class pulls, took a mob the way went
        // round to be in the way.
        const float lookAhead = config.avoidMobsTravelling && way.empty() ? 0.0f : protectLookAhead(snapshot);
        return targets.threatInTheWay(snapshot, navigator.travelHeading(snapshot.player), lookAhead, way);
    }

    bool lootEnabled() const
    {
        return config.loot;
    }

    bool skinEnabled() const
    {
        return config.skin;
    }

    // Fishing mode: the profile's, or switched on for the session (Start Fishing).
    bool fishingMode = false;
    // Why FishBehavior is not fishing right now; empty while it can.
    std::string fishingProblem;
    // A cast is out, from pressing Fishing until the catch is in: nothing but
    // a fight takes over then (Rest would take the channel for a cast of its own).
    bool fishingBusy = false;
    // A player found following us: the bot stands until they are gone, and
    // only fights back meanwhile. 0 when nobody is.
    Guid followerHold = 0;
    // A vendor trip is under way (VendorBehavior), fights on the way included.
    // Players are not watched for following then: the vendors stand in towns.
    bool vendorTrip = false;
    // Quest Mode: the quests in the log to kill and loot for, with where their
    // mobs stand; empty when it is off or nothing is left to do.
    std::vector<QuestTarget> questTargets;
    std::set<std::uint32_t> ignoredQuests;
    // Their names, as the quest log had them: a quest giver's offer is known
    // by its title only, and one turned off is not taken again.
    std::set<std::string> ignoredQuestNames;
    // Quests whose hand-in failed, until when they are left alone: out of the
    // choice of the quests to work on meanwhile (focusQuests), or one done in
    // the data but not in the game held every other quest up.
    std::map<std::uint32_t, TimePoint> handInHeldBack;
    // Under water (BotInstance::watchBreath): the surface over the character,
    // since when its head has been under, and the seconds of breath left;
    // when it last had to come up for air (SurfaceBehavior).
    struct Breath
    {
        std::optional<float> surface;
        std::optional<TimePoint> since;
        std::optional<float> left;
        std::optional<TimePoint> surfacedAt;
    };
    Breath breath;
    IQuestSource* questSource = nullptr; // Questing's data, where there is any
    // The flight paths, from the client's tables, where they could be read.
    std::shared_ptr<const TaxiNetwork> taxiNetwork;
    const TaxiNetwork* network() const
    {
        return taxiNetwork.get();
    }
    TimePoint questPullAfter{};          // let kill credit reach the quest log before the next pull
    // A profile with no vendor route: the vendor found in sight at Resume, clear
    // of No Go zones and obstacles, walked to straight for the vendor trips.
    struct TemporaryVendor
    {
        std::string name;
        Vec3 position;
        bool repairs = false;
        std::uint32_t entry = 0;
    };
    std::optional<TemporaryVendor> temporaryVendor;
    // Keep a lure on the pole this session (the Fishing window), on top of the profile's lureMinutes.
    bool fishingLure = false;
    // A party follower: follows its leader, pulls and travels on its own nothing.
    bool partyFollower() const
    {
        return config.party.follower();
    }

    bool fishingEnabled() const
    {
        return fishingMode || (profile && profile->options.fish);
    }

    // Health/mana below the combat class' rest thresholds.
    bool needsRest(const GameSnapshot& snapshot) const;
    // Eating and drinking sit the character down, and no spell is cast sitting
    // ("You must be standing to do that"). Asks the client to stand and returns
    // true while that is under way, so the caller waits; false once standing -
    // or after a few seconds, so a stand state that never clears cannot hold
    // the bot up.
    bool standUp(const GameSnapshot& snapshot);
    // Resting thresholds reached.
    bool restComplete(const GameSnapshot& snapshot) const;
    // A rest is under way (RestBehavior) and not yet up to where it ends.
    bool resting = false;
    // In the middle of one that can go on here: what comes before resting but
    // only waits for the rest to begin (a herb, a quest's object) waits for
    // it to end too. Past the health the rest starts at, the bot got up after
    // a few seconds of eating and went for the node, and the mobs round it.
    bool midRest(const GameSnapshot& snapshot) const
    {
        return resting && !restComplete(snapshot) && isSafeToRest(snapshot);
    }
    // No aggro: no hostile unit fighting the character or coming for it.
    // Mobs standing about do not count.
    bool isSafeToRest(const GameSnapshot& snapshot) const;
    // Avoid packed mobs: when `unit` has more hostile mobs about it than the
    // setting allows, blacklists it and them for five minutes and says so.
    bool avoidPacked(const GameSnapshot& snapshot, const UnitState& unit);
    // The way planned to `unit` goes through ground kept clear of - a pack's
    // reach, or a mob too strong to fight - where no way round it was found
    // (a mob in a house whose door the pack stands by): that zone, else none.
    std::optional<AvoidZone> wayThroughDanger(const GameSnapshot& snapshot, const UnitState& unit) const;
    // The ground kept clear of that holds `point` - a pack's reach, or a mob
    // too strong to fight - else none.
    std::optional<AvoidZone> dangerAt(const Vec3& point) const;
    // Avoid packed mobs: more hostile mobs than maxPackAdds, not in a fight,
    // that would notice the character where the fight with `unit` is going to
    // be - where the bot stands to pull it, combatRange off it on the way from
    // here - and join in (chain aggro). The nearest of them; nullptr when not.
    const UnitState* joinsTheFight(const GameSnapshot& snapshot, const UnitState& unit, float combatRange) const;
    // A hostile stands within lootDangerRadius of the corpse, free to come
    // for someone looting it (not busy fighting another player).
    bool corpseGuarded(const GameSnapshot& snapshot, const Vec3& corpse) const;
    // That hostile, the nearest to the corpse; nullptr when it is not guarded.
    const UnitState* guardOf(const GameSnapshot& snapshot, const Vec3& corpse) const;
    // Skinning is enabled and the character's skill can skin `unit`, as far
    // as the snapshot tells (unknown skills never block it).
    bool canSkin(const UnitState& unit, const PlayerState& player) const;

    void blacklistTarget(Guid guid, std::string_view name, const std::string& reason, std::optional<Duration> duration = std::nullopt);

    bool setTarget(Guid guid);
    // Clicks on a unit using screen projection (World to Screen) and mouse input.
    bool clickTarget(const UnitState& unit, MouseButton button = MouseButton::Left);
    // Whether the client shows the UI frame with this global name; false where
    // its UI cannot be read (see IGameStateProvider::uiFrame).
    bool uiFrameVisible(std::string_view name);
    // Clicks the middle of a UI frame the client shows, the way a player
    // would: no Lua and no addon. False when it is not showing, or the UI or
    // the window size cannot be read.
    bool clickUiFrame(std::string_view name, MouseButton button = MouseButton::Left);
    // A FontString's text in a frame the client has; nullopt where the UI
    // cannot be read.
    std::optional<std::string> uiText(std::string_view frame, std::string_view fontString);
    // Clicks the first button of the dialog (StaticPopup1-4) the client shows
    // whose text holds `textHolds`, or whose first button reads `buttonReads`
    // (either may be empty; case does not matter). The texts are the English
    // client's. Never any other dialog: a group invite or a trade is left alone.
    bool clickDialog(std::string_view textHolds, std::string_view buttonReads = {});
    // Moves the mouse until the unit or object is under it, one spot a tick
    // from the middle of the window out, as old Glider found the bobber: for a
    // client whose target cannot be set, a corpse or a herb is then
    // right-clicked. Unsupported where the client does not say what is under
    // the mouse; NotFound once every spot was tried.
    enum class MouseSeek
    {
        Unsupported,
        Searching,
        Found,
        NotFound
    };
    // `spots` (fractions of the window, tried in order) replaces the sweep
    // from the middle out: a bobber is smaller than its steps.
    using SeekSpots = std::vector<std::pair<float, float>>;
    SeekSpots buildTargetSeekSpots(const PlayerState& player, const Vec3& targetPos, float zOffset = 0.15f) const;
    MouseSeek seekUnderMouse(Guid guid, const SeekSpots* spots = nullptr);
    MouseSeek seekUnderMouse(Guid guid, const PlayerState& player, const Vec3& targetPos, float zOffset = 0.15f);
    void stopMouseSeek()
    {
        seekGuid_ = 0;
        targetSeekSpots_.clear();
    }

    Guid seekGuid_ = 0;
    std::size_t seekIndex_ = 0;
    SeekSpots targetSeekSpots_;
    // seekUnderMouse for a corpse or a node, and when that has not got it
    // looted, skinned or gathered within turnRoundAfter of the first look:
    // the character turns round on the spot, an eighth of a turn at a time,
    // and the mouse looks again about it at each - what hides it from where
    // the camera stood (a tree, a slope, another corpse) is no longer in the
    // way. Once round, and never again for the same one: NotFound.
    MouseSeek seekTurningRound(const PlayerState& player, Guid guid, std::optional<Vec3> targetPos = std::nullopt, float zOffset = 0.15f);
    // The click took - a cast under way, the loot in: the wait before turning
    // round runs from here again (the corpse still to skin, a node's second
    // harvest).
    void seekTook(Guid guid)
    {
        if (guid != 0 && guid == turnGuid_ && turnStep_ == 0)
        {
            turnFirstSeek_ = now();
        }
    }
    // While it turns round for `guid`: the caller leaves the facing alone.
    bool turningRound(Guid guid) const
    {
        return guid != 0 && guid == turnGuid_ && turnStep_ > 0;
    }

    Guid turnGuid_ = 0;
    TimePoint turnFirstSeek_{};
    TimePoint turnLastSeek_{};
    TimePoint turnStepSince_{};
    float turnFrom_ = 0.0f;
    int turnStep_ = 0; // 0: not turning yet; 1-7: the eighth faced; 8: been round
    std::optional<Vec3> turnTargetPos_;
    float turnZOffset_ = 0.15f;
    SeekSpots turnSpotsCache_;
    // The camera all the way in for a right-click under the mouse that keeps
    // doing nothing: asked before each click with the clicks already made at
    // the corpse or node. True while the camera is on its way in - the caller
    // waits. Only where the client says what is under the mouse.
    bool zoomInForClick(int clicksMade);
    // The camera back out to where zoomInForClick() found it: when the corpse
    // or node is done with, or given up on.
    void restoreCamera();

    bool cameraZoomedIn_ = false;
    TimePoint cameraSettledAt_{};
    bool interactUnit(Guid guid, const Vec3& position);
    // Talks to a living NPC (opens its vendor/quest window).
    bool interactNpc(Guid guid, const Vec3& position);
    // Clears a pending click-to-move interact so it stops steering the player.
    void cancelInteraction();

    // Lua in the client through providers that support it.
    bool canRunScripts() const;
    // The client can take a script now (the provider has none queued).
    bool scriptIdle() const;
    bool runScript(std::string_view lua);
};

} // namespace og
