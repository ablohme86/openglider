/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-13
 *  File: ObstacleMemory.h
 *
 *  Desc:
 *  Declarations and interface for ObstacleMemory (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/MathUtil.h"
#include "og/common/Vec3.h"
#include "og/navigation/WaypointRoute.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <stop_token>
#include <functional>
#include <string>
#include <vector>

namespace og
{

class Logger;

// The recovery moves worth making ahead of an obstacle. Turning away or backing
// up is not: when that is what it takes, the way round becomes a detour in the
// route instead.
enum class ObstacleManeuver
{
    Jump,
    StrafeLeft,
    StrafeRight
};

std::string_view toString(ObstacleManeuver maneuver);
std::optional<ObstacleManeuver> parseObstacleManeuver(std::string_view name);

// A spot where the bot jammed and one move got it past. The next time it gets
// there heading the same way, it makes that move before it jams.
struct LearnedManeuver
{
    Vec3 position;        // where the move that worked was started
    float heading = 0.0f; // direction of travel then (WoW heading convention)
    ObstacleManeuver maneuver = ObstacleManeuver::Jump;
    int successes = 1; // times it got the bot past
    int failures = 0;  // times making it ahead still ended stuck there
};

// A way round an obstacle on one leg of the grind route. Kept by the leg's end
// points rather than an index into the route, so it survives edits elsewhere.
struct LearnedDetour
{
    Vec3 from; // the waypoint the leg starts at
    Vec3 to;   // the waypoint it ends at
    Vec3 stuckAt;
    std::vector<Vec3> points;
};

// A small area the bot got stuck in while roaming inside its route. Roaming
// never picks a spot there again, nor walks a straight line through it.
struct AvoidZone
{
    Vec3 center;
    float radius = 6.0f;
    std::string name = {}; // the mob a zone round one is for, for the log
    bool ignored = false;  // pack ignored because all mobs in it are grey
    // How far up and down from its centre the zone holds; 0: at every height
    // (a No Go zone painted on the map, which has none). A mob's zone is the
    // mob's own level: the kobolds of the Fargodeep Mine closed the hill over
    // their heads, and every way across it.
    float reach = 0.0f;

    // Whether both heights lie over the zone, or both under it: nothing
    // between them is in it.
    bool apart(float a, float b) const
    {
        return reach > 0.0f && ((a > center.z + reach && b > center.z + reach) || (a < center.z - reach && b < center.z - reach));
    }

    // The reach a zone round a mob takes from its radius.
    static float reachOf(float radius)
    {
        return std::max(6.0f, radius / 3.0f);
    }
};

// What a profile remembers about the obstacles on its routes.
struct LearnedObstacles
{
    std::vector<LearnedManeuver> maneuvers;
    std::vector<LearnedDetour> detours;
    std::vector<AvoidZone> avoidZones;
    bool empty() const
    {
        return maneuvers.empty() && detours.empty() && avoidZones.empty();
    }
};

struct ObstacleMemoryConfig
{
    bool learn = false;        // record new fixes: the "Auto-fix obstacles" setting
    float jumpDistance = 1.5f; // yards short of the spot to jump
    // A jump fires this far to either side of the spot, too: the line walked
    // differs by a yard or two from lap to lap, and an obstacle is wider than a point.
    float jumpSideways = 2.5f;
    float strafeDistance = 2.5f; // yards short of the spot to start the sidestep
    float sameSpot = 2.0f;       // a fix this close to a remembered one replaces it
    float sameDirection = degToRad(45.0f);
    Duration retrigger{4000}; // one move per approach
    int maxFailures = 2;      // forgotten once making it ahead fails this often in a row
    int maxManeuvers = 64;
    int maxDetours = 24;
    float avoidRadius = 6.0f; // yards round a spot roaming got stuck at
    int maxAvoidZones = 256;  // learned on their own; one added by hand goes in regardless
};

// The obstacles a bot has learned its way past, and the moves it makes ahead of
// them. Replayed whether or not learning is on; only new fixes need it.
class ObstacleMemory
{
public:
    // The session's stop request also interrupts searches through these obstacles.
    void setStopToken(std::stop_token stop)
    {
        stop_ = stop;
    }

    std::stop_token stopToken() const
    {
        return stop_;
    }

    void setCalculationProgress(std::function<void(int)> callback)
    {
        calculationProgress_ = std::move(callback);
    }

    // Nested searches belong to the outer calculation. -1 clears its UI label
    // on success, failure or cancellation, including early returns.
    class Calculation
    {
    public:
        explicit Calculation(const ObstacleMemory* memory) : memory_(memory)
        {
            if (memory_ && ++memory_->calculationDepth_ == 1)
            {
                report(0);
            }
        }

        ~Calculation()
        {
            if (memory_ && --memory_->calculationDepth_ == 0 && memory_->calculationProgress_)
            {
                memory_->calculationProgress_(-1);
            }
        }

        Calculation(const Calculation&) = delete;
        Calculation& operator=(const Calculation&) = delete;
        void report(int percent) const
        {
            if (memory_ && memory_->calculationDepth_ == 1 && memory_->calculationProgress_)
            {
                memory_->calculationProgress_(percent);
            }
        }

    private:
        const ObstacleMemory* memory_;
    };
    void configure(const ObstacleMemoryConfig& config)
    {
        config_ = config;
    }

    const ObstacleMemoryConfig& config() const
    {
        return config_;
    }

    void setLearning(bool on)
    {
        config_.learn = on;
    }

    // Pathfinding: the client's own map knows the walls, and a learned circle
    // round a spot the bot jammed at is planned round in a straight line - in
    // a tunnel or a mine, through the rock. The table is neither consulted nor
    // added to (it stays as loaded, for the profile file); only the No Go
    // zones count.
    void setPathfinding(bool on)
    {
        pathfinding_ = on;
    }

    bool pathfinding() const
    {
        return pathfinding_;
    }

    // What navigation keeps clear of and walks: none of it with Pathfinding.
    const std::vector<AvoidZone>& avoidZones() const;
    const std::vector<LearnedDetour>& detours() const;

    // Replaces everything remembered (a profile was loaded).
    void setLearned(LearnedObstacles learned);
    const LearnedObstacles& learned() const
    {
        return learned_;
    }

    // Bumps on every change worth writing back to the profile.
    std::uint64_t revision() const
    {
        return revision_;
    }

    // Bumps whenever a move is learned, so a detour recorded while working free
    // of the same jam can stand down.
    std::uint64_t maneuversLearned() const
    {
        return maneuversLearned_;
    }

    // The move to make now, when the bot is closing on a remembered spot heading
    // the way it was going when it jammed. Each spot fires once per approach.
    std::optional<LearnedManeuver> due(const Vec3& position, float travelHeading, TimePoint now);

    // A recovery move freed the bot at `position`. Remembered, replacing a
    // different move remembered for the same spot and direction.
    void learnManeuver(const Vec3& position, float heading, ObstacleManeuver maneuver, Logger* log = nullptr);
    // Making `made` ahead of its spot did, or did not, get the bot past.
    void reportAhead(const LearnedManeuver& made, bool passed, Logger* log = nullptr);

    // False when learning is off or the cap is reached.
    bool learnDetour(LearnedDetour detour, Logger* log = nullptr);

    // Roaming got stuck at `center`: stay clear of the area round it. False when
    // learning is off, the cap is reached, or the spot is already avoided.
    // `force` (Add Obstacle Here, Aggressive registration) ignores learning
    // being off; `byHand` (Add Obstacle Here) the cap as well.
    bool learnAvoidZone(const Vec3& center, Logger* log = nullptr, bool force = false, bool byHand = false);
    void setNoGoZones(std::vector<AvoidZone> zones)
    {
        noGoZones_ = std::move(zones);
    }

    const std::vector<AvoidZone>& noGoZones() const
    {
        return noGoZones_;
    }

    // Whether `point` lies in an avoided area.
    bool avoids(const Vec3& point) const;
    // Whether the straight line from `a` to `b` stays clear of every avoided area.
    bool clearPath(const Vec3& a, const Vec3& b) const;

    void forget();

private:
    std::optional<std::size_t> find(const Vec3& position, float heading) const;

    ObstacleMemoryConfig config_;
    bool pathfinding_ = false;
    std::stop_token stop_;
    std::function<void(int)> calculationProgress_;
    mutable int calculationDepth_ = 0;
    LearnedObstacles learned_;
    std::vector<AvoidZone> noGoZones_;
    std::vector<TimePoint> lastFired_; // parallel to learned_.maneuvers
    std::uint64_t revision_ = 0;
    std::uint64_t maneuversLearned_ = 0;
};

// Splices remembered detours into a route, each onto the leg whose end points
// still match within `tolerance` yards. Returns how many went in; a detour for
// a leg the route no longer has is skipped.
std::size_t applyDetours(WaypointRoute& route, const std::vector<LearnedDetour>& detours, float tolerance = 1.0f);
// The same for a path walked once from its first point to its last, such as a
// vendor trip or a corpse run: on legs from `first` on, and a detour learned
// going the other way is walked backwards.
std::size_t applyDetours(std::vector<Vec3>& path, const std::vector<LearnedDetour>& detours, std::size_t first = 0, float tolerance = 1.0f);

} // namespace og
