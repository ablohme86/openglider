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
 *  File: BotConfig.h
 *
 *  Desc:
 *  Declarations and interface for BotConfig (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/navigation/RouteCorrector.h"
#include <string>
#include <vector>

namespace og
{

struct GatheringProfessionConfig
{
    bool enabled = false;
    float maxDetourDistance = 80.0f;
    int retryCount = 2;
    Duration blacklistDuration{300000};
    std::vector<std::string> nodes; // Empty accepts all nodes of this profession.
};

struct GatheringConfig
{
    GatheringProfessionConfig mining;
    GatheringProfessionConfig herbalism{false, 100.0f, 2, Duration(300000), {}};
    float interactionDistance = 3.0f;
    float maxVerticalDifference = 8.0f;
    float dangerRadius = 15.0f;
    // Leave a node alone while a hostile stands within dangerRadius of it. Off,
    // the bot goes for it anyway and fights whatever that pulls.
    bool skipHostileNodes = true;
    Duration approachTimeout{20000};
    Duration interactionTimeout{10000};
    Duration lootGrace{1500};
    Duration nodeTimeout{120000};
    // Stopped at the node and facing it: stand this long before the interact, as a
    // person would, so the harvest does not start while the character still slides.
    // Drawn anew from the range each time, so no two waits are alike.
    Duration settleDelayMin{500};
    Duration settleDelayMax{1000};
    int maxHarvestsPerNode = 10; // Classic veins can require multiple interactions.
    // No object interaction through memory (2.5.6): facing the node and pressing
    // the interact key starts the harvest - F11, bound by the bindings addon.
    bool interactKey = false;
};

// Fishing (FishBehavior): timings around one cast.
struct FishingConfig
{
    float arriveRadius = 1.5f;        // how close to a fishing spot to stand
    float bobberSearchRadius = 30.0f; // a bobber farther away is not ours
    Duration bobberTimeout{5000};     // cast -> bobber in the water
    Duration biteTimeout{32000};      // the channel ends before this
    // Bite -> click, like a person would. Not slower: on 1.15.9 the fish got
    // away 0.5-1 s after the bite.
    Duration hookDelayMin{150};
    Duration hookDelayMax{350};
    Duration lootWait{4000}; // click -> catch in the bags, at most
    // The bite is over (the bobber floats again, or is gone) and nothing came:
    // the fish got away. Autoloot puts a catch in the bags well within this.
    Duration escapeWait{1000};
    Duration recastDelayMin{100}; // after a catch
    Duration recastDelayMax{300};
    Duration missRecastDelayMin{100}; // after the fish got away or never bit
    Duration missRecastDelayMax{300};
    Duration lureApplyTime{4000}; // applying a lure is a cast of its own
    int maxFailedCasts = 5;       // casts in a row without a bobber stop fishing
};

// What the bot sells when it visits a vendor, and when it decides to go.
// Quality follows the client's scale: 0 poor/grey, 1 common/white,
// 2 uncommon/green. Anything rarer is never sold.
struct VendorConfig
{
    bool enabled = false;
    // Pathfinder: only the vendors of the zone the character is in - in
    // Westfall none from Elwynn Forest, however near. Where that zone has
    // none (or is not known), the nearest of any zone as before.
    bool onlyInZone = false;
    int freeBagSlotsTrigger = 2; // head to the vendor at or below this many free slots
    bool sellPoor = true;
    bool sellCommon = false;
    bool sellUncommon = false;
    // Sell greys ignoring `neverSell`, but still protect profession materials and tools.
    bool sellAllPoor = false;
    // Whole categories, whatever their quality: raw cooking ingredients
    // (meat, eggs, raw fish), skinning materials (leather, hides, scales) and
    // herbs. They win over keeping what a profession gathers.
    bool sellCooking = false;
    bool sellSkinning = false;
    bool sellHerbalism = false;
    bool sellsCategory() const
    {
        return sellCooking || sellSkinning || sellHerbalism;
    }

    bool repair = true;
    // When gear sends the bot to repair: once an equipped item shows yellow
    // on the armour figure (low durability), or only once one is red (broken).
    enum class RepairAt
    {
        Yellow,
        Red
    };
    RepairAt repairAt = RepairAt::Red;
    // Item names. `neverSell` wins over everything else, including `alwaysSell`
    // - but not over `sellAllPoor` for grey items.
    std::vector<std::string> neverSell;
    std::vector<std::string> alwaysSell;

    float searchRadius = 40.0f;    // look for the vendor NPC within this of the route's end
    // Budget for one leg of the trip (out and sell, then the walk home, which
    // gets its own). Time spent fighting does not count, so this is pure
    // travel. Overrunning it out on the route only sends the bot home early;
    // overrunning it on the way home gives up where it stands.
    Duration travelTimeout{300000};
    Duration stageTimeout{15000}; // give up on one step (open, sell, repair)
    Duration sellSettle{1500};    // let the client apply the sale before checking bags
    // After a trip that freed nothing (no vendor, nothing sellable, a sale that
    // did not take) wait this long before trying again, so a full-but-unsellable
    // bag does not send the bot back and forth forever.
    Duration retryDelay{600000};
    // No Lua (2.5.6): the interact key opens the merchant, and the bindings
    // addon repairs all and sells the grey items when it does. The bot waits
    // up to merchantAddonWait for that to show before walking back.
    bool merchantAddon = false;
    Duration merchantAddonWait{4000};

    // Nothing would be sold: every quality is off and no explicit list.
    bool sellsNothing() const
    {
        return !sellPoor && !sellCommon && !sellUncommon && !sellAllPoor && alwaysSell.empty() && !sellsCategory();
    }
};

// Occasional off-pattern movement while travelling a route, so the character
// does not walk every lap in exactly the same metronomic way. Each kind fires
// on its own randomised timer; only one is ever active at a time.
struct HumanizerConfig
{
    bool enabled = false;
    bool jumps = true;
    bool strafes = true;
    bool pauses = true;

    // Wait between two events of the same kind; a value is drawn from the range.
    Duration jumpMinInterval{20000};
    Duration jumpMaxInterval{90000};
    Duration strafeMinInterval{45000};
    Duration strafeMaxInterval{180000};
    Duration pauseMinInterval{120000};
    Duration pauseMaxInterval{420000};

    // How long one event lasts (a jump is instant and has no duration).
    Duration strafeMinDuration{200};
    Duration strafeMaxDuration{600};
    Duration pauseMinDuration{800};
    Duration pauseMaxDuration{3000};

    // A jump is kept for a stretch of open running: with a corner of the way
    // nearer than this it waits a few seconds and tries again.
    float jumpClearAhead = 15.0f;
    // Away from the keys now and then: a long stop, far apart.
    bool breaks = true;
    Duration breakMinInterval{1500000}; // 25 minutes
    Duration breakMaxInterval{4200000}; // 70 minutes
    Duration breakMinDuration{15000};
    Duration breakMaxDuration{75000};
    // With Pathfinder: a long straight leg of the way sways up to this many
    // yards to one side (Navigator::setSway). 0: straight.
    float sway = 6.0f;
};

// Roaming inside the grind route instead of walking its waypoints in order.
struct RoamConfig
{
    bool enabled = false;
    float minLeg = 15.0f; // yards to the next spot, at the least
    // Never pull a mob standing outside the area roamed in. Self-defence is
    // unaffected.
    bool pullInsideOnly = true;
    // "Build Route" (experimental): instead of one spot at a time, walk a whole
    // circuit built through the area (buildCircuit), and a new one every lap.
    bool circuit = false;
    // Roam this zone: only in this zone (0: wherever the area reaches), as far
    // as the map tells which zone a spot lies in.
    std::uint32_t zone = 0;
};

// Watching for other players coming near: for a warning in the log, and for
// the GUI's sound and radar flash, which work without the log warning.
struct PlayerAlertConfig
{
    bool enabled = false;
    bool logWarnings = true;
    float radius = 40.0f;
    // A player still within the radius is announced again this long after the
    // last warning; one stepping in and out on the edge sooner is not.
    Duration renotifyAfter{60000};
};

// Stop when another player follows us: one that stays within `radius` for
// `followTime` while both of us travel at least `minTravel` is following, not
// standing by or passing. The bot then stands (still fighting back) until
// they are `clearDistance` away or out of sight for `lostTimeout`.
struct FollowerConfig
{
    bool enabled = false;
    float radius = 50.0f;
    Duration followTime{30000};
    float minTravel = 20.0f;
    float clearDistance = 100.0f;
    Duration lostTimeout{10000};
};

// A chat command (an emote, say) for another player who buffs us, as a person
// would thank them: once per player per `perPlayer`, a moment after the buff.
// "{name}" in the command stands for the player's name.
struct BuffThanksConfig
{
    bool enabled = false;
    std::string command = "/bow {name}";
    Duration perPlayer{60000};
    Duration delayMin{1000};
    Duration delayMax{3000};
};

// Tunables for one bot instance. Values are in yards / engine time.
// Questing (experimental): do the quests in the log - pull only the mobs they
// need and walk to where those stand, inside the profile's area - and hand
// them in once done.
struct QuestConfig
{
    bool enabled = false;
    // Which reward to take where a quest offers a choice: the one worth most,
    // or gear the character can use (the one worth most of those), falling
    // back to the one worth most where none fits.
    enum class Reward
    {
        HighestValue,
        GearForCharacter
    };
    Reward reward = Reward::HighestValue;
};

// Party system (docs/PartySystem.md): bots on several clients play as one
// group. The leader runs the profile and waits for the others; a follower
// follows the leader and pulls nothing of its own.
struct PartyConfig
{
    bool enabled = false;
    bool leader = false;
    enum class Role
    {
        Damage,
        Healer
    };
    Role role = Role::Damage;
    std::string leaderName;           // a follower's leader
    std::vector<std::string> members; // the other members, up to four
    // The leader waits for a member farther than this, out of combat.
    float partyRange = 40.0f;
    // How far behind the leader a follower keeps: near for Damage, further
    // back for a healer, out of the mobs' way.
    float damageFollow = 5.0f;
    float healerFollow = 20.0f;

    bool follower() const
    {
        return enabled && !leader && !leaderName.empty();
    }

    bool leads() const
    {
        return enabled && leader;
    }
};

struct BotConfig
{
    Duration tickInterval{10};
    // A CSV file the session's movement is written to (MovementTrace), the
    // bot's and - attached and paused - the player's own; empty: none.
    std::string movementTrace;
    // A corpse or node not looted, skinned or gathered this long after the
    // mouse first looked for it: the character turns round once to find it
    // (BotContext::seekTurningRound).
    Duration turnRoundAfter{5000};
    GatheringConfig gathering;
    HumanizerConfig humanizer;
    VendorConfig vendor;
    // How close to stand to talk to an NPC (a vendor, a quest giver) or to
    // loot a corpse.
    float interactDistance = 2.5f;
    FishingConfig fishing;
    PlayerAlertConfig playerAlert;
    FollowerConfig follower;
    BuffThanksConfig buffThanks;
    RoamConfig roam;
    QuestConfig quest;
    PartyConfig party;

    // Navigation
    bool usePathfinding = false;
    // Pathfinder: long ways keep to the roads.
    bool useRoads = true;
    float pathfindingRoamRadius = 100.0f;
    float waypointRadius = 3.0f;
    // Remember the way round an obstacle the bot works itself free of, so the
    // next lap avoids it. Applied to the running route only; writing it back to
    // the profile is the user's call.
    RouteCorrectorConfig routeCorrector;

    // Targeting / pulling
    int tabAttempts = 8; // Target.Next presses before giving up on a target
    Duration tabInterval{350};
    Duration approachTimeout{30000}; // max time to reach pull range
    Duration pullTimeout{15000};     // max time in range without the mob engaging
    Duration blacklistDuration{120000};
    // Never go looking for a fight with a mob that gives no experience. Applies
    // to pulls only; the bot always defends itself.
    bool skipGrayMobs = false;
    // General Settings: pull elite mobs too (the profile's attackElites does as well).
    bool pullElites = false;
    // Avoid packed mobs (General Settings): more hostile mobs than this about
    // a mob we are after and it, and they, are blacklisted. 0 is off.
    int maxPackAdds = 0;
    // How near each other they stand to count (TargetFilterConfig::addRadius).
    float packRadius = 12.0f;
    // A hostile mob this near ahead on the way is fought before it comes for
    // us (TargetFilterConfig::protectRange).
    float protectRange = 15.0f;
    // How far off the direction of travel it may be, in degrees
    // (TargetFilterConfig::protectAngle).
    float protectAngle = 90.0f;
    // The way keeps this far outside the reach of a pack's mobs.
    float packMargin = 5.0f;
    // Pause after this many deaths; Resume after the limit starts a new count.
    // 0: never.
    int stopAfterDeaths = 0;
    // Passive mode: never actively pull or tag mobs; only defend when attacked.
    bool passiveMode = false;
    // In a fight, step away from a hostile mob that stands or walks to where
    // it would notice the character and join (double aggro): followed and
    // stepped away from as the mobs too strong to fight are, below.
    bool addAvoidance = true;
    float addAvoidanceMargin = 0.0f; // extra yards added to mob aggro radius for avoidance
    // How near a mob at the character's own level notices it - a yard more or
    // less for each level between them. 20 in Classic as the emulators have
    // it; WoW Forever's mobs notice from 10 (measured), set on attaching.
    float aggroRadiusBase = 20.0f;
    // On the way somewhere, the way goes round every hostile mob not to be
    // pulled - a quest's mobs are not gone round - by its aggro range and the
    // pack margin. Where there is no way round (a cave, a house), the one in
    // the way is pulled (ClearPath). Off: only the mobs too strong to fight,
    // packs, and in Quest Mode the other mobs near by, 11 yards round each.
    bool avoidMobsTravelling = true;
    // Take a mob to pull with a click on it, where the client says what is
    // under the mouse; off (or where that finds nothing): Tab only.
    bool clickToSelect = false;
    // Walk round a hostile mob above the target filter's maximum level, as far
    // off as it would come for us (20 yd at our level, a yard more for each
    // level above ours, 10-45 yd) and this much more, while it is in sight.
    // Mobs above the maximum mob level are kept away from: walked round at
    // their aggro range plus strongMobMargin, and in a fight stepped away
    // from when one stands or comes that near (CombatBehavior::dodgeStrongMob).
    bool avoidStrongMobs = true;
    float strongMobMargin = 10.0f;

    // Train Abilities (General Settings): on reaching an even level, a walk
    // to the nearest trainer of the character's class to learn what it
    // teaches now.
    bool trainAbilities = true;
    // Learn Flight Paths (General Settings): a flight master within 250 yd with a
    // flight path not learnt (the green "!") talked to on the way, so the
    // character knows it.
    bool learnFlightPaths = true;
    // Use Flight Paths (General Settings): a far Go To flown where a flight
    // path learnt saves a fair part of the way.
    bool useFlightPaths = true;
    // Auto Equip New Items (General Settings): gear that comes into the bags -
    // loot, a quest's reward - put on when it is better for the class than
    // what is worn there (gearRank), with the bags opened to read it and a
    // right-click on it. `autoEquipSkipBoE`: never a Bind on Equip piece.
    bool autoEquip = false;
    bool autoEquipSkipBoE = true;
    // Sit down at campfires (General Settings, WoW Forever): to a campfire in
    // sight, sat by it until its Welcoming Campfire minute gives Camp Benefits.
    bool sitAtCampfires = false;

    // Looting
    bool loot = true;               // General Settings; off for a fishing session without a profile
    bool skin = false;              // General Settings
    float lootSearchRadius = 20.0f; // extra flagged corpses to collect during a loot session
    int lootAttempts = 3;
    // A right-click on the corpse or node under the mouse that did nothing
    // this many times is made again with the camera all the way in, where the
    // character does not stand between the mouse and it. 0: never.
    int zoomInAfterClicks = 2;
    Duration cameraZoomSettle{1000}; // the camera glides in: no click before it is there
    // Try up to lootAttempts with every corpse of ours even when it is not
    // flagged lootable, and count it looted only if loot arrives. For clients whose
    // lootable flag cannot be trusted (2.5.6).
    bool lootWithoutFlag = false;
    // How long loot has to arrive after each unflagged interact: on 2.5.6 items came
    // 1.0-1.9 s after the kill.
    Duration lootArrivalWait{3000};
    // 2.5.6 flags a corpse lootable only once the fight is over, and an interact
    // before that finds nothing: the unflagged interact waits this long after it.
    Duration lootAfterFightDelay{700};
    int skinAttempts = 3;
    // The skinnable flag goes up only once the corpse's loot is gone. Not so on
    // the newer clients, where it is up from the kill: there another player's
    // kill is tried once - its owner may not have looted it yet.
    bool skinFlagTrusted = true;
    Duration lootInteractInterval{1500};
    // Before the first skin attempt (after looting) and between two; drawn anew
    // from the range each time.
    Duration skinIntervalMin{2000};
    Duration skinIntervalMax{3200};
    // Stood at a corpse and facing it: wait this long before the first loot,
    // drawn anew each time. Clicking the instant it arrives, every time, is not
    // how a person loots.
    Duration lootDelayMin{150};
    Duration lootDelayMax{600};
    Duration lootMoveTimeout{20000};
    Duration lootGrace{4000}; // wait this long for the lootable flag after a kill
    // Leave a corpse alone while a hostile stands this close to it (Glider's
    // LootCheckHostiles/LootCheckDistance); the pull clears it first. 0 = off.
    float lootDangerRadius = 15.0f;
    // A corpse can turn skinnable only after its loot window closes: wait this
    // long after looting before deciding there is nothing to skin.
    Duration skinFlagWait{2000};

    // Resting
    Duration restTimeout{90000};
    // A food/drink buff missing this long after using the item means the press
    // did not take; the bot drops the item's cooldown and uses it again.
    Duration restRetryInterval{3000};
    // The rest script runs one rule per tick, so eating and drinking land in
    // consecutive ticks - 50 ms apart. The client quietly drops the second item
    // use that close together, which is why drinking sometimes never happened.
    Duration restConsumableGap{600};

    // Death / corpse run
    Duration releaseDelay{1500};
    Duration releaseRetry{5000};
    float resurrectRange = 25.0f; // retrieve corpse within this distance (WoW allows ~39)
    float corpseSafeRadius = 20.0f;
    Duration safeSpotTimeout{10000}; // to get to a spot clear of hostiles, before another is tried
    // How long the ghost waits, moving about within resurrect range, for a
    // spot with no hostile within corpseSafeRadius before it resurrects
    // anyway (and runs, ResurrectEscape).
    Duration corpseCampedTimeout{90000};
    // The run after resurrecting next to hostiles, a new way taken while the
    // place reached is not clear of them; then rested wherever it got to.
    Duration resurrectEscapeTimeout{45000};
    Duration retrieveRetry{3000};
};

} // namespace og
