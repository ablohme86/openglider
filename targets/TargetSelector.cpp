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
 *  File: TargetSelector.cpp
 *
 *  Desc:
 *  Implementation of TargetSelector (target filtering, scoring, and
 *  priority selection).
 *
 *
 */
#include "og/targets/TargetSelector.h"

#include "og/common/MathUtil.h"
#include "og/navigation/Avoidance.h"
#include "og/state/Difficulty.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace og
{

namespace
{
bool iequals(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

} // namespace

TargetSelector::TargetSelector(TargetFilterConfig config, TargetBlacklist& blacklist) : config_(std::move(config)), blacklist_(blacklist)
{
}

bool TargetSelector::nameMatches(const std::vector<std::string>& list, const std::string& name) const
{
    return std::any_of(list.begin(), list.end(), [&](const std::string& n) { return iequals(n, name); });
}

float TargetSelector::aggroRadius(int mobLevel, int playerLevel, float base)
{
    const float radius = base + static_cast<float>(mobLevel - playerLevel);
    return std::clamp(radius, 5.0f, 45.0f);
}

const UnitState* TargetSelector::threatInTheWay(const GameSnapshot& s, std::optional<float> travelHeading, float lookAhead, std::span<const Vec3> way) const
{
    if (config_.passiveMode)
    {
        return nullptr;
    }

    const Vec3& at = s.player.position;
    const float heading = travelHeading.value_or(s.player.heading);
    const Vec3 dir = directionFromHeading(heading);
    // On the way to or from something (a quest giver, a vendor, a quest
    // object), a hostile mob this near ahead is fought before it comes for us:
    // at 10 yards the bot walked right into mobs that then pulled on their own.
    // 15 by default; General Settings' protect range.
    const float kPossibleAggroRange = config_.protectRange;
    const float protectAngle = std::clamp(config_.protectAngle, 0.0f, 180.0f);
    const float kCosProtectAngle = std::cos(protectAngle * kPi / 180.0f);
    const UnitState* best = nullptr;

    for (const auto& u : s.units)
    {
        if (u.isPlayer || !u.isAlive() || u.reaction != Reaction::Hostile || u.isCritter)
        {
            continue;
        }

        if (u.ownerGuid != 0 || u.isTappedByOther || !u.isAttackable || blacklist_.contains(u.guid))
        {
            continue;
        }

        const bool questMob = std::find(questMobs_.begin(), questMobs_.end(), u.entry) != questMobs_.end();

        if (u.level < config_.minLevel || u.level > config_.maxLevel || (isElite(u) && !config_.attackElites && (!questMob || u.level > s.player.level + 1)))
        {
            continue;
        }

        // Grey to the character: it comes for us only from right next to it,
        // and gives nothing. Walked past; fought if it does attack. A quest's
        // mob is wanted all the same.
        if (difficultyOf(u.level, s.player.level) == Difficulty::Gray && std::find(questMobs_.begin(), questMobs_.end(), u.entry) == questMobs_.end())
        {
            continue;
        }

        if (std::fabs(u.position.z - at.z) > config_.effectiveMaxHeightDifference())
        {
            continue;
        }

        // Under the ground, in a mine below: the fight would give it up at
        // once (CombatBehavior, "under the ground"), and the next mob down
        // there be taken the moment the bot walked on - over the Fargodeep
        // Mine it went from the way to a kobold and back all along the hill.
        if (tooSteep(u, s))
        {
            continue;
        }

        const Vec3 toMob = u.position - at;
        // Ahead along the direction of travel, and to the sides: within the
        // protect range up to the protect angle off it, 90 degrees by default
        // - one beside the way notices us as well (only straight ahead, it was
        // walked by and came after us), but none behind, passed already: no
        // pulling backwards. Farther off, only one ahead.
        const float along = toMob.x * dir.x + toMob.y * dir.y;
        const float d = distance2D(at, u.position);

        if (d > kPossibleAggroRange ? along <= 0.0f : (protectAngle < 180.0f && along < kCosProtectAngle * d))
        {
            continue;
        }

        // Farther off, as far as the class pulls from: one the way passes
        // within the protect range of - a hunter shoots it from 30 yards
        // rather than walking in to 15. Off to the side, it is passed.
        const float side = std::sqrt(std::max(0.0f, d * d - along * along));

        if (d > kPossibleAggroRange && d > lookAhead)
        {
            continue;
        }

        // Farther off, with the way as planned: only one it passes within
        // the aggro range of - no way round it (in a cave, a house). One
        // the way goes round is left alone.
        if (d > kPossibleAggroRange && !way.empty())
        {
            const AvoidZone reach{u.position, aggroRadius(u.level > 0 ? u.level : s.player.level + 10, s.player.level, config_.aggroRadiusBase), u.name};
            bool passes = false;

            for (std::size_t i = 0; i + 1 < way.size() && !passes; ++i)
            {
                passes = !clearOfZones(way[i], way[i + 1], std::span(&reach, 1));
            }

            if (!passes)
            {
                continue;
            }
        }
        else if (d > kPossibleAggroRange && side > kPossibleAggroRange)
        {
            continue;
        }

        if (!best || d < distance2D(at, best->position))
        {
            best = &u;
        }
    }

    return best;
}

const UnitState* TargetSelector::standingInTheWay(const GameSnapshot& s, const UnitState& target) const
{
    if (config_.passiveMode)
    {
        return nullptr;
    }

    const Vec3& at = s.player.position;
    const float toTarget = distance2D(at, target.position);

    if (toTarget < 1.0f)
    {
        return nullptr;
    }

    const Vec3 dir{(target.position.x - at.x) / toTarget, (target.position.y - at.y) / toTarget, 0};
    // Not wider than this, whatever the mob's level: farther off the line the
    // way to the target can be walked without it.
    constexpr float kWidest = 8.0f;
    // As near the target as this, the two come together anyway.
    constexpr float kBesideTarget = 3.0f;
    // Nearer the character than the target by this much, as the crow flies -
    // not only along the line to it. Of two trolls 12 and 14 yd off, a little
    // apart, each stood "before" the other along the line to it: the bot
    // changed its mind every tick and stood there for good.
    constexpr float kNearerBy = 3.0f;
    const UnitState* best = nullptr;
    float bestAlong = 0.0f;

    for (const auto& u : s.units)
    {
        if (u.guid == target.guid || u.isPlayer || !u.isAlive() || u.reaction != Reaction::Hostile || u.isCritter)
        {
            continue;
        }

        if (u.ownerGuid != 0 || u.isTappedByOther || !u.isAttackable || blacklist_.contains(u.guid))
        {
            continue;
        }

        const bool questMob = std::find(questMobs_.begin(), questMobs_.end(), u.entry) != questMobs_.end();

        if (u.level < config_.minLevel || u.level > config_.maxLevel || (isElite(u) && !config_.attackElites && (!questMob || u.level > s.player.level + 1)))
        {
            continue;
        }

        if (std::fabs(u.position.z - at.z) > config_.effectiveMaxHeightDifference() || tooSteep(u, s) || (avoidedPosition_ && avoidedPosition_(u.position)))
        {
            continue;
        }

        const Vec3 toMob = u.position - at;
        const float along = toMob.x * dir.x + toMob.y * dir.y;

        if (along <= 0.0f || along >= toTarget - kBesideTarget || distance2D(at, u.position) > toTarget - kNearerBy)
        {
            continue;
        }

        const float aside = std::fabs(toMob.x * dir.y - toMob.y * dir.x);

        if (aside > std::min(kWidest, aggroRadius(u.level, s.player.level, config_.aggroRadiusBase)))
        {
            continue;
        }

        if (!best || along < bestAlong)
        {
            best = &u;
            bestAlong = along;
        }
    }

    return best;
}

std::optional<std::string> TargetSelector::rejectReason(const UnitState& u, const GameSnapshot& s) const
{
    if (config_.passiveMode)
    {
        return "passive mode";
    }

    if (u.guid == s.player.guid)
    {
        return "self";
    }

    if (avoidedPosition_ && avoidedPosition_(u.position))
    {
        return "avoided area";
    }

    if (outOfBounds_ && outOfBounds_(u.position))
    {
        return "outside the roam area";
    }

    if (!u.isAlive())
    {
        return "dead";
    }

    if (u.reaction == Reaction::Friendly)
    {
        return "friendly";
    }

    if (u.isCritter)
    {
        return "critter";
    }

    if (u.isPlayer && !config_.attackPlayers)
    {
        return "player";
    }

    if (u.ownerGuid != 0)
    {
        return "pet/summon";
    }

    if (!u.isAttackable)
    {
        return "not attackable";
    }

    if (u.isTappedByOther)
    {
        return "tapped by other";
    }

    if (blacklist_.contains(u.guid))
    {
        return "blacklisted";
    }

    if (nameMatches(config_.excludedNames, u.name))
    {
        return "excluded";
    }

    const bool questMob = std::find(questMobs_.begin(), questMobs_.end(), u.entry) != questMobs_.end();

    if (!questMobs_.empty() && !questMob)
    {
        return "not a quest mob";
    }

    const bool listed = questMob || nameMatches(config_.names, u.name);

    if (!config_.names.empty() && !listed)
    {
        return "not in target list";
    }

    if (u.reaction == Reaction::Neutral && !config_.attackNeutral && !listed)
    {
        return "neutral";
    }

    if (u.level < config_.minLevel || u.level > config_.maxLevel)
    {
        return std::format("level {} outside {}-{}", u.level, config_.minLevel, config_.maxLevel);
    }

    // Only for pulls we choose to make; selectAttacker() does not come through
    // here, so anything that attacks first is still fought back. A quest's mob
    // is pulled grey or not: its drop or its kill is what the quest wants.
    if (config_.skipGrayMobs && !questMob && u.level > 0 && u.level <= grayLevel(s.player.level))
    {
        return std::format("grey at level {}", s.player.level);
    }

    if (isElite(u) && !config_.attackElites && (!questMob || u.level > s.player.level + 1))
    {
        return "elite";
    }

    // Only for pulls we choose to make, as the grey check: one that attacks
    // is fought where it stands.
    for (const DangerZone& zone : dangerZones_)
    {
        if (std::fabs(u.position.z - zone.center.z) > AvoidZone::reachOf(zone.radius))
        {
            continue;
        }

        if (distance2D(u.position, zone.center) <= zone.radius)
        {
            return zone.respawn ? std::format("where {} comes back at once", zone.name) : zone.pack ? std::format("within reach of a pack ({})", zone.name) : std::format("within reach of {}, too strong to fight", zone.name);
        }
    }

    const float dist = distance(s.player.position, u.position);

    if (dist > config_.maxDistance)
    {
        return "too far";
    }

    const float rise = std::fabs(u.position.z - s.player.position.z);

    if (rise > config_.effectiveMaxHeightDifference())
    {
        return "height difference";
    }

    if (tooSteep(u, s))
    {
        return std::format("{:.0f} yd above or below us, too steep to reach", rise);
    }

    // Someone else is already fighting it.
    if (u.inCombat && u.targetGuid != 0 && !s.isAttacker(u))
    {
        return "fighting someone else";
    }

    return std::nullopt;
}

bool TargetSelector::tooSteep(const UnitState& u, const GameSnapshot& s) const
{
    const float rise = std::fabs(u.position.z - s.player.position.z);
    if (config_.ranged)
    {
        const bool uphill = u.position.z >= s.player.position.z;
        const float slope = uphill ? config_.rangedUphillMaxSlope : config_.rangedMaxSlope;
        return rise > config_.heightAllowance + distance2D(s.player.position, u.position) * slope;
    }
    return rise > config_.heightAllowance + distance2D(s.player.position, u.position) * config_.maxSlope;
}

std::vector<const UnitState*> TargetSelector::packAround(const UnitState& unit, const GameSnapshot& s) const
{
    std::vector<const UnitState*> pack;

    for (const auto& other : s.units)
    {
        if (other.guid == unit.guid || !other.isAlive() || other.isCritter || other.isPlayer || other.ownerGuid != 0)
        {
            continue;
        }

        // Only hostile mobs come along. Neutral ones stay out of it, of the
        // same faction or not: Kobold Vermin beside each other were taken for
        // a pack, and none of them comes to another's help.
        if (other.reaction != Reaction::Hostile)
        {
            continue;
        }

        if (distance(other.position, unit.position) <= config_.addRadius)
        {
            pack.push_back(&other);
        }
    }

    return pack;
}

int TargetSelector::countAdds(const UnitState& unit, const GameSnapshot& s) const
{
    return static_cast<int>(packAround(unit, s).size());
}

bool TargetSelector::packAllGrey(const UnitState& unit, const GameSnapshot& s) const
{
    const int grayThresh = grayLevel(s.player.level);
    if (unit.level <= 0 || unit.level > grayThresh)
    {
        return false;
    }

    const auto pack = packAround(unit, s);
    for (const UnitState* other : pack)
    {
        if (other->level <= 0 || other->level > grayThresh)
        {
            return false;
        }
    }

    return true;
}

float TargetSelector::scoreTarget(const UnitState& u, const GameSnapshot& s) const
{
    const float dist = distance(s.player.position, u.position);

    if (isFreeroam())
    {
        float score = 0.0f;
        if (s.isAttacker(u))
        {
            score += 10000.0f;
        }
        score += (1000.0f - dist);
        score += (100.0f - u.healthPercent()) * 0.001f;
        return score;
    }

    float score = 0.0f;

    if (s.isAttacker(u))
    {
        score += 100.0f;
    }

    if (nameMatches(config_.names, u.name))
    {
        score += 50.0f;
    }

    // Closer is better: +40 at distance 0, and farther off on down past 40 yd.
    // Cut off at 0 there, a mob up the hill counted as near as one beside the
    // bot on the way to it, and one with fewer of its own about won.
    score += 40.0f - dist;

    const int levelDiff = u.level - s.player.level;

    if (levelDiff > 0)
    {
        score -= 10.0f * static_cast<float>(levelDiff);
    }

    const bool questMob = std::find(questMobs_.begin(), questMobs_.end(), u.entry) != questMobs_.end();

    if (isElite(u) && !questMob)
    {
        score -= 100.0f;
    }

    score -= 50.0f * static_cast<float>(countAdds(u, s));

    // A mob that flees at low health runs to the mobs of its faction about it
    // (within some 30 yards) and brings them: fewer of them, the better.
    if (u.factionTemplate != 0)
    {
        int friends = 0;

        for (const auto& other : s.units)
        {
            if (other.guid != u.guid && other.factionTemplate == u.factionTemplate && other.isAlive() && !other.inCombat && distance2D(other.position, u.position) <= kFleeHelpRadius)
            {
                ++friends;
            }
        }

        score -= 8.0f * static_cast<float>(std::min(friends, 5));
    }

    // Prefer targets that are already damaged.
    score += (100.0f - u.healthPercent()) * 0.1f;
    return score;
}

std::vector<ScoredTarget> TargetSelector::rank(const GameSnapshot& s) const
{
    std::vector<ScoredTarget> out;

    for (const auto& u : s.units)
    {
        if (isValidTarget(u, s))
        {
            out.push_back({&u, scoreTarget(u, s)});
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const ScoredTarget& a, const ScoredTarget& b) { return a.score > b.score; });
    return out;
}

const UnitState* TargetSelector::select(const GameSnapshot& s) const
{
    const auto ranked = rank(s);
    return ranked.empty() ? nullptr : ranked.front().unit;
}

const UnitState* TargetSelector::selectAttacker(const GameSnapshot& s) const
{
    const UnitState* best = nullptr;
    float bestScore = -1e9f;

    for (const UnitState* u : s.attackers())
    {
        if (blacklist_.contains(u->guid) || (u->isPlayer && !config_.attackPlayers))
        {
            continue;
        }

        float score = scoreTarget(*u, s);

        // Strongly prefer what we are already hitting to avoid target flapping.
        if (u->guid == s.targetGuid)
        {
            score += 1000.0f;
        }

        if (score > bestScore)
        {
            bestScore = score;
            best = u;
        }
    }

    return best;
}

} // namespace og
