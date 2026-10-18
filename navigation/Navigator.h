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
 *  File: Navigator.h
 *
 *  Desc:
 *  Declarations and interface for Navigator (navigation, pathfinding,
 *  movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/EventBus.h"
#include "og/common/Log.h"
#include "og/navigation/INavigationProvider.h"
#include "og/navigation/Lifts.h"
#include "og/navigation/MovementController.h"
#include "og/navigation/ObstacleMemory.h"
#include "og/navigation/StuckDetector.h"
#include "og/navigation/StuckRecovery.h"
#include "og/state/GameSnapshot.h"

#include <optional>
#include <random>

namespace og
{

struct StuckDetectedEvent
{
    Vec3 position;
    RecoveryStage stage;
};

enum class ObstacleRegistration
{
    Normal,
    Aggressive
};

struct NavigatorConfig
{
    float repathDistance = 4.0f; // destination moved more than this -> new path
    float cornerRadius = 2.0f;   // arrival radius for intermediate path points
    // On the client's map: how far out from a wall or a house's corner the
    // way may go round it, at most - drawn anew for each way, so it does not
    // hug every wall. 0 keeps the shortest way.
    float wallClearance = 2.5f;
    StuckConfig stuck;
    ObstacleRegistration obstacleRegistration = ObstacleRegistration::Normal;
};

enum class NavStatus
{
    Idle,
    Moving,
    Arrived,
    Recovering,
    Unreachable
};
std::string_view toString(NavStatus s);

// "Go to this position": pathfinding (via INavigationProvider) + path following
// (via MovementController) + stuck detection/recovery. Every behavior that
// needs to move goes through the Navigator, so stuck handling is uniform.
class Navigator
{
public:
    Navigator(INavigationProvider& provider, MovementController& movement, const IClock& clock, Logger& log, EventBus* events = nullptr, NavigatorConfig config = {});

    // Call every tick while you want to move. stopOnArrival=false keeps
    // running (chain waypoints without stopping). exact=true walks the straight
    // line to the destination and never leaves it: no way round No Go zones or
    // learned obstacles, and no retreat from a jam, only the recovery moves -
    // for a recorded route that has to be followed to the letter, indoors.
    // intoZone=true goes to a destination inside a No Go zone or obstacle by
    // the shortest way in (a recorded route painted over), instead of giving up.
    NavStatus moveTo(const GameSnapshot& snapshot, const Vec3& destination, float arriveRadius, bool stopOnArrival = true, bool exact = false, bool intoZone = false);
    // Plan from a safe position 250 ms ahead before yielding to a new moving
    // activity. Uses loaded geometry only; moveTo consumes the prepared route.
    bool preparePath(const GameSnapshot& snapshot, const Vec3& destination);
    bool hasPreparedPath() const
    {
        return prepared_.has_value();
    }

    void stop();
    // stop(), when moveTo() has had the bot going since the last stop; nothing
    // otherwise. For a caller that stands and faces something every tick:
    // stop() let go of the turn key face() holds, and a key let go of stays up
    // for a moment, so the bot turned in jerks.
    void halt();
    // Whether a path may end at this point: outside every No Go zone and
    // learned obstacle, with the clearance a route keeps from them.
    bool canStandAt(const Vec3& point) const;

    // Mobs to keep clear of for now (too strong to fight), each as a circle of
    // the distance at which it would come for us. Planned round like No Go
    // zones, but a mob the bot stands in the circle of, or that stands at the
    // destination, is walked past as before. Set every tick; the path is only
    // planned again when a circle comes in its way.
    void setMobZones(std::vector<AvoidZone> zones);
    // Avoid packed mobs: the packs' reach, kept clear of as a No Go zone is -
    // never let go of when there is no way round other mobs, or while walking
    // past them; the bot cut straight through a pack so.
    void setPackZones(std::vector<AvoidZone> zones)
    {
        packZones_ = std::move(zones);
    }

    // The humanizer: on the client's map, a long straight leg sways up to
    // this many yards to one side about its middle. 0: straight.
    void setSway(float yards)
    {
        sway_ = yards;
    }

    // What the server has put in the world that the map's files have not -
    // Hallow's End's pumpkins, a mailbox, an anvil - round its model's
    // collision (ObjectFootprints). Gone round on the way, never in the way
    // of setting out or of arriving: a vendor stands beside a pumpkin.
    void setObjectZones(std::vector<AvoidZone> zones)
    {
        objectZones_ = std::move(zones);
    }

    const std::vector<AvoidZone>& objectZones() const
    {
        return objectZones_;
    }

    // How far out from a wall the way may go round it (NavigatorConfig::wallClearance).
    void setWallClearance(float yards)
    {
        config_.wallClearance = yards;
    }

    const std::vector<AvoidZone>& packZones() const
    {
        return packZones_;
    }
    const std::vector<AvoidZone>& mobZones() const
    {
        return mobZones_;
    }

    // The lifts known, and their cars in sight.
    const LiftWatch& lifts() const
    {
        return lifts_;
    }
    // Riding a lift, or on the way to one, waiting for its car.
    bool onLiftTrip() const
    {
        return ride_.has_value();
    }
    // The heading off the car at the other stop, on a lift trip.
    std::optional<float> liftExitHeading() const
    {
        return ride_ ? std::optional<float>{ride_->exitHeading} : std::nullopt;
    }
    // Where the car is waited for, on a lift trip.
    std::optional<Vec3> liftWaitAt() const
    {
        return ride_ ? std::optional<Vec3>{ride_->waitAt} : std::nullopt;
    }

    void setProvider(INavigationProvider& provider)
    {
        provider_ = &provider;
        prepared_.reset();
        clearPath();
    }

    // Obstacles to move past ahead of time, and to learn new ones into. Without
    // one the navigator only recovers after the fact.
    void setObstacleMemory(ObstacleMemory* memory)
    {
        obstacles_ = memory;
    }

    INavigationProvider& provider()
    {
        return *provider_;
    }

    MovementController& movement()
    {
        return movement_;
    }

    const std::optional<Vec3>& destination() const
    {
        return destination_;
    }

    const Path& path() const
    {
        return path_;
    }

    std::size_t pathIndex() const
    {
        return pathIndex_;
    }

    bool recovering() const
    {
        return recovery_.active() || retreatTo_.has_value();
    }

    int stuckCount() const
    {
        return stuckCount_;
    }

    // The most recent failure reason when moveTo returned Unreachable.
    RecoveryResult lastFailure() const
    {
        return lastFailure_;
    }

    void setObstacleRegistration(ObstacleRegistration reg)
    {
        config_.obstacleRegistration = reg;
    }

    void setStuckWindow(Duration window)
    {
        config_.stuck.window = window;
        stuck_.setWindow(window);
    }

    ObstacleRegistration obstacleRegistration() const
    {
        return config_.obstacleRegistration;
    }

    float estimatedRunSpeed() const;

    // Direction the bot is trying to go: towards the path point it steers for.
    float travelHeading(const PlayerState& player) const;

private:
    // moveTo on the level: the way over the ground (or the bed of the lake).
    NavStatus moveFlat(const GameSnapshot& snapshot, const Vec3& destination, float arriveRadius, bool stopOnArrival, bool exact, bool intoZone);
    // In the water: +1 to swim up to `destination`, -1 down to it, 0 neither.
    int depthStep(const PlayerState& player, const Vec3& destination) const;
    std::optional<float> riseZ_; // the height a tick ago, swimming up
    int riseStill_ = 0;          // ticks it has risen no further
    std::optional<TimePoint> riseRetryAt_; // let go of the key until then
    static constexpr Duration kRiseRetry{3000};

    struct PreparedPath
    {
        Vec3 from, to;
        Path path;
        TimePoint at;
        std::optional<std::uint32_t> map;
        std::uint64_t revision;
    };
    std::optional<PreparedPath> prepared_;
    bool computePath(const PlayerState& player, const Vec3& to);
    bool planPath(const PlayerState& player, const Vec3& to, const std::vector<AvoidZone>& preferredSides = {});
    std::vector<AvoidZone> avoidedZones() const;
    // The zones the current path has to keep clear of: none while moving exactly.
    std::vector<AvoidZone> pathZones() const
    {
        return exact_ ? std::vector<AvoidZone>{} : avoidedZones();
    }

    // Without the zones the bot stands in, and with intoZone_ those covering
    // where it is going.
    std::vector<AvoidZone> pathZones(const Vec3& from, const Vec3& to) const
    {
        if (exact_)
        {
            return {};
        }

        auto all = avoidedZones();
        std::vector<AvoidZone> result;
        result.reserve(all.size() + mobZones_.size());

        for (const auto& z : all)
        {
            if (distance2D(from, z.center) > z.radius && (!intoZone_ || distance2D(to, z.center) > z.radius))
            {
                result.push_back(z);
            }
        }

        if (!walkingPastMobs())
        {
            for (const auto& z : mobZones_)
            {
                if (distance2D(from, z.center) > z.radius && distance2D(to, z.center) > z.radius)
                {
                    result.push_back(z);
                }
            }
        }

        for (const auto& z : objectZones_)
        {
            if (distance2D(from, z.center) > z.radius + kObjectApproach && distance2D(to, z.center) > z.radius + kObjectApproach)
            {
                result.push_back(z);
            }
        }

        return result;
    }

    // Rerouted round mobs again and again on the way to the same place - mobs
    // wandering about a farm, each new way crossed by another - the bot walks
    // past them for a while instead: it went round and round otherwise.
    bool walkingPastMobs() const
    {
        return clock_.now() < pastMobsUntil_;
    }

    void noteReroute(const Vec3& to);
    // Whether a mob circle now lies across what is left of the path.
    // The mob zone the way ahead crosses, when it is to be planned round
    // again; nullptr when none.
    const AvoidZone* mobZoneOnPath(const Vec3& from) const;
    // Keep the chosen side of mobs AND packs for the same destination until
    // it cannot be walked. The normal is fixed when chosen: turning toward
    // the goal during a detour must not rotate what "the same side" means.
    struct KeptSide
    {
        Vec3 center;
        Vec3 towardSide;
        bool pack = false;
    };
    void keepSides(const Vec3& from, const Vec3& to);
    std::vector<AvoidZone> otherSides(const Vec3& from, const Vec3& to) const;
    std::vector<KeptSide> keptSides_;
    Vec3 plannedTo_; // where the way was planned to; its end may follow a target a little from there
    std::minstd_rand wallRandom_{std::random_device{}()};
    float sway_ = 0.0f;
    std::uint32_t swaySeed_ = std::random_device{}();
    std::optional<Vec3> keptSidesTo_;
    std::vector<AvoidZone> mobZones_;
    std::vector<AvoidZone> packZones_;
    std::vector<AvoidZone> objectZones_;
    // Closer than this to an object's edge, the way to or from it goes straight.
    static constexpr float kObjectApproach = 3.0f;
    bool pathPastMobs_ = false;           // no way round them: the path goes past them, and stays
    int orbitsSeen_ = 0;                  // the movement's orbits already dealt with
    std::optional<TimePoint> hoverSince_; // since when right by path point hoverIndex_
    std::size_t hoverIndex_ = 0;
    std::optional<TimePoint> lastMoveAt_; // the last moveTo
    std::optional<TimePoint> headwayAt_;  // since when going without getting kHeadway from headwayFrom_
    Vec3 headwayFrom_{};
    std::vector<TimePoint> reroutes_; // new paths to the same place while mob circles were about
    // The mobs whose reach a way to crossedTo_ crossed, and when: one
    // crossing it again, the bot is going back and forth between ways round.
    std::vector<std::pair<Vec3, TimePoint>> crossed_;
    std::optional<Vec3> crossedTo_;
    bool crossedBefore(const AvoidZone& zone, const Vec3& to);
    TimePoint lastReroute_{};
    TimePoint pastMobsUntil_{};
    bool exact_ = false;
    bool intoZone_ = false;
    bool pathIntoZone_ = false;
    bool pathExact_ = false;
    std::optional<TimePoint> jamSince_;
    bool jamReplanned_ = false; // a new path was planned for this jam
    // On the client's map: what the bot jammed against is not on the map - a
    // fence, a cart, a tent rope - so a new path planned there was the old
    // one again. The place just ahead of each jam, kept clear of for a while,
    // makes the new path another way. Not saved: gone with the session.
    struct JamZone
    {
        AvoidZone zone;
        TimePoint until;
        // Where the bot stood stuck: a narrower zone there, just ahead of it,
        // where the wide one closes the way beside it too.
        Vec3 at;
        Vec3 along;
    };
    std::vector<JamZone> jamZones_;
    void rememberJam(const Vec3& at, const Vec3& along);
    Vec3 jamAt_;
    float arrivalHeading_ = 0.0f;
    std::optional<Vec3> retreatTo_;
    TimePoint retreatSince_{};
    bool retreatFaced_ = false;
    int retreatCount_ = 0;
    std::optional<Vec3> currentGoal_;
    bool going_ = false; // moveTo() since the last stop()
    // A whole turn made without getting anywhere (round a point, or on the
    // spot): the way is planned again from where the bot stands, rather than
    // going round and round. Where the turning began, and how far it has gone.
    std::optional<Vec3> spinAt_;
    float spinTurned_ = 0.0f;
    float spinHeading_ = 0.0f;
    std::uint64_t pathRevision_ = 0;
    void clearPath();
    // Learns a recovery move once it has visibly freed the bot, and settles
    // whether a move made ahead got it past.
    void watchObstacleMoves(const GameSnapshot& snapshot);
    void makeMovesAhead(const PlayerState& player);
    void holdStrafeAhead();

    INavigationProvider* provider_;
    MovementController& movement_;
    const IClock& clock_;
    Logger& log_;
    EventBus* events_;
    NavigatorConfig config_;

    StuckDetector stuck_;
    StuckRecovery recovery_;

    // A way up to Thunder Bluff's mesa, or down from it, by lift: to where its
    // car stops, waited for there, ridden, and stepped off at the other stop.
    // Riding, the client tells the character where it got on until it steps
    // off: the car's height says where it is.
    struct LiftRide
    {
        Lift lift;
        bool fromTop = false;
        // The destination on the other level: a vendor's place in the quest
        // data has no height, and the character's was taken for it.
        Vec3 to;
        enum class Stage
        {
            ToStop,
            Waiting,
            Boarding,
            Riding,
            Leaving
        } stage = Stage::ToStop;
        TimePoint since{};
        // Ground by the shaft on the map, where the car is waited for, and
        // where it is stepped off to: the shaft itself is no ground on it.
        Vec3 waitAt;
        // The gate onto the car at the stop got on at, where one is known:
        // walked through its middle before the car's.
        std::optional<Vec3> gateAt;
        Vec3 stepOffTo;
        // The way off the car at the other stop, found when the trip is
        // planned: the heading from the car's middle along which the map has
        // ground farthest out (the plank into Thunder Bluff), and how far.
        // Turned to while riding, and walked straight off along: the car's
        // floor is not on the map, and the bot's place on it is a guess -
        // steered by that, it walked over the edge beside the plank.
        float exitHeading = 0.0f;
        float exitReach = 0.0f;
    };
    LiftWatch lifts_;
    std::optional<LiftRide> ride_;
    // Where a lift took the bot to, for the destination asked for.
    std::optional<std::pair<Vec3, Vec3>> liftTo_;
    std::optional<Vec3> liftCheckedFor_;
    // The lifts known when the way was last looked at for one: a lift learnt
    // on the way is looked at for the destination again.
    int liftsChecked_ = 0;
    std::optional<LiftRide> planLift(const GameSnapshot& snapshot, const Vec3& to);
    // The lift trip, while there is one: its status, or nullopt to walk on.
    std::optional<NavStatus> rideLift(const GameSnapshot& snapshot, Vec3& destination, bool exact);

    std::optional<Vec3> destination_;
    Path path_;
    std::size_t pathIndex_ = 0;
    int stuckCount_ = 0;
    RecoveryResult lastFailure_ = RecoveryResult::Done;

    ObstacleMemory* obstacles_ = nullptr;
    Vec3 speedSamplePosition_{};
    std::optional<TimePoint> speedSampleAt_;
    float runSpeedEstimate_ = 0.0f;
    Vec3 recoveryStart_;
    float recoveryHeading_ = 0.0f;
    // The recovery move that just finished, until it proves it freed the bot.
    struct Candidate
    {
        ObstacleManeuver maneuver;
        Vec3 at;
        float heading;
        TimePoint since;
    };
    std::optional<Candidate> candidate_;
    // A remembered move made ahead of its obstacle, until it is clear whether it
    // got the bot past.
    struct MadeAhead
    {
        LearnedManeuver move;
        TimePoint since;
    };
    std::optional<MadeAhead> madeAhead_;
    int strafeAhead_ = 0; // +1 left, -1 right
    TimePoint strafeAheadUntil_{};
};

} // namespace og
