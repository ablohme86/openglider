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
 *  File: TargetSelector.h
 *
 *  Desc:
 *  Declarations and interface for TargetSelector (target filtering,
 *  scoring, and priority selection).
 *
 *
 */
#pragma once
#include <unordered_set>
#include <memory>

#include "og/common/Blacklist.h"
#include "og/state/Difficulty.h"
#include "og/state/GameSnapshot.h"

#include <optional>
#include <span>
#include <functional>
#include <string>
#include <vector>

namespace og
{

using TargetBlacklist = Blacklist<Guid>;
using NodeBlacklist = Blacklist<Guid>;
using ObjectBlacklist = Blacklist<Guid>;

struct TargetFilterConfig
{
    std::vector<std::string> names; // empty = every attackable mob (hostile, neutral or unknown reaction)
    std::vector<std::string> excludedNames;
    int minLevel = 1;
    int maxLevel = 255;
    float maxDistance = 40.0f; // search radius for new pulls
    float maxHeightDifference = 20.0f;
    // Below that cap, a target may be this far above or below the player plus
    // the slope to it: a mob up a hill is fine, one in a mine under our feet is
    // not. Jasperlode Mine's kobolds sit 17-40 yd down at 20-30 yd out.
    float heightAllowance = 3.0f;
    float maxSlope = 0.5f; // rise per yard of level distance, about 27 degrees
    float aggroRadiusBase = 20.0f; // BotConfig::aggroRadiusBase
    bool attackElites = false;
    // NPC entries known to be elite (rare elite, boss): the 64-bit clients do
    // not say, the quest data does (data/quests/elites.json).
    std::shared_ptr<const std::unordered_set<std::uint32_t>> eliteEntries;
    bool attackPlayers = false;
    bool attackNeutral = true; // unlisted neutral mobs (listed ones are always allowed)
    // Never deliberately pull a mob that is grey to the player. Self-defence is
    // unaffected: something that attacks first is still fought back. A quest's
    // mobs (setQuestMobs) are pulled grey or not.
    bool skipGrayMobs = false;
    // Passive mode: never deliberately pull or tag mobs. Self-defence is unaffected.
    bool passiveMode = false;
    float addRadius = 12.0f; // hostiles this close to a candidate count as potential adds
    // On the way somewhere, a hostile mob this near ahead is fought before it
    // comes for us (threatInTheWay; General Settings' protect range).
    float protectRange = 15.0f;
    // Within the protect range, how far off the direction of travel the mob
    // may be, in degrees: 90 is ahead and beside the way, never behind; 180
    // any mob about us.
    float protectAngle = 90.0f;
    // Avoid packed mobs: a mob with more hostile mobs than this within
    // addRadius (chain aggro) is left alone, and they with it. 0 is off.
    int maxPackAdds = 0;
    Duration blacklistDuration{120000};
};

struct ScoredTarget
{
    const UnitState* unit = nullptr;
    float score = 0.0f;
};

// Target selection is its own module - combat classes never pick targets.
class TargetSelector
{
public:
    TargetSelector(TargetFilterConfig config, TargetBlacklist& blacklist);

    // nullopt = valid pull target, otherwise the reason it was rejected.
    std::optional<std::string> rejectReason(const UnitState& unit, const GameSnapshot& snapshot) const;
    // Elite as the client says, or as the quest data knows the NPC.
    bool isElite(const UnitState& unit) const
    {
        return unit.isElite() || (config_.eliteEntries && unit.entry != 0 && config_.eliteEntries->contains(unit.entry));
    }
    bool isValidTarget(const UnitState& unit, const GameSnapshot& snapshot) const
    {
        return !rejectReason(unit, snapshot);
    }

    float scoreTarget(const UnitState& unit, const GameSnapshot& snapshot) const;
    // Freeroam mode: not in quest mode and not in passive mode
    bool isFreeroam() const
    {
        return questMobs_.empty() && !config_.passiveMode;
    }
    // Further above or below us than the slope to it allows: under the ground,
    // in a cave below or on another floor. Worth asking again while closing in,
    // since a mob we get nearer to without getting nearer in height is one of those.
    bool tooSteep(const UnitState& unit, const GameSnapshot& snapshot) const;

    // The hostile mobs within addRadius of `unit` that would come along
    // (chain aggro): living, not critters, not the unit itself.
    std::vector<const UnitState*> packAround(const UnitState& unit, const GameSnapshot& snapshot) const;
    // More of them than maxPackAdds allows.
    // Not one under a roof (setUnderRoof): in a mine there is no way round a
    // pack, and the quest's mobs are the pack.
    bool packed(const UnitState& unit, const GameSnapshot& snapshot) const
    {
        return config_.maxPackAdds > 0 && !underRoof_.contains(unit.guid) && static_cast<int>(packAround(unit, snapshot).size()) > config_.maxPackAdds;
    }

    // The mobs standing under a roof - in a mine, a cave - this tick: not
    // kept clear of as a pack while they stand there. By Echo Ridge Mine the
    // bot left the kobolds for 5 minutes, and went over the hill for them.
    void setUnderRoof(std::unordered_set<Guid> guids)
    {
        underRoof_ = std::move(guids);
    }
    bool underRoof(Guid guid) const
    {
        return underRoof_.contains(guid);
    }

    // Whether all mobs in the pack (unit and the mobs around it) are grey to the player
    bool packAllGrey(const UnitState& unit, const GameSnapshot& snapshot) const;

    // How near a hostile mob comes for us, as in Classic: 20 yards at the same
    // level, a yard more for each level it is above us and less for each below,
    // between 5 and 45.
    static float aggroRadius(int mobLevel, int playerLevel, float base = 20.0f);
    // A hostile mob within a fixed 10 yards ahead along the direction of travel.
    // Not one behind the line of travel (walked away from), above the level range
    // (walked round), an elite when elites are off, one grey to the character,
    // one tagged by another or blacklisted;
    // nothing in passive mode. Nearest first; nullptr when none.
    // `lookAhead`: farther than the protect range, a mob is looked for as far
    // as this - one the way passes within the protect range of.
    // `way`: the way ahead as planned, from the character on. Given, a mob
    // farther than the protect range counts only where the way passes within
    // its aggro range - where it could not be gone round.
    const UnitState* threatInTheWay(const GameSnapshot& snapshot, std::optional<float> travelHeading = std::nullopt, float lookAhead = 0.0f, std::span<const Vec3> way = {}) const;

    // A hostile mob standing between the character and `target`, that going
    // for the target would walk into (or pull along): fought first. The same
    // mobs threatInTheWay() counts, nearer the character than the target by
    // some yards and within their aggro range (8 yd at most) of the straight
    // line to it; nearest first. Of two mobs only the nearer can stand in the
    // other's way, so the choice never goes back and forth. A quest's mobs or
    // not: it comes for the character either way.
    const UnitState* standingInTheWay(const GameSnapshot& snapshot, const UnitState& target) const;

    // Best new target to pull, or nullptr.
    const UnitState* select(const GameSnapshot& snapshot) const;
    // Best unit among those already attacking us (combat target switching).
    const UnitState* selectAttacker(const GameSnapshot& snapshot) const;
    // All valid candidates, best first (debug/UI).
    std::vector<ScoredTarget> rank(const GameSnapshot& snapshot) const;

    const TargetFilterConfig& config() const
    {
        return config_;
    }

    void setConfig(TargetFilterConfig config)
    {
        auto quest = std::move(questMobs_);
        config_ = std::move(config);
        questMobs_ = std::move(quest);
    }

    // Ground a mob too strong to fight would notice the character on - where
    // it stands and along the way it walks (StrongMobWatch) - or a mob of a
    // pack (Avoid packed mobs). A mob standing there is not pulled: the fight
    // would bring the strong one, or the pack, in.
    struct DangerZone
    {
        Vec3 center;
        float radius = 0;
        std::string name; // the strong mob's, or the pack member's
        bool pack = false;
        bool respawn = false; // where `name` comes back at once (FastRespawn)
    };
    void setDangerZones(std::vector<DangerZone> zones)
    {
        dangerZones_ = std::move(zones);
    }

    // Quest Mode: pull only these mobs (by entry), whatever the profile's target
    // list says; empty goes by the profile again.
    void setQuestMobs(std::vector<std::uint32_t> entries)
    {
        questMobs_ = std::move(entries);
    }

    TargetBlacklist& blacklist()
    {
        return blacklist_;
    }

    void setAvoidedPosition(std::function<bool(const Vec3&)> avoided)
    {
        avoidedPosition_ = std::move(avoided);
    }

    // Mobs where this says true are not pulled: outside the area roamed in.
    void setOutOfBounds(std::function<bool(const Vec3&)> outside)
    {
        outOfBounds_ = std::move(outside);
    }

private:
    bool nameMatches(const std::vector<std::string>& list, const std::string& name) const;
    int countAdds(const UnitState& unit, const GameSnapshot& snapshot) const;

public:
    // How far a fleeing mob goes for help (the classic servers: about 30 yd).
    static constexpr float kFleeHelpRadius = 30.0f;

private:

    std::function<bool(const Vec3&)> avoidedPosition_;
    std::function<bool(const Vec3&)> outOfBounds_;
    TargetFilterConfig config_;
    std::vector<std::uint32_t> questMobs_;
    std::vector<DangerZone> dangerZones_;
    std::unordered_set<Guid> underRoof_;
    TargetBlacklist& blacklist_;
};

} // namespace og
