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
 *  File: Navigator.cpp
 *
 *  Desc:
 *  Implementation of Navigator (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/Navigator.h"

#include "og/common/MathUtil.h"
#include "og/navigation/Avoidance.h"

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <random>
#include <utility>

namespace og
{

namespace
{

// A recovery move counts as the fix once the bot got this far past where it
// started, along the way it was going, soon enough after it. Only so far to the
// side, though: a bot that got free by walking off sideways found a way round,
// which is the detour's business, not the jump's.
constexpr float kFreedDistance = 3.0f;
constexpr float kJumpSideways = 2.5f;
constexpr float kStrafeSideways = 5.0f;
constexpr Duration kFreedWindow{2500};
// A move made ahead is judged a success if the bot has not jammed near its
// spot within this long.
constexpr Duration kAheadVerdict{3000};
constexpr float kAheadRadius = 4.0f;
constexpr Duration kStrafeAhead{900}; // as long as the recovery sidestep
// With Pathfinding, a bot not free this long after it jammed plans a new path
// from where it stands. Kept on the one it jammed on, the moves only pushed it
// back into the same wall; a fresh path (what Detach and Attach gave) found
// the way out.
constexpr Duration kReplanAfter{3000};
// Within this much height a path point is on the bot's level; one higher or
// lower is a floor above or below, however close it is on the map.
constexpr float kSameLevel = 2.5f;
// Within kHoverReach of a path point for kOffLevelHover and it counts as reached, level or not.
constexpr float kHoverReach = 1.5f;
constexpr Duration kOffLevelHover{2000};
// Going somewhere, the bot gets this far within kNoHeadwayFor or counts as jammed.
constexpr float kHeadway = 2.0f;
constexpr Duration kNoHeadwayFor{6000};
constexpr Duration kMoveGap{1500};
// Turning a whole circle within this far of where the turning began is going
// nowhere. A spiral stair is wider, and climbs.
constexpr float kSpinReach = 5.0f;
// Round mobs: a new way is kept at least this long; this many new ways to the
// same place within kReroutesWithin and the bot walks past them for kPastMobsFor.
constexpr Duration kRerouteSettle{2000};
constexpr std::size_t kReroutesBeforePassing = 4;
constexpr Duration kReroutesWithin{20000};
constexpr Duration kPastMobsFor{20000};
// The way round one mob led round past another and back past the first, every
// dozen seconds, the bot walking to and fro between them - too far apart for
// kReroutesWithin. The same mob crossing the way again within this long, to
// the same place, and the bot walks past them.
constexpr Duration kCrossedWithin{180000};
constexpr float kSameMob = 10.0f;

bool walkableArcSegment(INavigationProvider& provider, const Vec3& from, const Vec3& to)
{
    if (provider.canWalkStraight(from, to))
    {
        return true;
    }

    const float length = distance2D(from, to);

    if (length < 0.001f || length >= 1.2f)
    {
        return false;
    }

    // Tiny arc samples can give conservative results at collision edges.
    // Retry along the same line with a longer ray that still covers the whole
    // proposed step. A wall across the step blocks this ray as well.
    return provider.canWalkStraight(from, from + (to - from) * (2.0f / length));
}

// The client's map gives the shortest way: round every corner of a house as
// close as the collision allows, along every wall. Each corner is moved out,
// away from what it turns round, by a clearance drawn for the way (and a
// little more or less at each corner), where the legs either side of it can
// still be walked and stay clear of the zones kept off; in a narrow passage
// less, or not at all.
Path spreadFromWalls(const Vec3& from, Path path, INavigationProvider& provider, std::span<const AvoidZone> zones, float maxClearance, float turnRadius, std::minstd_rand& random)
{
    if (maxClearance <= 0.0f || path.points.size() < 2)
    {
        return path;
    }

    std::uniform_real_distribution<float> share(0.5f, 1.0f), jitter(0.85f, 1.0f);
    const float clearance = maxClearance * share(random);
    Vec3 previous = from;

    for (std::size_t i = 0; i + 1 < path.points.size(); ++i)
    {
        const Vec3 corner = path.points[i], next = path.points[i + 1];
        Vec3 in{corner.x - previous.x, corner.y - previous.y, 0.0f}, on{next.x - corner.x, next.y - corner.y, 0.0f};
        const float inLength = std::hypot(in.x, in.y), onLength = std::hypot(on.x, on.y);

        // Not where two corners lie close together (both ends of a thin
        // wall): moved apart, they made a zigzag the bot stopped to turn on.
        if (inLength > 3.0f && onLength > 3.0f)
        {
            in = in * (1.0f / inLength);
            on = on * (1.0f / onLength);
            // Away from the inside of the turn, where what it turns round is.
            Vec3 out = in - on;
            const float turn = std::hypot(out.x, out.y);

            if (turn > 0.35f) // more than some 20 degrees
            {
                out = out * (1.0f / turn);
                // The turn is rounded off afterwards (roundClientCorners), its
                // arc as far inside the corner as this: out by that much more.
                const float angle = std::acos(std::clamp(in.x * on.x + in.y * on.y, -1.0f, 1.0f));
                const float cut = angle < 2.6f ? turnRadius * (1.0f / std::cos(angle * 0.5f) - 1.0f) : 0.0f;
                const float wanted = clearance * jitter(random) + cut;

                for (const float part : {1.0f, 0.6f, 0.3f})
                {
                    const Vec3 moved = corner + out * (wanted * part);

                    if (provider.canWalkStraight(previous, moved) && provider.canWalkStraight(moved, next) && clearOfZones(previous, moved, zones) && clearOfZones(moved, next, zones))
                    {
                        path.points[i] = moved;
                        break;
                    }
                }
            }
        }

        previous = path.points[i];
    }

    return path;
}

// Nobody runs two hundred yards as a ruler lies: a long straight leg gets a
// point about its middle, a few yards to one side, where the ground either
// way of it can be walked and is clear of the zones kept off. The draws come
// from `seed` - the destination's - so the way planned again from further
// along the same trip leans the same way rather than to and fro.
Path swayLongLegs(const Vec3& from, Path path, INavigationProvider& provider, std::span<const AvoidZone> zones, float sway, std::uint32_t seed)
{
    constexpr float kLongLeg = 60.0f;
    constexpr float kShare = 0.05f; // of the leg's length, to the side at most
    constexpr float kLeast = 1.0f;  // less is no sway worth a corner

    if (sway <= 0.0f)
    {
        return path;
    }

    std::minstd_rand random(seed);
    std::uniform_real_distribution<float> along(0.35f, 0.65f), side(-1.0f, 1.0f);
    Path result;
    result.continues = path.continues;
    Vec3 previous = from;

    for (const Vec3& point : path.points)
    {
        const float length = distance2D(previous, point);
        // Drawn for every leg, long or not: the same legs get the same draws.
        const float at = along(random);
        const float off = side(random) * std::min(sway, length * kShare);

        if (length >= kLongLeg && std::abs(off) >= kLeast)
        {
            const Vec3 leg = point - previous;
            const Vec3 normal{-leg.y / length, leg.x / length, 0.0f};

            if (const auto mid = provider.findWalkablePoint(previous + leg * at + normal * off); mid && provider.canWalkStraight(previous, *mid) && provider.canWalkStraight(*mid, point) && clearOfZones(previous, *mid, zones) && clearOfZones(*mid, point, zones))
            {
                result.points.push_back(*mid);
            }
        }

        result.points.push_back(point);
        previous = point;
    }

    return result;
}

Path roundClientCorners(const Vec3& from, Path path, INavigationProvider& provider, float radius)
{
    std::vector<Vec3> rounded{from};

    for (std::size_t i = 0; i + 1 < path.points.size(); ++i)
    {
        const Vec3 corner = path.points[i], next = path.points[i + 1], previous = rounded.back();
        const float before = distance2D(previous, corner), after = distance2D(corner, next);

        if (before < 0.1f || after < 0.1f)
        {
            rounded.push_back(corner);
            continue;
        }

        Vec3 incoming = (corner - previous) / before, outgoing = (next - corner) / after;
        incoming.z = outgoing.z = 0;
        const float angle = std::acos(std::clamp(incoming.x * outgoing.x + incoming.y * outgoing.y, -1.0f, 1.0f));
        const float trim = radius * std::tan(angle * 0.5f);

        if (angle < 0.15f || angle > 2.6f || trim > std::min(before, after) * 0.45f)
        {
            rounded.push_back(corner);
            continue;
        }

        const float direction = incoming.x * outgoing.y - incoming.y * outgoing.x > 0 ? 1.0f : -1.0f;
        const Vec3 outward = (incoming - outgoing) / (incoming - outgoing).length2D();
        const float clearance = radius * (1 / std::cos(angle * 0.5f) - 1) + 0.3f;
        bool accepted = false;

        // First try rounding inside the original corridor. At a convex wall,
        // move the bend outwards to make room for the character's turning arc.
        for (float offset : {0.0f, clearance, clearance * 1.5f, clearance * 2.0f})
        {
            const Vec3 bend = corner + outward * offset;
            Vec3 entry = bend - incoming * trim, exit = bend + outgoing * trim;
            entry.z = corner.z + (previous.z - corner.z) * trim / before;
            exit.z = corner.z + (next.z - corner.z) * trim / after;
            const Vec3 center = entry + Vec3{-incoming.y, incoming.x, 0} * (direction * radius);
            const float start = std::atan2(entry.y - center.y, entry.x - center.x);
            const int steps = std::max(2, static_cast<int>(std::ceil(radius * angle / 0.8f)));
            std::vector<Vec3> curve{entry};

            for (int j = 1; j <= steps; ++j)
            {
                const float fraction = static_cast<float>(j) / steps;
                const float heading = start + direction * angle * fraction;
                curve.push_back({center.x + radius * std::cos(heading), center.y + radius * std::sin(heading), entry.z + (exit.z - entry.z) * fraction});
            }

            Vec3 test = previous;
            bool clear = true;

            for (const auto& point : curve)
            {
                if (!provider.canWalkStraight(test, point))
                {
                    clear = false;
                    break;
                }

                test = point;
            }

            if (!clear || !provider.canWalkStraight(test, next))
            {
                continue;
            }

            rounded.insert(rounded.end(), curve.begin(), curve.end());
            accepted = true;
            break;
        }

        if (!accepted)
        {
            rounded.push_back(corner);
        }
    }

    if (!path.points.empty())
    {
        rounded.push_back(path.points.back());
    }

    path.points.assign(rounded.begin() + 1, rounded.end());
    return path;
}

std::optional<ObstacleManeuver> maneuverFor(RecoveryStage stage)
{
    switch (stage)
    {
    case RecoveryStage::Jump:
        return ObstacleManeuver::Jump;
    case RecoveryStage::StrafeLeft:
        return ObstacleManeuver::StrafeLeft;
    case RecoveryStage::StrafeRight:
        return ObstacleManeuver::StrafeRight;
    default:
        return std::nullopt;
    }
}

} // namespace

std::string_view toString(NavStatus s)
{
    switch (s)
    {
    case NavStatus::Idle:
        return "Idle";
    case NavStatus::Moving:
        return "Moving";
    case NavStatus::Arrived:
        return "Arrived";
    case NavStatus::Recovering:
        return "Recovering";
    case NavStatus::Unreachable:
        return "Unreachable";
    }

    return "?";
}

Navigator::Navigator(INavigationProvider& provider, MovementController& movement, const IClock& clock, Logger& log, EventBus* events, NavigatorConfig config) : provider_(&provider), movement_(movement), clock_(clock), log_(log), events_(events), config_(config), stuck_(config.stuck), recovery_(movement, clock, log), lifts_(clock)
{
}

namespace
{

// A way that gets there: to within a few yards, on the same level.
bool reaches(const std::optional<Path>& path, const Vec3& to)
{
    return path && !path->points.empty() && distance2D(path->points.back(), to) <= 4.0f && std::fabs(path->points.back().z - to.z) <= 5.0f;
}

float lengthOf(const Vec3& from, const Path& path)
{
    float length = 0.0f;
    Vec3 previous = from;

    for (const auto& p : path.points)
    {
        length += distance(previous, p);
        previous = p;
    }

    return length;
}

// Lifts this far from a destination are looked at for a way to it.
constexpr float kLiftReach = 600.0f;
// What a lift trip costs over its length, as yards walked: waiting for the
// car and riding it, up to some 30 s.
constexpr float kLiftDetour = 200.0f;
// The car stands 5 s at a stop: gone on to it only in the first 2.
constexpr Duration kBoardWithin{2000};
constexpr Duration kLiftWaitLongest{90000};
constexpr Duration kStepOffLongest{6000};

} // namespace

std::optional<Navigator::LiftRide> Navigator::planLift(const GameSnapshot& snapshot, const Vec3& to)
{
    const Vec3& from = snapshot.player.position;
    const auto near = [&](const Lift& l) { return snapshot.mapId && l.mapId == *snapshot.mapId && std::hypot(l.x - to.x, l.y - to.y) <= kLiftReach; };

    if (std::none_of(lifts_.lifts().begin(), lifts_.lifts().end(), near))
    {
        return std::nullopt;
    }

    // A way the map has is taken unless the lift is much the shorter: up to
    // Freewind Post from below the map has one round by the slopes to the
    // west, some 700 yd, and the lift is a few steps off.
    float mapWay = std::numeric_limits<float>::max();

    if (const auto direct = provider_->findPath(from, to); reaches(direct, to))
    {
        mapWay = lengthOf(from, *direct);
    }

    std::optional<LiftRide> best;
    float bestLength = 0.0f;

    for (const Lift& lift : lifts_.lifts())
    {
        if (!near(lift))
        {
            continue;
        }

        // Got on where the character is nearer in height.
        const bool fromTop = std::fabs(from.z - lift.top) < std::fabs(from.z - lift.bottom);
        const Vec3 on = lift.stop(fromTop);
        const Vec3 off = lift.stop(!fromTop);

        // Ground by each stop at its height: the map has the edge of the
        // shaft, not its middle - and under the middle maybe the pit the car
        // goes down into (the Great Lift's, 5 yd under its floor below). Round
        // it then, though some of what is round it is cut off: beside the
        // Great Lift's cars below, a ledge between the two shafts.
        const auto groundBy = [&](const Vec3& stop)
        {
            std::vector<Vec3> grounds;
            const auto add = [&](const std::optional<Vec3>& g)
            {
                if (g && std::fabs(g->z - stop.z) <= 3.0f)
                {
                    grounds.push_back(*g);
                }
            };
            add(provider_->findWalkablePoint(stop, Lift::kFloor + 2.0f, 0.0f));

            for (int i = 0; i < 12; ++i)
            {
                const float h = static_cast<float>(i) * kTwoPi / 12;
                add(provider_->findWalkablePoint(stop + Vec3{std::cos(h), std::sin(h), 0.0f} * (Lift::kFloor + 1.0f), 2.0f, 0.0f));
            }

            return grounds;
        };

        std::optional<Vec3> onGround;
        std::optional<Path> toLift;

        for (const Vec3& g : groundBy(on))
        {
            if (auto way = provider_->findPath(from, g); reaches(way, g))
            {
                onGround = g;
                toLift = std::move(way);
                break;
            }
        }

        if (!onGround)
        {
            continue;
        }

        // The destination on the other level: as it is (a place with its
        // height), else the ground under it high over the other stop (a
        // vendor's place in the quest data, which has none).
        std::vector<Vec3> places{to};

        if (const auto below = provider_->groundBelow({to.x, to.y, off.z + 80.0f}))
        {
            places.push_back({to.x, to.y, *below});
        }

        if (const auto walkable = provider_->findWalkablePoint({to.x, to.y, off.z}, 40.0f))
        {
            places.push_back(*walkable);
        }

        std::optional<Path> fromLift;
        std::optional<Vec3> offGround;
        Vec3 there = to;

        for (const Vec3& g : groundBy(off))
        {
            for (const Vec3& place : places)
            {
                if (auto way = provider_->findPath(g, place); reaches(way, place))
                {
                    fromLift = std::move(way);
                    offGround = g;
                    there = place;
                    break;
                }
            }

            if (fromLift)
            {
                break;
            }
        }

        if (!fromLift)
        {
            continue;
        }

        const float length = lengthOf(from, *toLift) + lengthOf(*offGround, *fromLift);

        if ((!best || length < bestLength) && length + kLiftDetour < mapWay)
        {
            // Off the car's floor, on the side the ground is: it comes down
            // onto where it stops. Stepped off the same way at the other stop.
            const auto outward = [](const Vec3& middle, const Vec3& ground, const Vec3& otherwise)
            {
                Vec3 d{ground.x - middle.x, ground.y - middle.y, 0.0f};
                float across = std::hypot(d.x, d.y);

                if (across < 0.5f)
                {
                    d = {otherwise.x - middle.x, otherwise.y - middle.y, 0.0f};
                    across = std::max(0.5f, std::hypot(d.x, d.y));
                }

                return d * (1.0f / across);
            };
            const Vec3 waitDir = outward(on, *onGround, from);
            const Vec3 offDir = outward(off, *offGround, there);
            Vec3 waitAt = *onGround;

            if (const auto beside = provider_->findWalkablePoint(on + waitDir * (Lift::kFloor + 3.0f), 3.0f, 0.0f); beside && std::fabs(beside->z - on.z) <= 3.0f && reaches(provider_->findPath(from, *beside), *beside))
            {
                waitAt = *beside;
            }

            best = LiftRide{lift, fromTop, there};
            best->waitAt = waitAt;
            best->stepOffTo = Vec3{off.x, off.y, off.z} + offDir * (Lift::kFloor + 3.0f);
            best->exitHeading = headingTo(off, best->stepOffTo);

            // The way off: round the other stop every 5 degrees, how far out
            // the map has ground at its height. The farthest, and of those the
            // middle of the widest run of headings (on a plank, its middle) -
            // the one towards the destination where runs are alike. Ground
            // cut off from the way on is none.
            constexpr int kSteps = 72;
            std::array<float, kSteps> reach{};

            for (int i = 0; i < kSteps; ++i)
            {
                const float h = static_cast<float>(i) * kTwoPi / kSteps;
                const Vec3 d{std::cos(h), std::sin(h), 0.0f};
                std::optional<Vec3> last;

                for (float out = Lift::kFloor + 1.0f; out <= 16.0f; out += 2.0f)
                {
                    const auto g = provider_->findWalkablePoint(off + d * out, 1.0f, 0.0f);

                    if (!g || std::fabs(g->z - off.z) > 3.0f)
                    {
                        break;
                    }

                    reach[i] = out;
                    last = g;
                }

                if (last && !reaches(provider_->findPath(*last, *offGround), *offGround))
                {
                    reach[i] = 0.0f;
                }
            }

            const float farthest = *std::max_element(reach.begin(), reach.end());
            float bestToward = 0.0f;
            int bestRun = 0;

            for (int i = 0; i < kSteps && farthest > 0.0f; ++i)
            {
                // A run starts where the one before is short of the farthest.
                if (reach[i] < farthest || reach[(i + kSteps - 1) % kSteps] >= farthest)
                {
                    continue;
                }

                int run = 0;

                while (run < kSteps && reach[(i + run) % kSteps] >= farthest)
                {
                    ++run;
                }

                const float h = (static_cast<float>(i) + static_cast<float>(run - 1) * 0.5f) * kTwoPi / kSteps;
                const float toward = distance2D(off + Vec3{std::cos(h), std::sin(h), 0.0f} * farthest, there);

                if (run > bestRun || (run == bestRun && toward < bestToward))
                {
                    bestRun = run;
                    bestToward = toward;
                    best->exitHeading = normalizeHeading(h);
                    best->exitReach = farthest;
                }
            }

            // Ground all round (or none known): the heading towards the destination.
            if (bestRun == 0 && farthest > 0.0f)
            {
                best->exitHeading = headingTo(off, there);
                best->exitReach = farthest;
            }
            bestLength = length;
        }
    }

    return best;
}

std::optional<NavStatus> Navigator::rideLift(const GameSnapshot& snapshot, Vec3& destination, bool exact)
{
    lifts_.update(snapshot);

    if (!provider_->usesClientGeometry() || exact)
    {
        return std::nullopt;
    }

    // Where the lift took it for this destination: the same place on its level.
    if (liftTo_ && distance(destination, liftTo_->first) <= 1.0f)
    {
        destination = liftTo_->second;
    }

    // Riding, the car is not got off for another destination.
    if (ride_ && ride_->stage != LiftRide::Stage::Riding && distance(destination, ride_->to) > 5.0f)
    {
        ride_.reset();
    }

    if (!ride_ && (!liftCheckedFor_ || distance(*liftCheckedFor_, destination) > 5.0f || liftsChecked_ != lifts_.revision()))
    {
        if (liftsChecked_ != lifts_.revision() && liftCheckedFor_)
        {
            log_.info(logcat::Move, "a lift learnt on the way - looking at the way to {} again", destination);
        }

        liftCheckedFor_ = destination;
        liftsChecked_ = lifts_.revision();

        if (auto ride = planLift(snapshot, destination))
        {
            log_.info(logcat::Move, "the way to {} goes by the lift at ({:.0f}, {:.0f}), {} - to its stop at {}, off it heading {:.0f} degrees, ground {:.0f} yd out", destination, ride->lift.x, ride->lift.y, ride->fromTop ? "down" : "up", ride->lift.stop(ride->fromTop),
                      ride->exitHeading * 180.0f / kPi, ride->exitReach);
            liftTo_ = std::pair{destination, ride->to};
            liftCheckedFor_ = ride->to;
            destination = ride->to;
            ride->since = clock_.now();
            ride_ = std::move(ride);
            clearPath();
        }
    }

    if (!ride_)
    {
        return std::nullopt;
    }

    LiftRide& r = *ride_;
    const TimePoint now = clock_.now();
    const Vec3& pos = snapshot.player.position;
    const Vec3 on = r.lift.stop(r.fromTop);
    const Vec3 off = r.lift.stop(!r.fromTop);
    const auto car = lifts_.car(r.lift);
    const auto at = [&](const Vec3& stop) { return car && !car->moving && std::fabs(car->z - stop.z) <= 1.5f; };
    // On a car the client tells where on it, a few yards from its middle -
    // not where in the world, some way off from it.
    const auto onCar = [&](const Vec3& p) { return std::hypot(p.x, p.y) <= Lift::kFloor + 3.0f && std::fabs(p.z) <= 5.0f && distance2D(p, on) > 100.0f; };
    const auto giveUp = [&](std::string_view why)
    {
        log_.warn(logcat::Move, "giving up the lift at ({:.0f}, {:.0f}): {}", r.lift.x, r.lift.y, why);
        ride_.reset();
        movement_.stop();
        return std::optional<NavStatus>{NavStatus::Unreachable};
    };
    const auto stage = [&](LiftRide::Stage next, std::string_view what)
    {
        r.stage = next;
        r.since = now;
        log_.info(logcat::Move, "lift at ({:.0f}, {:.0f}): {}", r.lift.x, r.lift.y, what);
    };

    switch (r.stage)
    {
    case LiftRide::Stage::ToStop:
    {
        const NavStatus status = moveFlat(snapshot, r.waitAt, 1.5f, true, false, false);

        if (status == NavStatus::Unreachable)
        {
            return giveUp("no way to its stop");
        }

        if (status == NavStatus::Arrived || distance2D(pos, r.waitAt) <= 2.0f)
        {
            stop();
            stage(LiftRide::Stage::Waiting, "waiting for the car");
        }

        return NavStatus::Moving;
    }
    case LiftRide::Stage::Waiting:
        halt();
        movement_.face(snapshot.player, on, 0.3f);

        if (at(on) && car->still <= kBoardWithin)
        {
            stage(LiftRide::Stage::Boarding, "the car is here - getting on");
        }
        else if (now - r.since > kLiftWaitLongest)
        {
            return giveUp("the car did not come");
        }

        return NavStatus::Moving;
    case LiftRide::Stage::Boarding:
        // On the car: the client tells where on it, not where in the world.
        if (onCar(pos))
        {
            stop();
            stage(LiftRide::Stage::Riding, "on the car - riding");
            return NavStatus::Moving;
        }

        if (!at(on))
        {
            // Gone with the bot on its floor: riding.
            if (car && car->moving && distance2D(pos, on) <= Lift::kFloor && std::fabs(pos.z - on.z) <= 3.0f)
            {
                stop();
                stage(LiftRide::Stage::Riding, "on the car as it left - riding");
                return NavStatus::Moving;
            }

            // Gone without it: never into the shaft without the car in it - a
            // pit lies under one of Thunder Bluff's.
            stop();
            stage(LiftRide::Stage::ToStop, "the car left before the bot was on - waiting for the next");
            return NavStatus::Moving;
        }

        if (distance2D(pos, on) <= 1.5f)
        {
            stop();
            stage(LiftRide::Stage::Riding, "on the car - riding");
            return NavStatus::Moving;
        }

        movement_.faceWhileMoving(snapshot.player, on, 0.3f);
        movement_.setForward(true);
        return NavStatus::Moving;
    case LiftRide::Stage::Riding:
        // Turned already the way off the car at the other stop.
        movement_.faceHeading(snapshot.player, r.exitHeading, 0.1f);

        // Where the bot got on, on the ground beside the shaft, while the car
        // is gone: it was not on it.
        if (!onCar(pos) && distance2D(pos, on) > Lift::kFloor + 1.0f && std::fabs(pos.z - on.z) <= 3.0f && car && !at(on))
        {
            stage(LiftRide::Stage::ToStop, "not on the car - waiting for the next");
            return NavStatus::Moving;
        }

        if (at(off))
        {
            stage(LiftRide::Stage::Leaving, "at the other stop - stepping off");
        }
        else if (now - r.since > kLiftWaitLongest)
        {
            return giveUp("the car did not go");
        }

        return NavStatus::Moving;
    case LiftRide::Stage::Leaving:
    {
        const bool told = !onCar(pos) && std::fabs(pos.z - off.z) <= 3.0f;

        // Fallen off the edge: the other level below.
        if (!onCar(pos) && pos.z < off.z - 6.0f && std::fabs(pos.z - on.z) > 3.0f)
        {
            return giveUp("fell off stepping off the car");
        }

        // Still where it got on, a while after stepping off: it never went
        // with the car. Until then, the client may yet be telling where it got
        // on, as some do while riding.
        if (!onCar(pos) && std::fabs(pos.z - on.z) <= 3.0f && std::fabs(on.z - off.z) > 6.0f && now - r.since > kStepOffLongest)
        {
            movement_.setForward(false);
            stage(LiftRide::Stage::ToStop, "still at the stop it got on at - waiting for the next");
            return NavStatus::Moving;
        }

        // Straight along the way off, the heading kept whatever the bot's
        // place on the car is taken for, until well out on the map.
        const Vec3 along{std::cos(r.exitHeading), std::sin(r.exitHeading), 0.0f};
        const float wellOff = std::max(Lift::kFloor + 3.0f, std::min(r.exitReach - 1.0f, 12.0f));

        if (told && distance2D(pos, off) >= wellOff)
        {
            log_.info(logcat::Move, "off the lift at ({:.0f}, {:.0f}) - on to {}", r.lift.x, r.lift.y, r.to);
            ride_.reset();
            movement_.setForward(false);
            clearPath();
            stuck_.reset();
            return std::nullopt;
        }

        const bool facing = movement_.faceWhileMoving(snapshot.player, snapshot.player.position + along * 100.0f, 0.15f);
        movement_.setForward(facing || movement_.isMovingForward());

        if (now - r.since > kStepOffLongest * 2)
        {
            return giveUp("not off the car at the other stop");
        }

        return NavStatus::Moving;
    }
    }

    return std::nullopt;
}

float Navigator::estimatedRunSpeed() const
{
    return runSpeedEstimate_ > 1.0f ? runSpeedEstimate_ : movement_.config().runSpeed;
}

void Navigator::clearPath()
{
    destination_.reset();
    path_.points.clear();
    pathIndex_ = 0;
}

void Navigator::rememberJam(const Vec3& at, const Vec3& along)
{
    constexpr float kRadius = 3.0f;
    constexpr Duration kFor{300000};
    constexpr std::size_t kMost = 12;
    // Ahead of the bot, which stands just outside it: a zone stood in does
    // not count for the way out of it.
    const Vec3 center = at + along * (kRadius + 1.0f);
    const TimePoint now = clock_.now();
    std::erase_if(jamZones_, [&](const JamZone& jam) { return now >= jam.until || distance2D(jam.zone.center, center) < kRadius; });

    if (jamZones_.size() >= kMost)
    {
        jamZones_.erase(jamZones_.begin());
    }

    jamZones_.push_back({AvoidZone{center, kRadius, "what the bot got stuck on"}, now + kFor});
}

std::vector<AvoidZone> Navigator::avoidedZones() const
{
    std::vector<AvoidZone> zones = packZones_;
    const TimePoint now = clock_.now();

    for (const auto& jam : jamZones_)
    {
        if (now < jam.until)
        {
            zones.push_back(jam.zone);
        }
    }

    if (!obstacles_)
    {
        return zones;
    }

    const auto& noGo = obstacles_->noGoZones();
    const auto& learned = obstacles_->avoidZones();
    zones.insert(zones.end(), noGo.begin(), noGo.end());
    zones.insert(zones.end(), learned.begin(), learned.end());
    return zones;
}

void Navigator::setMobZones(std::vector<AvoidZone> zones)
{
    mobZones_ = std::move(zones);
}

const AvoidZone* Navigator::mobZoneOnPath(const Vec3& from) const
{
    if (mobZones_.empty() || !destination_ || path_.points.empty() || exact_ || pathPastMobs_)
    {
        return nullptr;
    }

    // A way just planned round mobs is kept a moment: wandering mobs crossed
    // each new one at once, and the bot turned this way and that.
    if (walkingPastMobs() || clock_.now() - lastReroute_ < kRerouteSettle)
    {
        return nullptr;
    }

    for (const auto& z : mobZones_)
    {
        if (distance2D(from, z.center) <= z.radius || distance2D(*destination_, z.center) <= z.radius)
        {
            continue;
        }

        // On another level than both ends of the way: a floor under it, or over.
        if (z.apart(from.z, destination_->z))
        {
            continue;
        }

        Vec3 previous = from;

        for (std::size_t i = pathIndex_; i < path_.points.size(); ++i)
        {
            if (!clearOfZones(previous, path_.points[i], std::span(&z, 1)))
            {
                return &z;
            }

            previous = path_.points[i];
        }
    }

    return nullptr;
}

void Navigator::keepSides(const Vec3& from, const Vec3& to)
{
    const auto previousSides = keptSidesTo_ && distance(*keptSidesTo_, to) <= 0.05f ? keptSides_ : std::vector<KeptSide>{};
    keptSides_.clear();
    keptSidesTo_ = to;
    const Vec3 along{to.x - from.x, to.y - from.y, 0.0f};
    const float length = std::hypot(along.x, along.y);

    if (path_.points.empty() || exact_ || length < 1.0f)
    {
        return;
    }

    const auto remember = [&](const AvoidZone& z, bool pack)
    {
        // Only remember zones actually kept clear of on this route.
        if (distance2D(from, z.center) <= z.radius || distance2D(to, z.center) <= z.radius)
        {
            return;
        }
        Vec3 nearest = from;
        Vec3 previous = from;
        for (const Vec3& point : path_.points)
        {
            if (!clearOfZones(previous, point, std::span(&z, 1)))
            {
                return;
            }
            const Vec3 leg = point - previous;
            const float leg2 = leg.x * leg.x + leg.y * leg.y;
            const float t = leg2 > 0 ? std::clamp(((z.center.x - previous.x) * leg.x + (z.center.y - previous.y) * leg.y) / leg2, 0.0f, 1.0f) : 0.0f;
            const Vec3 at = previous + leg * t;
            if (distance2D(at, z.center) < distance2D(nearest, z.center))
            {
                nearest = at;
            }
            previous = point;
        }
        if (distance2D(nearest, z.center) > z.radius + 15.0f)
        {
            return;
        }

        // Match the nearest previous circle, not the first pack member in
        // iteration order. Keep its original side as the player turns round it.
        const KeptSide* matched = nullptr;
        float nearestMatch = 12.0f;
        for (const auto& kept : previousSides)
        {
            const float d = distance2D(kept.center, z.center);
            if (kept.pack == pack && d <= nearestMatch)
            {
                matched = &kept;
                nearestMatch = d;
            }
        }
        if (matched)
        {
            keptSides_.push_back({z.center, matched->towardSide, pack});
            return;
        }

        const Vec3 left{-along.y / length, along.x / length, 0.0f};
        const float side = left.x * (nearest.x - z.center.x) + left.y * (nearest.y - z.center.y);
        if (std::abs(side) >= 2.0f)
        {
            keptSides_.push_back({z.center, left * (side > 0 ? 1.0f : -1.0f), pack});
        }
    };
    for (const auto& z : packZones_)
    {
        remember(z, true);
    }
    if (!pathPastMobs_ && !walkingPastMobs())
    {
        for (const auto& z : mobZones_)
        {
            remember(z, false);
        }
    }
}

std::vector<AvoidZone> Navigator::otherSides(const Vec3& from, const Vec3& to) const
{
    std::vector<AvoidZone> out;
    if (!keptSidesTo_ || distance(*keptSidesTo_, to) > 0.05f || exact_)
    {
        return out;
    }

    const auto closeOtherSide = [&](const AvoidZone& z, bool pack)
    {
        const KeptSide* matched = nullptr;
        float nearestMatch = 12.0f;
        for (const auto& kept : keptSides_)
        {
            const float d = distance2D(kept.center, z.center);
            if (kept.pack == pack && d <= nearestMatch)
            {
                matched = &kept;
                nearestMatch = d;
            }
        }
        if (matched)
        {
            const AvoidZone closed{z.center - matched->towardSide * (z.radius * 1.5f), z.radius * 1.5f, z.name};
            // A preference must not prevent leaving where we already stand
            // or reaching the destination as we finish passing the group.
            if (distance2D(from, closed.center) > closed.radius && distance2D(to, closed.center) > closed.radius)
            {
                out.push_back(closed);
            }
        }
    };
    for (const auto& z : packZones_)
    {
        closeOtherSide(z, true);
    }
    if (!walkingPastMobs())
    {
        for (const auto& z : mobZones_)
        {
            closeOtherSide(z, false);
        }
    }
    return out;
}

bool Navigator::canStandAt(const Vec3& point) const
{
    return clearOfZones(point, point, avoidedZones(), config_.cornerRadius + 1.0f);
}

void Navigator::noteReroute(const Vec3& to)
{
    // Only a new way to the very same place counts: one to a mob that moves
    // (chasing it) is planned every tick, round mobs or not.
    if (mobZones_.empty() || !destination_ || distance(*destination_, to) > 0.05f || walkingPastMobs())
    {
        return;
    }

    const TimePoint now = clock_.now();
    lastReroute_ = now;
    std::erase_if(reroutes_, [&](TimePoint at) { return now - at > kReroutesWithin; });
    reroutes_.push_back(now);

    if (reroutes_.size() < kReroutesBeforePassing)
    {
        return;
    }

    reroutes_.clear();
    pastMobsUntil_ = now + kPastMobsFor;
    log_.info(logcat::Move, "mobs keep crossing the way to {} - walking past them for {} seconds", to, std::chrono::duration_cast<std::chrono::seconds>(kPastMobsFor).count());
}

bool Navigator::crossedBefore(const AvoidZone& zone, const Vec3& to)
{
    const TimePoint now = clock_.now();

    if (!crossedTo_ || distance(*crossedTo_, to) > kSameMob)
    {
        crossed_.clear();
        crossedTo_ = to;
    }

    std::erase_if(crossed_, [&](const auto& c) { return now - c.second > kCrossedWithin; });

    if (std::any_of(crossed_.begin(), crossed_.end(), [&](const auto& c) { return distance2D(c.first, zone.center) <= kSameMob; }))
    {
        crossed_.clear();
        return true;
    }

    crossed_.emplace_back(zone.center, now);
    return false;
}

bool Navigator::preparePath(const GameSnapshot& snapshot, const Vec3& to)
{
    prepared_.reset();
    const auto& player = snapshot.player;

    if (!movement_.isMovingForward() || recovering() || exact_ || intoZone_ || distance2D(player.position, to) < 4.0f || !provider_->canPlanWithoutLoading(player.position, to))
    {
        return false;
    }

    const auto zones = pathZones(player.position, to);
    // Follow the keys currently held, including a turn. Every small step must
    // remain walkable: never carry motion through a tree while planning past it.
    Vec3 ahead = player.position;
    float heading = player.heading;
    const float turn = movement_.turnDirection() * movement_.estimatedTurnRate() * 0.05f;

    for (int step = 0; step < 5; ++step)
    {
        const Vec3 next = ahead + directionFromHeading(heading + turn * 0.5f) * (estimatedRunSpeed() * 0.05f);

        if (!clearOfZones(ahead, next, zones) || (provider_->usesClientGeometry() && !provider_->canWalkStraight(ahead, next)))
        {
            return false;
        }

        ahead = next;
        heading += turn;
    }

    if (!provider_->canPlanWithoutLoading(ahead, to))
    {
        return false;
    }

    auto path = provider_->findLoadedPath(ahead, to);

    if (!path || path->points.empty())
    {
        return false;
    }

    if (std::abs(headingDelta(heading, ahead, path->points.front())) > 1.75f)
    {
        return false;
    }

    Vec3 previous = ahead;

    for (const auto& point : path->points)
    {
        if (!clearOfZones(previous, point, zones))
        {
            return false;
        }

        previous = point;
    }

    path->points.insert(path->points.begin(), ahead);
    prepared_ = PreparedPath{player.position, to, std::move(*path), clock_.now(), snapshot.mapId, obstacles_ ? obstacles_->revision() : 0};
    return true;
}

bool Navigator::computePath(const PlayerState& player, const Vec3& to)
{
    noteReroute(to);

    if (const auto closed = otherSides(player.position, to); !closed.empty())
    {
        const bool planned = planPath(player, to, closed);
        bool keptSide = planned;
        Vec3 previous = player.position;
        for (const auto& point : path_.points)
        {
            keptSide = keptSide && clearOfZones(previous, point, closed);
            previous = point;
        }
        if (keptSide)
        {
            keepSides(player.position, to);
            plannedTo_ = to;
            return true;
        }

        // Map repair/fallback must not silently discard the preference. Only
        // when that side cannot be planned do we commit to a different side.
        log_.debug(logcat::Move, "the chosen side of the mobs or packs has no verified route to {} - trying another side", to);
        keptSides_.clear();
        keptSidesTo_.reset();
    }

    const bool planned = planPath(player, to);

    if (planned)
    {
        keepSides(player.position, to);
        plannedTo_ = to;
    }

    return planned;
}

bool Navigator::planPath(const PlayerState& player, const Vec3& to, const std::vector<AvoidZone>& preferredSides)
{
    hoverSince_.reset();
    const Vec3& from = player.position;
    // Side preferences are separate from soft mob avoidance: the temporary
    // "walk past mobs" fallback must still keep the chosen side of hard packs.
    const auto planningZones = [&](const Vec3& start, const Vec3& end)
    {
        auto zones = pathZones(start, end);
        zones.insert(zones.end(), preferredSides.begin(), preferredSides.end());
        return zones;
    };
    const ObstacleMemory::Calculation calculation(obstacles_);
    pathExact_ = exact_;
    pathIntoZone_ = intoZone_;

    if (exact_ && !provider_->usesClientGeometry())
    {
        path_ = Path{{to}};
        pathRevision_ = obstacles_ ? obstacles_->revision() : 0;
        pathIndex_ = 0;
        destination_ = to;
        return true;
    }

    // Keep Forward held through nearby circuit points and small updates to a
    // moving target. Releasing it at every replan also triggers the key's
    // repress delay, turning a continuous chase into repeated short steps.
    std::optional<Path> ready;

    if (prepared_)
    {
        if (!exact_ && !intoZone_ && distance(prepared_->to, to) <= 0.05f && clock_.now() - prepared_->at <= Duration(500) && distance(player.position, prepared_->from) <= 2.0f && prepared_->revision == (obstacles_ ? obstacles_->revision() : 0) && (!provider_->usesClientGeometry() || provider_->canWalkStraight(player.position, prepared_->path.points.front())))
        {
            ready = std::move(prepared_->path);
        }

        prepared_.reset();
    }

    if (provider_->usesClientGeometry() && !ready)
    {
        const bool targetDrift = destination_ && !path_.points.empty() && distance(*destination_, to) <= config_.repathDistance;
        // At the last destination and on to the next, however far: quest
        // mode's places its mobs are at. Let go there, the character stood
        // still a moment at each, the key's repress delay on top of the plan.
        const bool reachedLast = destination_ && distance2D(*destination_, from) <= 4.0f;
        const bool nextNearbyPoint = reachedLast && distance2D(from, to) <= 12.0f && std::abs(to.z - from.z) <= 3.0f;
        Vec3 continueToward = to;

        if (targetDrift && pathIndex_ < path_.points.size())
        {
            continueToward = path_.points[pathIndex_];
        }

        const float currentHeading = player.heading;
        const float delta = headingDelta(currentHeading, from, continueToward);
        const auto zones = planningZones(from, continueToward);
        const float turnRadius = estimatedRunSpeed() / movement_.estimatedTurnRate();
        const float arc = std::min(std::abs(delta), movement_.estimatedTurnRate() * 0.65f);
        const float direction = delta >= 0.0f ? 1.0f : -1.0f;
        Vec3 previous = from;
        bool clearArc = std::abs(delta) <= 100.0f * kPi / 180.0f;

        if (arc < 0.02f)
        {
            const Vec3 next = from + directionFromHeading(currentHeading) * 2.0f;
            clearArc = clearArc && provider_->canWalkStraight(from, next) && clearOfZones(from, next, zones);
        }

        for (int sample = 1; clearArc && arc >= 0.02f && sample <= 5; ++sample)
        {
            const float heading = currentHeading + direction * arc * sample / 5;
            const Vec3 next = from + Vec3{direction * turnRadius * (std::sin(heading) - std::sin(currentHeading)), direction * turnRadius * (std::cos(currentHeading) - std::cos(heading)), 0};


            if (!walkableArcSegment(*provider_, previous, next) || !clearOfZones(previous, next, zones))
            {
                clearArc = false;
            }

            previous = next;
        }

        const bool safeContinuation = (targetDrift || nextNearbyPoint || reachedLast) && clearArc && provider_->canPlanWithoutLoading(from, to);

        if (!safeContinuation)
        {
            movement_.setForward(false);
        }
    }

    auto path = ready ? std::move(ready) : provider_->findPath(from, to);

    if (!path || path->points.empty())
    {
        log_.warn(logcat::Move, "no path from {} to {} ({})", from, to, provider_->name());
        clearPath();
        return false;
    }

    // A way round a mob kept as the map has it, through a wall otherwise.
    bool keptPastMobs = false;
    const auto around = [&](const std::vector<AvoidZone>& zones) -> std::optional<Path>
    {
        if (zones.empty())
        {
            return path;
        }

        Path safe;
        Vec3 previous = from;
        std::size_t done = 0;
        // The map's own points skipped for lying in a zone: put back where
        // the way round them would go through a wall.
        std::vector<Vec3> skipped;

        for (std::size_t i = 0; i < path->points.size(); ++i)
        {
            const Vec3& point = path->points[i];

            // The client nav route may contain walkable samples through a
            // learned dynamic obstacle (such as a mailbox). Those samples
            // cannot be A* endpoints; retain the last outside sample instead.
            if (i + 1 < path->points.size() && std::any_of(zones.begin(), zones.end(), [&](const AvoidZone& zone) { return !zone.apart(point.z, point.z) && distance2D(point, zone.center) <= zone.radius + config_.cornerRadius + 1.0f; }))
            {
                skipped.push_back(point);
                continue;
            }

            auto leg = pathAroundZones(previous, point, zones, config_.cornerRadius + 1.0f, 3.0f, obstacles_ ? obstacles_->stopToken() : std::stop_token{}, [&](int percent) { calculation.report(static_cast<int>((done * 100 + percent) / path->points.size())); });

            if (!leg)
            {
                return std::nullopt;
            }

            // The way round is drawn round circles, not by the map: on the
            // client's map it is walked only where it is clear of walls. Kul
            // Tiras Marines by the gate of Tiragarde Keep took the gate's
            // points out of the way, and the bot walked into the keep's wall
            // beside it. Through a wall, the map's way is kept - past the
            // mob, which is pulled on the way (ClearPath).
            if (provider_->usesClientGeometry() && (leg->points.size() != 1 || !skipped.empty()))
            {
                Vec3 at = previous;
                bool clear = true;

                for (const Vec3& next : leg->points)
                {
                    clear = clear && provider_->canWalkStraight(at, next);
                    at = next;
                }

                // The map's points were left out for lying in a zone: put back
                // only clear of the No Go zones and the learned obstacles -
                // put back in one, the way went through a No Go zone drawn in
                // the profile, and was planned again there every tick.
                std::vector<Vec3> kept = skipped;
                kept.push_back(point);
                const auto fixed = avoidedZones();
                Vec3 keptFrom = previous;
                bool keptClear = true;

                for (const Vec3& next : kept)
                {
                    keptClear = keptClear && clearOfZones(keptFrom, next, fixed);
                    keptFrom = next;
                }

                // A piece of the way round that cannot be walked straight - a
                // tree, a slope, a wall - is asked of the map, piece by piece,
                // and kept where the map's way stays clear of the zones too.
                // Only with no such way is the map's own way kept, past the
                // mobs: on open ground near a pack of five, one tree made the
                // bot walk right through the pack.
                std::optional<std::vector<Vec3>> repaired;

                if (!clear)
                {
                    repaired.emplace();
                    Vec3 at2 = previous;

                    for (const Vec3& next : leg->points)
                    {
                        if (provider_->canWalkStraight(at2, next))
                        {
                            repaired->push_back(next);
                        }
                        else if (auto piece = provider_->findPath(at2, next); piece && !piece->points.empty())
                        {
                            Vec3 from2 = at2;
                            bool pieceClear = true;

                            for (const Vec3& q : piece->points)
                            {
                                pieceClear = pieceClear && clearOfZones(from2, q, zones);
                                from2 = q;
                            }

                            if (!pieceClear)
                            {
                                repaired.reset();
                                break;
                            }

                            repaired->insert(repaired->end(), piece->points.begin(), piece->points.end());
                        }
                        else
                        {
                            repaired.reset();
                            break;
                        }

                        at2 = next;
                    }
                }

                if (repaired)
                {
                    leg->points = std::move(*repaired);
                }
                else if (!clear && keptClear)
                {
                    leg->points = std::move(kept);
                    keptPastMobs = true;
                }
                else if (!clear)
                {
                    // Neither the detour nor the original map leg can be
                    // used. Do not keep an unwalkable same-side detour.
                    return std::nullopt;
                }
            }

            skipped.clear();
            ++done;
            calculation.report(static_cast<int>(done * 100 / path->points.size()));
            safe.points.insert(safe.points.end(), leg->points.begin(), leg->points.end());
            previous = point;
        }

        safe.continues = path->continues;
        return safe;
    };

    if (provider_->usesClientGeometry())
    {
        auto safe = around(planningZones(from, to));

        // The way round a zone is drawn round its circle, and mended by the
        // map piece by piece. Where that comes to nothing - the circle's edge
        // in a lake, up a slope, and the map's pieces back through the zone -
        // the map is asked for a whole way that keeps out of the zones. The
        // bot stood still otherwise, every place it could go "blocked by an
        // avoidance zone", with open ground to walk round on.
        const auto byTheMap = [&]
        {
            const auto zones = planningZones(from, to);

            if (!provider_->canAvoid() || zones.empty())
            {
                return false;
            }

            std::vector<INavigationProvider::Avoided> circles;

            for (const AvoidZone& zone : zones)
            {
                const float radius = zone.radius + config_.cornerRadius + 1.0f;

                // Not one the bot stands at the edge of: closed, it leaves no
                // ground to set out on. On a bridge, with what the bot jammed
                // on three yards ahead, every way began and ended on the
                // polygon it stood on.
                if (distance2D(from, zone.center) > radius + 4.0f)
                {
                    circles.push_back({zone.center, radius, zone.reach});
                }
            }

            if (circles.empty())
            {
                return false;
            }

            provider_->setAvoiding(std::move(circles));
            auto other = provider_->findPath(from, to);
            provider_->setAvoiding({});

            if (!other || other->points.empty())
            {
                return false;
            }

            const bool past = std::exchange(keptPastMobs, false);
            auto mapsOwn = std::exchange(path, std::move(other));
            auto way = around(zones);

            if (way && !way->points.empty() && !keptPastMobs)
            {
                safe = std::move(way);
                log_.info(logcat::Move, "no way round what is avoided close by - the map's own way round it to {}", to);
                return true;
            }

            path = std::move(mapsOwn);
            keptPastMobs = past;
            return false;
        };

        if (!safe || safe->points.empty() || keptPastMobs)
        {
            byTheMap();
        }

        bool passedMobs = keptPastMobs;

        if (keptPastMobs)
        {
            log_.debug(logcat::Move, "no way round the mobs by {} clear of walls - past them, as the map goes", to);
        }

        if (!safe && !mobZones_.empty())
        {
            const auto keep = std::exchange(mobZones_, {});
            safe = around(planningZones(from, to));

            // Past the mobs, then, but still round the packs and the No Go zones.
            if (!safe || safe->points.empty())
            {
                byTheMap();
            }

            mobZones_ = keep;

            if (safe)
            {
                passedMobs = true;
                log_.debug(logcat::Move, "no way round nearby mobs to {} - continuing toward the objective", to);
            }
        }

        // No way but past where the bot jammed (a doorway, a bridge): the
        // map's way again, and the recovery moves see to the rest.
        if ((!safe || safe->points.empty()) && !jamZones_.empty())
        {
            auto kept = std::exchange(jamZones_, {});
            safe = around(planningZones(from, to));

            if (safe && !safe->points.empty())
            {
                log_.debug(logcat::Stuck, "no way to {} round where the bot got stuck - the same way again", to);
            }
            else
            {
                // Something else is in the way: they were not what blocked it.
                jamZones_ = std::move(kept);
            }
        }

        if (!safe || safe->points.empty())
        {
            log_.warn(logcat::Move, "client route is blocked by an avoidance zone from {} to {}", from, to);
            clearPath();
            return false;
        }

        if (!exact_)
        {
            const auto zones = planningZones(from, to);
            *safe = spreadFromWalls(from, std::move(*safe), *provider_, zones, config_.wallClearance, estimatedRunSpeed() / movement_.estimatedTurnRate(), wallRandom_);
            // The destination to the nearest 8 yards: one trip, one seed.
            const auto cell = [](float v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(v / 8.0f))); };
            *safe = swayLongLegs(from, std::move(*safe), *provider_, zones, sway_, swaySeed_ ^ (cell(to.x) * 73856093u) ^ (cell(to.y) * 19349663u));
        }

        path_ = roundClientCorners(from, std::move(*safe), *provider_, estimatedRunSpeed() / movement_.estimatedTurnRate());
        pathRevision_ = obstacles_ ? obstacles_->revision() : 0;
        pathIndex_ = 0;
        destination_ = to;
        pathPastMobs_ = passedMobs;
        return true;
    }

    auto safe = around(planningZones(from, to));
    pathPastMobs_ = false;

    // No way round a strong mob: past it as before, rather than not at all.
    if (!safe && !mobZones_.empty())
    {
        const auto keep = std::exchange(mobZones_, {});
        safe = around(planningZones(from, to));
        mobZones_ = keep;
        pathPastMobs_ = safe.has_value();

        if (safe)
        {
            log_.debug(logcat::Move, "no way round the strong mobs to {} - going past them", to);
        }
    }

    if (!safe)
    {
        log_.warn(logcat::Move, "no route clear of obstacles from {} to {}", from, to);
        clearPath();
        return false;
    }

    path_ = std::move(*safe);
    pathRevision_ = obstacles_ ? obstacles_->revision() : 0;
    pathIndex_ = 0;
    destination_ = to;
    log_.debug(logcat::Move, "path to {} distance={:.1f} nodes={}", to, distance(from, to), path_.points.size());
    return true;
}

void Navigator::stop()
{
    prepared_.reset();
    headwayAt_.reset();
    recovery_.cancel();
    jamSince_.reset();
    retreatTo_.reset();
    retreatCount_ = 0;
    currentGoal_.reset();
    movement_.stop();
    stuck_.reset();
    strafeAhead_ = 0;
    clearPath();
    going_ = false;
}

void Navigator::halt()
{
    if (going_)
    {
        headwayAt_.reset();
        recovery_.cancel();
        jamSince_.reset();
        retreatTo_.reset();
        retreatCount_ = 0;
        currentGoal_.reset();
        movement_.setForward(false);
        movement_.setBackward(false);
        movement_.setStrafeLeft(false);
        movement_.setStrafeRight(false);
        movement_.releaseVertical();
        stuck_.reset();
        strafeAhead_ = 0;
        clearPath();
        going_ = false;
    }
}

float Navigator::travelHeading(const PlayerState& player) const
{
    if (pathIndex_ < path_.points.size() && distance2D(player.position, path_.points[pathIndex_]) > 0.5f)
    {
        return headingTo(player.position, path_.points[pathIndex_]);
    }

    return player.heading;
}

void Navigator::watchObstacleMoves(const GameSnapshot& snapshot)
{
    if (!obstacles_)
    {
        return;
    }

    const TimePoint now = clock_.now();
    const Vec3& at = snapshot.player.position;

    if (candidate_)
    {
        const Vec3 moved = at - candidate_->at;
        const Vec3 along = directionFromHeading(candidate_->heading);
        const float ahead = moved.x * along.x + moved.y * along.y;
        const float sideways = std::fabs(moved.x * along.y - moved.y * along.x);
        const float allowedSideways = candidate_->maneuver == ObstacleManeuver::Jump ? kJumpSideways : kStrafeSideways;

        if (ahead >= kFreedDistance && sideways <= allowedSideways)
        {
            // A root or a stun looks just like a jam; in combat, whatever move
            // happened to be running when it wore off would be learned.
            if (!snapshot.inCombat())
            {
                obstacles_->learnManeuver(candidate_->at, candidate_->heading, candidate_->maneuver, &log_);
            }

            candidate_.reset();
        }
        else if (sideways > allowedSideways || now - candidate_->since > kFreedWindow)
        {
            candidate_.reset();
        }
    }

    if (madeAhead_ && now - madeAhead_->since > kAheadVerdict)
    {
        obstacles_->reportAhead(madeAhead_->move, true, &log_);
        madeAhead_.reset();
    }
}

void Navigator::makeMovesAhead(const PlayerState& player)
{
    if (!obstacles_ || strafeAhead_ != 0 || !clearOfZones(player.position, player.position, pathZones(), 8.0f))
    {
        return;
    }

    const TimePoint now = clock_.now();
    const auto move = obstacles_->due(player.position, travelHeading(player), now);

    if (!move)
    {
        return;
    }

    log_.info(logcat::Stuck, "{} ahead of the obstacle at {}", toString(move->maneuver), move->position);

    switch (move->maneuver)
    {
    case ObstacleManeuver::Jump:
        movement_.jump();
        break;
    case ObstacleManeuver::StrafeLeft:
        strafeAhead_ = 1;
        break;
    case ObstacleManeuver::StrafeRight:
        strafeAhead_ = -1;
        break;
    }

    strafeAheadUntil_ = now + kStrafeAhead;
    madeAhead_ = MadeAhead{*move, now};
}

// Steering releases the strafe keys every tick, so a sidestep is re-asserted
// after it for as long as it lasts.
void Navigator::holdStrafeAhead()
{
    if (strafeAhead_ == 0)
    {
        return;
    }

    if (clock_.now() < strafeAheadUntil_)
    {
        if (strafeAhead_ > 0)
        {
            movement_.setStrafeLeft(true);
        }
        else
        {
            movement_.setStrafeRight(true);
        }

        return;
    }

    movement_.setStrafeLeft(false);
    movement_.setStrafeRight(false);
    strafeAhead_ = 0;
}

int Navigator::depthStep(const PlayerState& player, const Vec3& destination) const
{
    // Within a yard and a half of the height wanted is near enough; diving
    // starts within this of the place (2D), not on the way across a lake.
    constexpr float kLevel = 1.5f;
    constexpr float kDiveFrom = 20.0f;
    // Feet this far under the surface: under, not afloat (a human's feet are
    // 1.5 yd under at the surface; the waves come and go).
    constexpr float kSubmerged = 2.2f;
    const auto surface = provider_->waterSurface(player.position);

    if (!surface || player.position.z > *surface)
    {
        return 0;
    }

    const auto there = provider_->waterSurface(destination);
    const bool underThere = there && destination.z < *there - kLevel;

    if (underThere && player.position.z > destination.z + kLevel && distance2D(player.position, destination) <= kDiveFrom)
    {
        return -1;
    }

    // Up to it, or out of the depths to the surface on the way somewhere dry.
    if (player.position.z < *surface - kSubmerged && (player.position.z < destination.z - kLevel || !underThere))
    {
        return 1;
    }

    return 0;
}

NavStatus Navigator::moveTo(const GameSnapshot& snapshot, const Vec3& wanted, float arriveRadius, bool stopOnArrival, bool exact, bool intoZone)
{
    Vec3 destination = wanted;

    const bool interrupted = movement_.syncReleasedKeys();
    if (interrupted || movement_.inputPaused())
    {
        // Typing deliberately releases the keys. Start a fresh progress
        // window when walking resumes, keeping the route we already have.
        stuck_.reset();
        headwayAt_.reset();
        jamSince_.reset();
        retreatTo_.reset();
        recovery_.cancel();
        strafeAhead_ = 0;
        spinAt_.reset();
        lastMoveAt_.reset();
        speedSampleAt_.reset();
        riseZ_.reset();
        riseStill_ = 0;
        riseRetryAt_.reset();
        if (movement_.inputPaused())
        {
            return NavStatus::Moving;
        }
    }

    if (const auto lift = rideLift(snapshot, destination, exact))
    {
        return *lift;
    }

    // Under water the height counts too: above a corpse on the bed of a lake
    // is not at it. The way over the bed is the flat one; up and down are
    // keys of their own, held beside it.
    int vertical = provider_->usesClientGeometry() ? depthStep(snapshot.player, destination) : 0;
    const float z = snapshot.player.position.z;

    // Rising no further for three ticks: at the surface, where the key only
    // jumps - or under something. Let go until the character is lower again,
    // and try again in a while: what was in the way may not be by then.
    if (vertical == 1)
    {
        if (riseZ_ && z <= *riseZ_ + 0.05f && z >= *riseZ_ - 0.5f)
        {
            if (++riseStill_ >= 3)
            {
                const TimePoint now = clock_.now();

                if (!riseRetryAt_)
                {
                    riseRetryAt_ = now + kRiseRetry;
                }

                if (now < *riseRetryAt_)
                {
                    vertical = 0;
                }
                else
                {
                    riseStill_ = 0;
                    riseRetryAt_.reset();
                }
            }
        }
        else
        {
            riseStill_ = 0;
            riseRetryAt_.reset();
        }

        riseZ_ = z;
    }
    else
    {
        riseZ_.reset();
        riseStill_ = 0;
        riseRetryAt_.reset();
    }

    // A cast is stood still for (moveFlat): the swim keys let go of too.
    if (snapshot.player.isCasting && !snapshot.inCombat() && snapshot.attackers().empty())
    {
        vertical = 0;
    }

    movement_.keepVertical(vertical != 0);
    const NavStatus status = moveFlat(snapshot, destination, arriveRadius, stopOnArrival, exact, intoZone);
    movement_.keepVertical(false);

    if (status == NavStatus::Unreachable)
    {
        movement_.setVertical(0);
        return status;
    }

    movement_.setVertical(vertical);
    return status == NavStatus::Arrived && vertical != 0 ? NavStatus::Moving : status;
}

NavStatus Navigator::moveFlat(const GameSnapshot& snapshot, const Vec3& destination, float arriveRadius, bool stopOnArrival, bool exact, bool intoZone)
{
    if (prepared_ && prepared_->map != snapshot.mapId)
    {
        prepared_.reset();
    }

    if (obstacles_ && obstacles_->stopToken().stop_requested())
    {
        stop();
        return NavStatus::Unreachable;
    }

    // A spell being cast out of a fight - Summon Imp's ten seconds, pressed
    // as a buff on the way to hand a quest in - is broken off by a step: the
    // way waits until it is done. In a fight the rotation sees to it.
    if (snapshot.player.isCasting && !snapshot.inCombat() && snapshot.attackers().empty())
    {
        halt();
        return NavStatus::Moving;
    }

    // Swimming, or a ghost walking on the water: the map there is the bed of
    // the lake, far under the bot. Taken as standing on it - else no point of
    // the way was ever on its level, and a new way "started on another floor":
    // a ghost turned round and round at the shore of Lake Everstill, then
    // stood there for good.
    PlayerState afloat;
    const PlayerState* standing = &snapshot.player;

    if (provider_->usesClientGeometry())
    {
        if (const auto ground = provider_->groundBelow(snapshot.player.position))
        {
            afloat = snapshot.player;
            afloat.position.z = *ground;
            standing = &afloat;
        }
    }

    const PlayerState& player = *standing;
    going_ = true;

    // The recovery moves and backing away are watched on their own.
    if (recovery_.active() || retreatTo_)
    {
        headwayAt_.reset();
    }

    exact_ = exact;
    intoZone_ = intoZone;

    if (!currentGoal_ || distance(*currentGoal_, destination) > 1.0f)
    {
        currentGoal_ = destination;
        retreatCount_ = 0;
    }

    const auto zones = pathZones(player.position, destination);
    const TimePoint now = clock_.now();

    // A while since the last step - the bot was busy elsewhere, or a long
    // way took seconds to plan (the map's tiles built anew): that time was
    // not spent stuck. Counted as such, it jumped and planned again at once.
    if (lastMoveAt_ && now - *lastMoveAt_ > kMoveGap)
    {
        headwayAt_.reset();
        stuck_.reset();
        spinAt_.reset();
    }

    lastMoveAt_ = now;

    if (speedSampleAt_)
    {
        const float seconds = static_cast<float>(toSeconds(now - *speedSampleAt_));

        if (seconds >= 0.04f && seconds <= 0.75f)
        {
            const float observed = distance2D(speedSamplePosition_, player.position) / seconds;

            if (observed >= 1.0f && observed <= 12.0f)
            {
                runSpeedEstimate_ = runSpeedEstimate_ > 1.0f ? runSpeedEstimate_ * 0.7f + observed * 0.3f : observed;
            }
        }
    }

    speedSamplePosition_ = player.position;
    speedSampleAt_ = now;
    // Moving exactly, a jam is left to the recovery moves: marking an obstacle
    // and backing away from it would take the bot off the line it must keep.
    const bool pathfinding = provider_->usesClientGeometry();

    if (jamSince_ && !retreatTo_ && !exact_)
    {
        const Vec3 delta = player.position - jamAt_;
        const Vec3 along = directionFromHeading(arrivalHeading_);

        // Sidesteps and repeated small movements must not restart the deadline.
        // After a new path, which may well lead another way, any way counts.
        if (delta.x * along.x + delta.y * along.y >= kFreedDistance || (jamReplanned_ && distance2D(player.position, jamAt_) >= kFreedDistance))
        {
            jamSince_.reset();
        }
        else if (pathfinding && !jamReplanned_ && now - *jamSince_ >= kReplanAfter)
        {
            jamReplanned_ = true;
            recovery_.cancel();
            strafeAhead_ = 0;
            candidate_.reset();
            stuck_.reset();
            clearPath();
            rememberJam(jamAt_, along);
            log_.warn(logcat::Stuck, "not free after 3 seconds at {}: planning a new path to {}, round what is in the way", jamAt_, destination);
        }
        else if (now - *jamSince_ >= Duration(5500))
        {
            if (++retreatCount_ >= 2)
            {
                log_.warn(logcat::Stuck, "stuck for 5.5s twice trying to reach {}: giving up", destination);
                lastFailure_ = RecoveryResult::RequestBlacklist;
                stop();
                return NavStatus::Unreachable;
            }

            recovery_.cancel();
            strafeAhead_ = 0;
            candidate_.reset();
            madeAhead_.reset();

            if (obstacles_)
            {
                obstacles_->learnAvoidZone(jamAt_, &log_, true);
            }

            const float retreatDistance = obstacles_ ? obstacles_->config().avoidRadius + 6.0f : 12.0f;
            retreatTo_ = jamAt_ - along * retreatDistance;

            if (pathfinding)
            {
                // Straight back only as far as there is ground to walk: the
                // way in can bend, in a tunnel or a mine.
                retreatTo_.reset();

                for (const float back : {retreatDistance, 8.0f, 5.0f, 3.0f})
                {
                    const Vec3 to = player.position - along * back;

                    if (provider_->canWalkStraight(player.position, to))
                    {
                        retreatTo_ = to;
                        break;
                    }
                }
            }

            retreatSince_ = now;
            retreatFaced_ = false;
            clearPath();

            if (retreatTo_ && pathfinding)
            {
                log_.warn(logcat::Stuck, "stuck for 5.5 seconds at {}: backing away, then a new path to {}", jamAt_, destination);
            }
            else if (retreatTo_)
            {
                log_.warn(logcat::Stuck, "stuck for 5.5 seconds: turn back, leave obstacle at {}, then route around it", jamAt_);
            }
            else
            {
                // Nowhere to back away to: a new path from here, and the jam
                // is judged afresh.
                jamSince_.reset();
                stuck_.reset();
                log_.warn(logcat::Stuck, "stuck for 5.5 seconds at {} with no room to back away: planning a new path to {}", jamAt_, destination);
            }
        }
    }

    if (retreatTo_)
    {
        // Allow an outward escape from a learned obstacle, never from a painted
        // exclusion and never deeper into another remembered obstacle.
        bool safe = !obstacles_ || clearOfZones(player.position, *retreatTo_, obstacles_->noGoZones());

        if (obstacles_)
        {
            for (const auto& zone : obstacles_->avoidZones())
            {
                const Vec3 outward = player.position - zone.center;
                const Vec3 direction = *retreatTo_ - player.position;

                if (distance2D(player.position, zone.center) <= zone.radius)
                {
                    if (distance2D(zone.center, jamAt_) < 0.1f)
                    {
                        continue; // Leave the obstacle we just marked.
                    }

                    if (outward.x * direction.x + outward.y * direction.y < -0.01f)
                    {
                        safe = false;
                    }
                }
                else if (!clearOfZones(player.position, *retreatTo_, std::span(&zone, 1)))
                {
                    safe = false;
                }
            }
        }

        if (!safe || now - retreatSince_ >= Duration(6000))
        {
            lastFailure_ = RecoveryResult::RequestBlacklist;
            stop();
            return NavStatus::Unreachable;
        }

        // Face fully before walking, even when ordinary steering allows a curve.
        // Only once: facing lets go of W, and walking presses it again - each
        // tick, that tapped W twenty times a second.
        if (distance2D(player.position, *retreatTo_) > 1.0f)
        {
            if (!retreatFaced_ && !movement_.face(player, *retreatTo_))
            {
                return NavStatus::Recovering;
            }

            retreatFaced_ = true;
            movement_.moveTo(player, *retreatTo_, 1.0f);
            return NavStatus::Recovering;
        }

        movement_.stop();
        retreatTo_.reset();
        jamSince_.reset();
        stuck_.reset();
        clearPath();
    }

    if (jamSince_ && obstacles_ && !exact_ && !clearOfZones(player.position, player.position, obstacles_->noGoZones(), 8.0f))
    {
        recovery_.cancel();
        return NavStatus::Recovering;
    }

    if (recovery_.active())
    {
        const RecoveryStage stage = recovery_.stage();

        switch (recovery_.tick(player))
        {
        case RecoveryResult::InProgress:
            return NavStatus::Recovering;
        case RecoveryResult::Done:
            stuck_.reset();

            if (const auto maneuver = maneuverFor(stage); maneuver && obstacles_)
            {
                candidate_ = Candidate{*maneuver, recoveryStart_, recoveryHeading_, clock_.now()};
            }

            break;
        case RecoveryResult::RequestRecalculate:
            stuck_.reset();
            clearPath();
            break;
        // Given up on this destination: the next starts the moves afresh. Left
        // at its last stage, every new destination was given up the moment the
        // bot jammed again, one after another.
        case RecoveryResult::RequestBlacklist:
            lastFailure_ = RecoveryResult::RequestBlacklist;
            stop();
            recovery_.resetEscalation();
            return NavStatus::Unreachable;
        case RecoveryResult::RequestFallback:
            lastFailure_ = RecoveryResult::RequestFallback;
            stop();
            recovery_.resetEscalation();
            return NavStatus::Unreachable;
        }
    }

    // A target that moves a little (a mob walking off) keeps the path, only its
    // end following the target - unless that last leg now crosses an obstacle.
    // Planned afresh every tick, the way round an obstacle could change sides
    // from one tick to the next, and the bot pushed on into it instead.
    const auto lastLegBlocked = [&]
    {
        if (zones.empty() || path_.points.empty())
        {
            return false;
        }

        const Vec3 from = path_.points.size() >= 2 ? path_.points[path_.points.size() - 2] : player.position;
        return !clearOfZones(from, destination, zones);
    };

    // On the client's map, a target that moves a little (a mob shifting where
    // it stands) keeps the way too, its end following the target, as long as
    // the last leg can still be walked straight. Planned afresh every tick,
    // the way round a house's corner started anew at each step, and the bot
    // turned to and fro until it saw the mob.
    // From where the way was planned to: further, and it is planned again.
    constexpr float kFollowDrift = 3.0f;
    const auto endFollows = [&]
    {
        if (!destination_ || path_.points.empty() || distance(plannedTo_, destination) > kFollowDrift || exact_)
        {
            return false;
        }

        const Vec3 from = path_.points.size() >= 2 ? path_.points[path_.points.size() - 2] : player.position;
        return provider_->canWalkStraight(from, destination) && clearOfZones(from, destination, zones);
    };
    const bool clientDrift = provider_->usesClientGeometry() && destination_ && distance(*destination_, destination) > 0.05f;
    const bool following = clientDrift && endFollows();
    // Its last leg not straight - a bump in the ground between - the way is
    // kept as it is while the target is near where it was planned to: an
    // Adult Plainstrider walking off made a new way of every step it took,
    // and each let go of the forward key, the bot stepping a tick at a time.
    const bool keptAsIs = clientDrift && !following && !exact_ && !path_.points.empty() && distance(plannedTo_, destination) <= kFollowDrift;
    const bool replan = !destination_ || (clientDrift && !following && !keptAsIs) || (!provider_->usesClientGeometry() && distance(*destination_, destination) > config_.repathDistance) || (!following && !keptAsIs && distance(*destination_, destination) > 0.01f && lastLegBlocked()) || (obstacles_ && pathRevision_ != obstacles_->revision()) || pathExact_ != exact_ || pathIntoZone_ != intoZone_;
    const AvoidZone* crossing = replan ? nullptr : mobZoneOnPath(player.position);

    if (crossing && crossedBefore(*crossing, destination))
    {
        pastMobsUntil_ = clock_.now() + kPastMobsFor;
        log_.info(logcat::Move, "the way round the mobs to {} keeps turning back past {} - walking past them for {} seconds", destination, crossing->name.empty() ? std::string("a mob") : crossing->name, std::chrono::duration_cast<std::chrono::seconds>(kPastMobsFor).count());
    }
    else if (crossing)
    {
        log_.info(logcat::Move, "the way crosses the reach of {} ({:.0f} yd round it, {:.0f} yd off) - planning round it", crossing->name.empty() ? std::string("a mob") : crossing->name, crossing->radius, distance2D(player.position, crossing->center));
    }

    if (replan || crossing)
    {
        if (!computePath(player, destination))
        {
            lastFailure_ = RecoveryResult::RequestBlacklist;
            movement_.stop();
            return NavStatus::Unreachable;
        }
    }
    else if (!provider_->usesClientGeometry() || following)
    {
        // Destination drifted a little (moving target): keep the path but aim at the new end point.
        path_.points.back() = destination;
        destination_ = destination;
    }
    else if (keptAsIs)
    {
        destination_ = destination;
    }

    // One whole turn and still at the same place: stop, and plan the way again
    // from here. The bot went round three times on open ground before it got on.
    if (recovering() || retreatTo_ || !spinAt_ || distance2D(*spinAt_, player.position) > kSpinReach || std::abs(spinAt_->z - player.position.z) > kSameLevel)
    {
        spinAt_ = player.position;
        spinTurned_ = 0.0f;
    }
    else
    {
        spinTurned_ += normalizeAngle(player.heading - spinHeading_);
    }

    spinHeading_ = player.heading;

    if (std::abs(spinTurned_) >= kTwoPi)
    {
        log_.info(logcat::Move, "turned right round without getting anywhere at {} - stopping to plan the way again", player.position);
        spinAt_.reset();
        movement_.stop();

        if (!computePath(player, destination))
        {
            lastFailure_ = RecoveryResult::RequestBlacklist;
            stop();
            return NavStatus::Unreachable;
        }

        return NavStatus::Moving;
    }

    // Skip intermediate path points we have already reached.
    // A point counts as reached only on its own level: up a spiral stair the
    // point a turn higher stands right over the bot, and taking it as reached
    // skipped a whole turn - the bot turned this way and that at the foot of
    // the tower of Northshire Abbey.
    const auto onLevel = [&](const Vec3& point) { return std::abs(point.z - player.position.z) <= kSameLevel; };

    while (pathIndex_ + 1 < path_.points.size() && onLevel(path_.points[pathIndex_]) && distance2D(player.position, path_.points[pathIndex_]) <= (provider_->usesClientGeometry() ? 0.6f : config_.cornerRadius))
    {
        ++pathIndex_;
    }

    // Run circles round a point on the way (it could not be reached - its
    // height off, or something in the way not on the map): on to the next.
    // Stopping to face it and setting off again, the bot went round and round
    // it for good near Sentinel Hill.
    if (movement_.orbits() != orbitsSeen_)
    {
        orbitsSeen_ = movement_.orbits();

        if (pathIndex_ + 1 < path_.points.size() && distance2D(movement_.lastOrbitPoint(), path_.points[pathIndex_]) <= 3.0f)
        {
            log_.info(logcat::Move, "ran circles round {} on the way - going on to the next point", path_.points[pathIndex_]);
            ++pathIndex_;
        }
    }

    // Standing right by a point off its level for kOffLevelHover, it is the
    // ground there, not a floor above: a point whose height is off went
    // round and round otherwise. Up a stair the bot only passes under one.
    if (pathIndex_ + 1 < path_.points.size() && distance2D(player.position, path_.points[pathIndex_]) <= kHoverReach)
    {
        if (hoverIndex_ != pathIndex_ || !hoverSince_)
        {
            hoverIndex_ = pathIndex_;
            hoverSince_ = clock_.now();
        }
        else if (clock_.now() - *hoverSince_ >= kOffLevelHover)
        {
            ++pathIndex_;
            hoverSince_.reset();
        }
    }
    else
    {
        hoverSince_.reset();
    }

    // Advance past curve samples and corners already behind the player,
    // without forcing a return to their exact position after an overshoot or
    // a corner cut wide. Facing back at one (now turned on the spot, as a
    // sharp turn is) stopped the bot to turn round at every wide corner.
    if (provider_->usesClientGeometry())
    {
        constexpr float kPassedReach = 8.0f;

        while (pathIndex_ + 1 < path_.points.size() && distance2D(player.position, path_.points[pathIndex_]) < kPassedReach && onLevel(path_.points[pathIndex_]))
        {
            const Vec3 segment = path_.points[pathIndex_ + 1] - path_.points[pathIndex_];
            const Vec3 passed = player.position - path_.points[pathIndex_];
            const float along = passed.x * segment.x + passed.y * segment.y;

            if (along <= 0)
            {
                break;
            }

            // Back onto the way where it runs beside the bot - not straight to
            // its next point: that one can be far off, and the line to it
            // graze a rock the bend goes round. The corner then stayed the
            // point steered by, 3.5 yd past it lay behind, and the bot turned
            // round to it and on again, in circles on open ground.
            const float length2 = segment.x * segment.x + segment.y * segment.y;
            const Vec3 beside = path_.points[pathIndex_] + segment * (length2 > 0.0001f ? std::min(along / length2, 1.0f) : 1.0f);

            if (distance2D(player.position, beside) > 0.5f && !provider_->canWalkStraight(player.position, beside))
            {
                break;
            }

            ++pathIndex_;
        }
    }

    if (!clearOfZones(player.position, path_.points[pathIndex_], zones))
    {
        if (!computePath(player, destination))
        {
            lastFailure_ = RecoveryResult::RequestBlacklist;
            stop();
            return NavStatus::Unreachable;
        }
    }

    watchObstacleMoves(snapshot);
    makeMovesAhead(player);

    const bool last = pathIndex_ + 1 >= path_.points.size();
    const float radius = last ? arriveRadius : provider_->usesClientGeometry() ? 0.6f : config_.cornerRadius;
    Vec3 steeringTarget = path_.points[pathIndex_];

    if (provider_->usesClientGeometry())
    {
        // Aim ahead along the curve. This starts steering before the corner
        // and keeps W held while the turn key follows the bend.
        if (!last)
        {
            const auto pointAlongPath = [&](float distance)
            {
                Vec3 previous = player.position;

                for (std::size_t j = pathIndex_; j < path_.points.size(); ++j)
                {
                    const Vec3 point = path_.points[j];
                    const float length = distance2D(previous, point);

                    if (length >= distance && length > 0.001f)
                    {
                        return previous + (point - previous) * (distance / length);
                    }

                    distance -= length;
                    previous = point;
                }

                return path_.points.back();
            };
            constexpr float baseLookahead = 3.5f;
            constexpr float maxLookahead = 8.0f;
            float lookahead = baseLookahead;
            const Vec3 near = pointAlongPath(2.0f), far = pointAlongPath(maxLookahead);
            Vec3 nearDirection = near - player.position, farDirection = far - player.position;
            nearDirection.z = farDirection.z = 0;

            if (nearDirection.length2D() > 0.1f && farDirection.length2D() > 0.1f)
            {
                const float cosine = std::clamp((nearDirection.x * farDirection.x + nearDirection.y * farDirection.y) / (nearDirection.length2D() * farDirection.length2D()), -1.0f, 1.0f);
                const float upcomingTurn = std::acos(cosine);
                const float turnRadius = estimatedRunSpeed() / movement_.estimatedTurnRate();
                lookahead = std::clamp(baseLookahead + turnRadius * upcomingTurn * 1.25f, baseLookahead, maxLookahead);

                // Where the way runs straight on, aim well ahead. A point 3.5 yd
                // off turns half a yard beside the line into a heading to be
                // corrected, and each correction carried the bot across the
                // line for the next: left and right about once a second. Only
                // on the straight: round a corner it would cut it.
                constexpr float straightLookahead = 10.0f;
                constexpr float kStraight = 0.12f;

                if (upcomingTurn < kStraight)
                {
                    Vec3 beyond = pointAlongPath(straightLookahead + 2.0f) - player.position;
                    beyond.z = 0;

                    if (beyond.length2D() > 0.1f && std::acos(std::clamp((nearDirection.x * beyond.x + nearDirection.y * beyond.y) / (nearDirection.length2D() * beyond.length2D()), -1.0f, 1.0f)) < kStraight)
                    {
                        lookahead = straightLookahead;
                    }
                }
            }

            float remaining = lookahead;
            Vec3 previous = player.position;

            // Past a corner cut wide, the way is taken up where it runs beside
            // the bot rather than by the straight line to its next point.
            if (pathIndex_ > 0)
            {
                const Vec3 from = path_.points[pathIndex_ - 1];
                const Vec3 leg = path_.points[pathIndex_] - from;
                const float length2 = leg.x * leg.x + leg.y * leg.y;

                if (length2 > 0.0001f)
                {
                    const float t = std::clamp(((player.position.x - from.x) * leg.x + (player.position.y - from.y) * leg.y) / length2, 0.0f, 1.0f);
                    const Vec3 beside = from + leg * t;

                    if (distance2D(player.position, beside) > 0.5f)
                    {
                        previous = beside;
                    }
                }
            }

            for (std::size_t j = pathIndex_; j < path_.points.size(); ++j)
            {
                const Vec3 point = path_.points[j];
                const float length = distance2D(previous, point);
                const Vec3 candidate = length > remaining ? previous + (point - previous) * (remaining / length) : point;

                if (!provider_->canWalkStraight(player.position, candidate))
                {
                    break;
                }

                steeringTarget = candidate;

                if (length >= remaining)
                {
                    break;
                }

                remaining -= length;
                previous = point;
            }
        }
    }

    bool allowMovingTurn = true;

    if (provider_->usesClientGeometry())
    {
        // Keep forward held whenever the movement arc for this steering step
        // stays on walkable ground and clears the configured obstacle zones.
        const float turnRadius = estimatedRunSpeed() / movement_.estimatedTurnRate();
        // Check the same turn arc the controller will use: turn until its
        // tangent points at the path lookahead, rather than simply facing its
        // current position.
        const float directDelta = headingDelta(player.heading, player.position, steeringTarget);
        const float delta = turnArcDeltaToTarget(player, steeringTarget, turnRadius).value_or(directDelta);
        const float direction = delta > 0 ? 1.0f : -1.0f;
        const float angle = std::min(std::abs(delta), movement_.estimatedTurnRate() * 0.65f);
        Vec3 previous = player.position;

        if (angle < 0.02f)
        {
            const Vec3 next = player.position + directionFromHeading(player.heading) * 2.0f;
            allowMovingTurn = provider_->canWalkStraight(player.position, next) && clearOfZones(player.position, next, zones);
        }

        for (int sample = 1; angle >= 0.02f && sample <= 5; ++sample)
        {
            if (!allowMovingTurn)
            {
                break;
            }

            const float heading = player.heading + direction * angle * sample / 5;
            const Vec3 next = player.position + Vec3{direction * turnRadius * (std::sin(heading) - std::sin(player.heading)), direction * turnRadius * (std::cos(player.heading) - std::cos(heading)), 0};

            if (!walkableArcSegment(*provider_, previous, next) || !clearOfZones(previous, next, zones))
            {
                allowMovingTurn = false;
                break;
            }

            previous = next;
        }
    }

    const MoveStatus status = movement_.moveTo(player, steeringTarget, radius, last ? stopOnArrival : false, allowMovingTurn, provider_->usesClientGeometry(), estimatedRunSpeed());

    if (!clearOfZones(player.position, player.position, zones, 8.0f))
    {
        strafeAhead_ = 0;
    }

    holdStrafeAhead();

    // The end of the way is judged as before: a caller tells a floor it
    // cannot reach (Quest hand-in leaves then) from standing there for good.
    if (status == MoveStatus::Arrived && (last || onLevel(path_.points[pathIndex_])))
    {
        if (last)
        {
            if (path_.continues)
            {
                clearPath();
                stuck_.reset();
                return NavStatus::Moving;
            }

            stuck_.reset();
            jamSince_.reset();
            headwayAt_.reset();
            recovery_.resetEscalation();
            retreatCount_ = 0;
            currentGoal_.reset();

            if (stopOnArrival)
            {
                clearPath();
            }

            return NavStatus::Arrived;
        }

        ++pathIndex_;
        return NavStatus::Moving;
    }

    // Going somewhere without getting anywhere, though not with Forward held
    // throughout - let go and pressed again every half second, which the stuck
    // window starts over on - is a jam as well: 20 seconds stood still on the
    // way to a vendor so.
    bool noHeadway = false;

    if (!headwayAt_ || distance2D(player.position, headwayFrom_) >= kHeadway)
    {
        headwayFrom_ = player.position;
        headwayAt_ = now;
    }
    else if (now - *headwayAt_ >= kNoHeadwayFor)
    {
        noHeadway = true;
        headwayFrom_ = player.position;
        headwayAt_ = now;
        log_.warn(logcat::Stuck, "no headway for {} seconds at {}", toSeconds(kNoHeadwayFor), player.position);
    }

    if (stuck_.update(clock_.now(), player.position, movement_.isMovingForward()) || noHeadway)
    {
        ++stuckCount_;
        // Jammed again: the last recovery move did not free the bot, and a
        // move made ahead of this spot did not get it past.
        candidate_.reset();

        if (obstacles_ && madeAhead_ && distance2D(player.position, madeAhead_->move.position) <= kAheadRadius)
        {
            obstacles_->reportAhead(madeAhead_->move, false, &log_);
            madeAhead_.reset();
        }

        if (!jamSince_)
        {
            jamSince_ = now - config_.stuck.window;
            jamAt_ = player.position;
            arrivalHeading_ = travelHeading(player);
            jamReplanned_ = false;
        }

        if (config_.obstacleRegistration == ObstacleRegistration::Aggressive && obstacles_)
        {
            obstacles_->learnAvoidZone(jamAt_, &log_, true);
        }

        recoveryStart_ = player.position;
        recoveryHeading_ = travelHeading(player);

        if (exact_ || !obstacles_ || clearOfZones(player.position, player.position, obstacles_->noGoZones(), 8.0f))
        {
            recovery_.begin(player);
        }
        else
        {
            movement_.stop();
        }

        if (events_)
        {
            events_->publish(StuckDetectedEvent{player.position, recovery_.stage()});
        }

        return NavStatus::Recovering;
    }

    return NavStatus::Moving;
}

} // namespace og
