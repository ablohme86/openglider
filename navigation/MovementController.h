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
 *  File: MovementController.h
 *
 *  Desc:
 *  Declarations and interface for MovementController (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/actions/IActionService.h"
#include "og/common/Clock.h"
#include "og/common/Log.h"
#include "og/common/MathUtil.h"
#include "og/state/UnitState.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace og
{

struct MovementConfig
{
    float turnDeadZone = 0.20f;      // rad: start correcting heading above this error while running
    float turnStopTolerance = 0.05f; // rad: stop turning below this error
    // Turns are run through, in an arc (runSpeed / turnRate yards across, about
    // 2 yd): stopping to turn at every corner looked like a bot. The bot turns
    // on the spot to turn round (above turnInPlaceAngle; from standing, above
    // startInPlaceAngle), or when the destination is too near to reach in the
    // arc (with arcMargin to spare). Turning round in an arc in a narrow
    // corridor (out of Northshire Abbey) ran it into the walls.
    float turnInPlaceAngle = 2.1f;   // rad: about 120 degrees
    float startInPlaceAngle = 1.75f; // rad: about 100 degrees, setting off
    // Setting off after standing still a while (stoodStill): turned on the
    // spot for anything more than this first. Off at a run from a quest
    // giver, a vendor or a trainer in a house, the arc took the bot into the
    // tables and barrels about it.
    float setOffInPlaceAngle = 0.6f; // rad: about 35 degrees
    Duration stoodStill{1000};
    float runSpeed = 7.0f;           // yards/s running
    float arcMargin = 1.3f;
    float faceTolerance = 0.15f; // rad: default tolerance for face()
    float turnRate = kPi;        // rad/s keyboard turn speed (WoW: 180 deg/s)
    // A movement key just let go of is not pressed again sooner than this
    // (drawn from the range each time), nor is a turn reversed: a bot tapping
    // W or A/D several times a second is easy to spot. Letting go is never held up.
    Duration repressDelayMin{180};
    Duration repressDelayMax{320};
};

// Heading change for a constant-radius turn whose endpoint leaves the player
// on a tangent line to `target`; null when no forward tangent fits a moving
// turn. `turnRadius` is horizontal speed / angular turn rate.
std::optional<float> turnArcDeltaToTarget(const PlayerState& player, const Vec3& target, float turnRadius);

enum class MoveStatus
{
    Moving,
    Turning,
    Arrived
};

// Turns "go to this position" / "face this" into held movement actions
// (Move.Forward, Move.TurnLeft, ...). Knows nothing about paths or routes.
class MovementController
{
public:
    MovementController(IActionService& actions, const IClock& clock, Logger& log, MovementConfig config = {});

    // Steers towards `dest`. With stopOnArrival=false the forward key stays held
    // when arriving so the caller can chain to the next waypoint smoothly.
    MoveStatus moveTo(const PlayerState& player, const Vec3& dest, float arriveRadius, bool stopOnArrival = true, bool allowMovingTurn = true, bool followingCurve = false, float horizontalSpeed = 0.0f);

    // Turns in place. Returns true once facing within tolerance (turn released).
    bool face(const PlayerState& player, const Vec3& target, float tolerance = -1.0f);
    bool faceHeading(const PlayerState& player, float heading, float tolerance = -1.0f);
    // Turns towards target without canceling active forward/backward or strafing movement.
    bool faceWhileMoving(const PlayerState& player, const Vec3& target, float tolerance = -1.0f);

    void stop(); // release every movement key
    void stopTurning();

    // Chat and other actions can release keys outside this controller.
    // Returns true when that interrupted a movement we believed was held.
    bool syncReleasedKeys();
    bool inputPaused() const
    {
        return actions_.inputLocked();
    }

    void setForward(bool on);
    void setBackward(bool on);
    void setStrafeLeft(bool on);
    void setStrafeRight(bool on);
    void jump();
    // +1 = left, -1 = right, 0 = stop turning.
    void setTurn(int direction);
    // In the water: +1 = swim up, -1 = swim down, 0 = neither.
    void setVertical(int direction);
    // Lets go of up and down - unless keepVertical holds them.
    void releaseVertical();

    int verticalDirection() const
    {
        return up_ ? 1 : down_ ? -1 : 0;
    }

    // While set, stop() leaves the up and down keys as they are: a way
    // arrived at over the bed of a lake, with the dive to it still going on.
    void keepVertical(bool keep)
    {
        keepVertical_ = keep;
    }

    bool isMovingForward() const
    {
        return forward_;
    }

    // How many times the bot has run circles round a point, and the last one.
    int orbits() const
    {
        return orbits_;
    }

    const Vec3& lastOrbitPoint() const
    {
        return orbitDest_;
    }

    bool isMoving() const
    {
        return forward_ || backward_ || strafeLeft_ || strafeRight_ || turn_ != 0;
    }

    int turnDirection() const
    {
        return turn_;
    }

    // Measured from heading changes while a turn key is held; falls back to
    // the configured rate until the first usable observation.
    float estimatedTurnRate() const
    {
        return estimatedTurnRate_;
    }

    const MovementConfig& config() const
    {
        return config_;
    }

private:
    // One movement key (a pair for turning): when it was let go of, and how
    // long it stays up before it may be pressed again.
    struct KeyGate
    {
        TimePoint releasedAt{};
        Duration wait{0};
        bool open(TimePoint now) const
        {
            return now - releasedAt >= wait;
        }
    };
    // Applies turn hysteresis for a heading error `delta`.
    void steer(const PlayerState& player, float delta, float startThreshold);
    void updateTurnRateEstimate(const PlayerState& player, TimePoint now);
    void set(std::string_view action, bool& state, KeyGate& gate, bool on);
    void released(KeyGate& gate);

    std::uint64_t random_ = 0x9E3779B97F4A7C15ull;

    IActionService& actions_;
    const IClock& clock_;
    Logger& log_;
    MovementConfig config_;
    float estimatedTurnRate_ = kPi;
    bool hasTurnRateObservation_ = false;

    bool forward_ = false;
    bool backward_ = false;
    bool strafeLeft_ = false;
    bool strafeRight_ = false;
    bool up_ = false;
    bool down_ = false;
    bool keepVertical_ = false;
    int turn_ = 0;
    KeyGate forwardGate_, backwardGate_, strafeLeftGate_, strafeRightGate_, turnGate_, upGate_, downGate_;
    TimePoint lastSteer_{};
    float lastDt_ = 0.05f;
    TimePoint turnSampleAt_{};
    float turnSampleHeading_ = 0.0f;
    int turnSampleDirection_ = 0;

    // How far the character goes on turning after the turn key is let go, as
    // the bot sees it: the key's way to the game, the game's frame, and the
    // age of the heading read. Measured after every turn the steering ends,
    // and the key is let go that much short of the heading wanted. Reckoned
    // from the tick alone (a fraction of a degree) the turn ended well past
    // it, and the way there was a string of small turns back and forth.
    struct Coast
    {
        TimePoint releasedAt;
        float heading = 0.0f; // as seen when the key was let go
        int direction = 0;
        float turned = 0.0f; // since, the most seen
        TimePoint turnedAt;
    };
    std::optional<Coast> coasting_;
    TimePoint turnPressedAt_{}; // when the steering last pressed a turn key
    float coast_ = 0.0f;
    bool hasCoast_ = false;
    void watchCoast(const PlayerState& player, TimePoint now);

    // Running round a point inside the turning circle never reaches it: the
    // heading error stays about the same while the bot goes round and round
    // (outside the Northshire Abbey stairs, for 20 seconds). How far the bot
    // has turned, running, one way, at one point; past kOrbitAngle it stops
    // and faces the point before it runs on.
    void watchOrbit(const PlayerState& player, const Vec3& dest);
    float orbitTurned_ = 0.0f;
    float orbitHeading_ = 0.0f;
    int orbitDirection_ = 0;
    Vec3 orbitDest_{};
    bool orbitBreak_ = false;
    int orbits_ = 0;
};

} // namespace og
