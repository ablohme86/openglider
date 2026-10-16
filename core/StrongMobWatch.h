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
 *  File: StrongMobWatch.h
 *
 *  Desc:
 *  Declarations and interface for StrongMobWatch (bot engine, state
 *  machines, and automated behaviors).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/MathUtil.h"
#include "og/navigation/ObstacleMemory.h"
#include "og/state/GameSnapshot.h"

#include <map>
#include <string>
#include <vector>

namespace og
{

// A hostile mob above the maximum mob level, as the watch knows it.
struct StrongMob
{
    Guid guid = 0;
    std::string name;
    int level = 0;       // -1: "??", far above ours
    Vec3 position;
    Vec3 velocity;       // yards a second, by where it was a moment ago; 0 standing
    float aggroRange = 0; // how near it notices the character: more for each level above
    bool onUs = false;   // already fighting us, or coming: no keeping away from that
    float distance = 0;  // from the character

    // Where it is going to be within kLead, as far as its walk tells.
    Vec3 heading() const;
};

// Keeps an eye on every hostile mob above the maximum mob level the client
// shows: where each is, which way it walks and how near it notices the
// character. The way is planned round them, a mob standing within their reach
// is not pulled, and in a fight the character steps away from one that comes.
class StrongMobWatch
{
public:
    // How far ahead a walking mob's way is looked at.
    static constexpr float kLeadSeconds = 4.0f;
    static constexpr float kLeadLongest = 25.0f; // yards

    // How near a mob so many levels above the character notices it, as in
    // Classic: 20 yards at its own level and a yard more for each level above.
    static float aggroRange(int mobLevel, int playerLevel);

    void update(const GameSnapshot& snapshot, int maxLevel, TimePoint now);
    const std::vector<StrongMob>& mobs() const
    {
        return mobs_;
    }

    // Seen for the first time by this update: for the log.
    const std::vector<Guid>& spotted() const
    {
        return spotted_;
    }

    // The ground to keep off: each mob's reach plus `margin`, where it stands
    // and along the way it walks. Not the mobs on us, `engaged`, or those over
    // `within` yards from the character.
    std::vector<AvoidZone> zones(float margin, Guid engaged = 0, float within = 100.0f) const;
    // The mob whose reach plus `margin` holds the point - where it stands or
    // is walking to - the nearest first; nullptr when none. Not the mobs on
    // us or `engaged`.
    const StrongMob* threatTo(const Vec3& point, float margin, Guid engaged = 0) const;
    // The point of a mob's way nearest to `point`.
    static Vec3 nearestOnWay(const StrongMob& mob, const Vec3& point);

private:
    struct Track
    {
        Vec3 position;
        TimePoint sampledAt{};
        Vec3 velocity;
        TimePoint seenAt{};
    };
    std::map<Guid, Track> tracks_;
    std::vector<StrongMob> mobs_;
    std::vector<Guid> spotted_;
};

} // namespace og
