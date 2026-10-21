/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-02
 *  File: StrongMobWatch.cpp
 *
 *  Desc:
 *  Implementation of StrongMobWatch (bot engine, state machines, and
 *  automated behaviors).
 *
 *
 */
#include "og/core/StrongMobWatch.h"

#include <algorithm>
#include <cmath>

namespace og
{

namespace
{

// A position is compared with the one this long ago: shorter, and the client's
// own jitter reads as walking.
constexpr Duration kSample{500};
// Not seen for this long: forgotten (out of sight, dead).
constexpr Duration kForget{5000};
// Slower than this is standing.
constexpr float kStanding = 0.5f;

} // namespace

Vec3 StrongMob::heading() const
{
    const float speed = std::hypot(velocity.x, velocity.y);

    if (speed < kStanding)
    {
        return position;
    }

    const float lead = std::min(speed * StrongMobWatch::kLeadSeconds, StrongMobWatch::kLeadLongest);
    return position + Vec3{velocity.x / speed * lead, velocity.y / speed * lead, 0.0f};
}

float StrongMobWatch::aggroRange(int mobLevel, int playerLevel, float base)
{
    return std::clamp(base + static_cast<float>(mobLevel - playerLevel), 5.0f, 45.0f);
}

void StrongMobWatch::update(const GameSnapshot& s, int maxLevel, TimePoint now, float aggroBase, std::span<const std::uint32_t> questMobs)
{
    mobs_.clear();
    spotted_.clear();
    const PlayerState& p = s.player;

    if (p.isDeadOrGhost())
    {
        tracks_.clear();
        return;
    }

    for (const auto& u : s.units)
    {
        if (!u.isAlive() || u.isPlayer || u.isCritter || u.reaction != Reaction::Hostile)
        {
            continue;
        }

        // Not what cannot be attacked or selected, nor the game's own unseen
        // helpers: "[DNT] Invisible Stalker" (level 60, hostile, nowhere to be
        // seen) was stepped away from in the middle of a fight.
        // Nor a player's pet ("Generic Hunter Pet", level 20, 200 yd off): it
        // fights only where its owner does.
        if (!u.isAttackable || u.name.starts_with("[DNT]") || u.ownerGuid != 0)
        {
            continue;
        }

        // Do not classify a current quest objective as too strong from a
        // grinding level cap; quest objectives are handled by the target filter.
        if (std::find(questMobs.begin(), questMobs.end(), u.entry) != questMobs.end())
        {
            continue;
        }

        // Level -1: "??", far above ours.
        const int level = u.level > 0 ? u.level : p.level + 10;

        if (level <= maxLevel)
        {
            continue;
        }

        auto [it, fresh] = tracks_.try_emplace(u.guid, Track{u.position, now, {}, now});
        Track& track = it->second;

        if (fresh)
        {
            spotted_.push_back(u.guid);
        }
        else if (now - track.sampledAt >= kSample)
        {
            const float seconds = static_cast<float>(toSeconds(now - track.sampledAt));
            const Vec3 moved = u.position - track.position;
            // Half the old with half the new: a turn shows at once, a jitter does not.
            const Vec3 measured{moved.x / seconds, moved.y / seconds, 0.0f};
            track.velocity = Vec3{(track.velocity.x + measured.x) / 2, (track.velocity.y + measured.y) / 2, 0.0f};

            if (std::hypot(measured.x, measured.y) < kStanding)
            {
                track.velocity = {};
            }

            track.position = u.position;
            track.sampledAt = now;
        }

        track.seenAt = now;
        // Already ours: at us, or in the fight with us and no one else's - one
        // fleeing has no target, and Add Avoidance stepped away from a mob it
        // had aggro from already.
        const bool ours = u.targetGuid == p.guid || s.isAttacker(u) || (u.inCombat && !u.isTappedByOther && s.inCombat());

        // Busy with something else - another player, an NPC - it does not
        // come for us: not kept clear of.
        if (!ours && u.targetGuid != 0)
        {
            continue;
        }
        mobs_.push_back({u.guid, u.name, u.level, u.position, track.velocity, aggroRange(level, p.level, aggroBase), ours, distance(p.position, u.position)});
    }

    std::erase_if(tracks_, [&](const auto& t) { return now - t.second.seenAt > kForget; });
}

Vec3 StrongMobWatch::nearestOnWay(const StrongMob& mob, const Vec3& point)
{
    const Vec3 to = mob.heading();
    const float dx = to.x - mob.position.x, dy = to.y - mob.position.y;
    const float length2 = dx * dx + dy * dy;

    if (length2 < 0.01f)
    {
        return mob.position;
    }

    const float t = std::clamp(((point.x - mob.position.x) * dx + (point.y - mob.position.y) * dy) / length2, 0.0f, 1.0f);
    return {mob.position.x + dx * t, mob.position.y + dy * t, mob.position.z};
}

std::vector<AvoidZone> StrongMobWatch::zones(float margin, Guid engaged, float within) const
{
    std::vector<AvoidZone> out;

    for (const StrongMob& mob : mobs_)
    {
        if (mob.onUs || mob.guid == engaged || mob.distance > within)
        {
            continue;
        }

        const float radius = mob.aggroRange + margin;
        out.push_back({mob.position, radius, mob.name, false, AvoidZone::reachOf(radius)});
        // A walking mob's way, as circles close enough to leave no gap.
        const Vec3 to = mob.heading();
        const float way = distance2D(mob.position, to);
        const int steps = static_cast<int>(std::ceil(way / std::max(1.0f, radius * 0.75f)));

        for (int i = 1; i <= steps; ++i)
        {
            out.push_back({mob.position + (to - mob.position) * (static_cast<float>(i) / static_cast<float>(steps)), radius, mob.name, false, AvoidZone::reachOf(radius)});
        }
    }

    return out;
}

const StrongMob* StrongMobWatch::threatTo(const Vec3& point, float margin, Guid engaged) const
{
    const StrongMob* nearest = nullptr;
    float best = 0;

    for (const StrongMob& mob : mobs_)
    {
        if (mob.onUs || mob.guid == engaged)
        {
            continue;
        }

        const float radius = mob.aggroRange + margin;
        const Vec3 onWay = nearestOnWay(mob, point);

        // A mob too far above or below is on another level (e.g. in a mine or
        // cave under the ground) and will not aggro.
        if (std::fabs(point.z - onWay.z) > AvoidZone::reachOf(radius))
        {
            continue;
        }

        const float d = distance2D(point, onWay);

        if (d <= radius && (!nearest || d < best))
        {
            nearest = &mob;
            best = d;
        }
    }

    return nearest;
}

} // namespace og
