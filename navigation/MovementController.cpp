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
 *  File: MovementController.cpp
 *
 *  Desc:
 *  Implementation of MovementController (navigation, pathfinding,
 *  movement control, and stuck recovery).
 *
 *
 */
#include "og/navigation/MovementController.h"

#include "og/actions/ActionBinding.h"

#include <algorithm>
#include <cmath>

namespace og
{

std::optional<float> turnArcDeltaToTarget(const PlayerState& player, const Vec3& target, float turnRadius)
{
    constexpr float kMaxTurnAngle = 1.75f;
    constexpr float kSearchStep = 0.025f;
    constexpr float kMinLateralOffset = 0.01f;

    if (turnRadius <= 0.0f)
    {
        return std::nullopt;
    }

    const float dx = target.x - player.position.x;
    const float dy = target.y - player.position.y;
    const float forward = dx * std::cos(player.heading) + dy * std::sin(player.heading);
    const float left = -dx * std::sin(player.heading) + dy * std::cos(player.heading);
    const float lateral = std::fabs(left);

    if (lateral < kMinLateralOffset)
    {
        return 0.0f;
    }

    const float side = left > 0.0f ? 1.0f : -1.0f;
    const auto tangentError = [&](float angle)
    {
        return forward * std::sin(angle) - lateral * std::cos(angle) + turnRadius * (std::cos(angle) - 1.0f);
    };
    float previousAngle = 0.0f;
    float previousError = tangentError(previousAngle);

    for (float angle = kSearchStep; angle <= kMaxTurnAngle + 0.5f * kSearchStep; angle += kSearchStep)
    {
        const float currentAngle = std::min(angle, kMaxTurnAngle);
        const float currentError = tangentError(currentAngle);

        if (previousError < 0.0f && currentError >= 0.0f)
        {
            float low = previousAngle;
            float high = currentAngle;

            for (int iteration = 0; iteration < 18; ++iteration)
            {
                const float middle = (low + high) * 0.5f;

                if (tangentError(middle) < 0.0f)
                {
                    low = middle;
                }
                else
                {
                    high = middle;
                }
            }

            const float turnAngle = (low + high) * 0.5f;
            const float endX = turnRadius * std::sin(turnAngle);
            const float endY = side * turnRadius * (1.0f - std::cos(turnAngle));
            const float remainingAlongTangent = (forward - endX) * std::cos(turnAngle) + (left - endY) * side * std::sin(turnAngle);

            if (remainingAlongTangent > 0.0f)
            {
                return side * turnAngle;
            }
        }

        previousAngle = currentAngle;
        previousError = currentError;

        if (currentAngle >= kMaxTurnAngle)
        {
            break;
        }
    }

    return std::nullopt;
}

MovementController::MovementController(IActionService& actions, const IClock& clock, Logger& log, MovementConfig config) : actions_(actions), clock_(clock), log_(log), config_(config), estimatedTurnRate_(config.turnRate)
{
}

void MovementController::released(KeyGate& gate)
{
    // xorshift64*: an even draw from the range, different every time.
    random_ ^= random_ >> 12;
    random_ ^= random_ << 25;
    random_ ^= random_ >> 27;
    const auto low = config_.repressDelayMin.count();
    const auto span = std::max<std::int64_t>(config_.repressDelayMax.count() - low, 0);
    const std::uint64_t draw = random_ * 0x2545F4914F6CDD1Dull;
    gate.releasedAt = clock_.now();
    gate.wait = Duration(low + (span > 0 ? static_cast<std::int64_t>(draw % static_cast<std::uint64_t>(span + 1)) : 0));
}

bool MovementController::syncReleasedKeys()
{
    bool interrupted = false;
    const auto sync = [&](std::string_view action, bool& state, KeyGate& gate)
    {
        if (state && !actions_.isHeld(action))
        {
            state = false;
            released(gate);
            interrupted = true;
        }
    };
    sync(actions::MoveForward, forward_, forwardGate_);
    sync(actions::MoveBackward, backward_, backwardGate_);
    sync(actions::StrafeLeft, strafeLeft_, strafeLeftGate_);
    sync(actions::StrafeRight, strafeRight_, strafeRightGate_);
    sync(actions::SwimUp, up_, upGate_);
    sync(actions::SwimDown, down_, downGate_);
    if (turn_ != 0 && !actions_.isHeld(turn_ > 0 ? actions::TurnLeft : actions::TurnRight))
    {
        turn_ = 0;
        released(turnGate_);
        interrupted = true;
    }
    if (interrupted)
    {
        orbitBreak_ = false;
        orbitTurned_ = 0;
        orbitDirection_ = 0;
    }
    return interrupted;
}

void MovementController::set(std::string_view action, bool& state, KeyGate& gate, bool on)
{
    syncReleasedKeys();
    if (state == on)
    {
        return;
    }

    if (on && (actions_.isCasting() || !gate.open(clock_.now())))
    {
        return;
    }

    if (on)
    {
        actions_.hold(action);

        // Held back for a moment after a chat line: not moving yet, so the
        // next tick asks again - rather than walking in place, as far as the
        // stuck detector could tell.
        if (!actions_.isHeld(action))
        {
            return;
        }

        state = true;
    }
    else
    {
        state = false;
        actions_.release(action);
        released(gate);
    }
}

void MovementController::setForward(bool on)
{
    set(actions::MoveForward, forward_, forwardGate_, on);
}

void MovementController::setBackward(bool on)
{
    set(actions::MoveBackward, backward_, backwardGate_, on);
}

void MovementController::setVertical(int direction)
{
    set(actions::SwimUp, up_, upGate_, direction > 0);
    set(actions::SwimDown, down_, downGate_, direction < 0);
}

void MovementController::setStrafeLeft(bool on)
{
    set(actions::StrafeLeft, strafeLeft_, strafeLeftGate_, on);
}

void MovementController::setStrafeRight(bool on)
{
    set(actions::StrafeRight, strafeRight_, strafeRightGate_, on);
}

void MovementController::setTurn(int direction)
{
    syncReleasedKeys();
    direction = std::clamp(direction, -1, 1);

    if (turn_ == direction)
    {
        return;
    }

    // A reversal lets go first; the other way waits for the gate, like any
    // new turn. Stopping is never held up.
    if (turn_ != 0 && direction != 0)
    {
        direction = 0;
    }

    if (direction != 0 && !turnGate_.open(clock_.now()))
    {
        return;
    }

    if (turn_ > 0)
    {
        actions_.release(actions::TurnLeft);
    }

    if (turn_ < 0)
    {
        actions_.release(actions::TurnRight);
    }

    if (turn_ != 0)
    {
        released(turnGate_);
    }

    turn_ = 0;

    if (direction == 0)
    {
        return;
    }

    const auto key = direction > 0 ? actions::TurnLeft : actions::TurnRight;
    actions_.hold(key);

    // Held back for a moment after a chat line: not turning yet, so the next
    // tick asks again - rather than wait, facing away, for a turn that never
    // comes (and never walk, since walking waits for the turn).
    if (actions_.isHeld(key))
    {
        turn_ = direction;
    }
}

void MovementController::jump()
{
    actions_.execute(actions::Jump);
}

void MovementController::stopTurning()
{
    setTurn(0);
}

namespace
{
// A full circle and a bit, run one way at the same point: the arc misses it.
// A bend in the path is never this much, and a target that keeps moving (a
// point ahead on a curve) starts the count again.
constexpr float kOrbitAngle = 1.25f * kTwoPi;
constexpr float kOrbitDestMoved = 1.0f;
} // namespace

void MovementController::watchOrbit(const PlayerState& player, const Vec3& dest)
{
    if (!forward_ || turn_ == 0 || turn_ != orbitDirection_ || distance2D(dest, orbitDest_) > kOrbitDestMoved)
    {
        orbitTurned_ = 0.0f;
        orbitDirection_ = forward_ ? turn_ : 0;
        orbitDest_ = dest;
    }
    else
    {
        orbitTurned_ += std::fabs(normalizeAngle(player.heading - orbitHeading_));
    }

    orbitHeading_ = player.heading;

    if (orbitTurned_ < kOrbitAngle)
    {
        return;
    }

    log_.info(logcat::Move, "running in circles round {} ({:.1f} yd off) - stopping to face it", dest, distance2D(player.position, dest));
    orbitBreak_ = true;
    ++orbits_;
    orbitTurned_ = 0.0f;
    orbitDirection_ = 0;
}

void MovementController::stop()
{
    orbitBreak_ = false;
    orbitTurned_ = 0.0f;
    orbitDirection_ = 0;
    setForward(false);
    setBackward(false);
    setStrafeLeft(false);
    setStrafeRight(false);
    setTurn(0);
    releaseVertical();
}

void MovementController::updateTurnRateEstimate(const PlayerState& player, TimePoint now)
{
    constexpr float kMinSampleInterval = 0.04f;
    constexpr float kMaxSampleInterval = 0.5f;
    constexpr float kMinUsefulTurn = 0.02f;
    constexpr float kMinPlausibleRate = 1.5f;
    constexpr float kMaxPlausibleRate = 7.0f;
    constexpr float kNewSampleWeight = 0.25f;

    const float dt = static_cast<float>(toSeconds(now - turnSampleAt_));

    if (turn_ != 0 && turn_ == turnSampleDirection_ && dt >= kMinSampleInterval && dt <= kMaxSampleInterval)
    {
        // WoW's left turn increases heading. Keep only motion in the held
        // direction so a knockback, snap, or stale heading cannot skew the
        // estimate.
        const float turned = normalizeAngle(player.heading - turnSampleHeading_) * static_cast<float>(turn_);

        if (turned >= kMinUsefulTurn)
        {
            const float measuredRate = turned / dt;

            if (measuredRate >= kMinPlausibleRate && measuredRate <= kMaxPlausibleRate)
            {
                estimatedTurnRate_ = hasTurnRateObservation_ ? estimatedTurnRate_ * (1.0f - kNewSampleWeight) + measuredRate * kNewSampleWeight : config_.turnRate * (1.0f - kNewSampleWeight) + measuredRate * kNewSampleWeight;
                hasTurnRateObservation_ = true;
            }
        }
    }

    turnSampleAt_ = now;
    turnSampleHeading_ = player.heading;
    turnSampleDirection_ = turn_;
}

void MovementController::watchCoast(const PlayerState& player, TimePoint now)
{
    if (!coasting_)
    {
        return;
    }

    constexpr Duration kStill{80};   // no more turning seen for this long: it has stopped
    constexpr Duration kLongest{400};
    constexpr float kMostPlausible = 0.6f;
    const float turned = normalizeAngle(player.heading - coasting_->heading) * static_cast<float>(coasting_->direction);

    if (turned > coasting_->turned + 0.002f)
    {
        coasting_->turned = turned;
        coasting_->turnedAt = now;
    }

    // A new turn begun before the last one came to rest says nothing of it.
    if (turn_ != 0)
    {
        coasting_.reset();
        return;
    }

    if (now - coasting_->turnedAt < kStill && now - coasting_->releasedAt < kLongest)
    {
        return;
    }

    if (coasting_->turned <= kMostPlausible)
    {
        coast_ = hasCoast_ ? coast_ * 0.7f + coasting_->turned * 0.3f : coasting_->turned;
        hasCoast_ = true;
    }

    coasting_.reset();
}

void MovementController::releaseVertical()
{
    if (!keepVertical_)
    {
        setVertical(0);
    }
}

void MovementController::steer(const PlayerState& player, float delta, float startThreshold)
{
    syncReleasedKeys();
    const TimePoint now = clock_.now();
    const float dt = static_cast<float>(toSeconds(now - lastSteer_));

    if (dt > 0.001f && dt < 0.5f)
    {
        lastDt_ = dt;
    }

    lastSteer_ = now;
    updateTurnRateEstimate(player, now);
    watchCoast(player, now);

    // Release the turn key early enough that the next tick does not overshoot.
    // A slow update must not release the key outside the tolerance that would
    // start another turn. That inverted hysteresis caused stop/repress cycles
    // while facing a ranged target, especially after expensive path planning.
    // Use the measured turn speed to predict how far the character can rotate
    // before this tick's release is applied. A larger lead than half a tick
    // absorbs input/snapshot latency and avoids the small correction turn that
    // followed fast turns on clients rotating faster than the default rate.
    // Once a turn's end has been watched, the lead is what the character was
    // seen to go on turning (watchCoast): the tick is the least of it.
    const float leadFraction = hasTurnRateObservation_ ? 0.75f : 0.5f;
    const float lead = hasCoast_ ? std::max(coast_, estimatedTurnRate_ * lastDt_ * 0.5f) : estimatedTurnRate_ * lastDt_ * leadFraction;
    const float stopThreshold = std::min(startThreshold, std::max(config_.turnStopTolerance, lead));
    const float absDelta = std::fabs(delta);
    int wanted = delta > 0.0f ? 1 : -1;

    if (turn_ != 0 && absDelta > kPi - 0.35f)
    {
        wanted = turn_;
    }

    // The heading seen is an old one: of a turn just begun nothing shows yet,
    // and the key held until something did turned a quarter of a radian at the
    // least - too much for a small correction, which was then corrected back,
    // left and right all along the way. What the turn has done unseen is
    // reckoned in: as long as the key has been down, up to the run-on watched.
    float left = absDelta;

    if (turn_ != 0 && hasCoast_)
    {
        left -= std::min(coast_, estimatedTurnRate_ * static_cast<float>(toSeconds(now - turnPressedAt_)));
    }

    if (turn_ != 0)
    {
        if (wanted != turn_ || (hasCoast_ ? left <= config_.turnStopTolerance : absDelta <= stopThreshold))
        {
            const int was = turn_;
            setTurn(0);

            if (turn_ == 0)
            {
                coasting_ = Coast{now, player.heading, was, 0.0f, now};
            }
        }
    }
    else if (absDelta > startThreshold)
    {
        setTurn(wanted);

        if (turn_ != 0)
        {
            turnPressedAt_ = now;
        }
    }

    // If this call started or stopped a turn, the old sample interval spans
    // two different key states and must not be used on the next observation.
    if (turnSampleDirection_ != turn_)
    {
        turnSampleAt_ = now;
        turnSampleHeading_ = player.heading;
        turnSampleDirection_ = turn_;
    }
}

MoveStatus MovementController::moveTo(const PlayerState& player, const Vec3& dest, float arriveRadius, bool stopOnArrival, bool allowMovingTurn, bool followingCurve, float horizontalSpeed)
{
    syncReleasedKeys();
    if (actions_.isCasting())
    {
        stop();
        return MoveStatus::Moving;
    }

    const float dist = distance2D(player.position, dest);

    if (dist <= arriveRadius)
    {
        orbitBreak_ = false;

        if (stopOnArrival)
        {
            stop();
        }

        return MoveStatus::Arrived;
    }

    const float delta = headingDelta(player.heading, player.position, dest);
    const float absDelta = std::fabs(delta);

    // Faced, or going somewhere else now: run on.
    if (orbitBreak_ && (absDelta <= config_.faceTolerance || distance2D(dest, orbitDest_) > kOrbitDestMoved))
    {
        orbitBreak_ = false;
    }

    watchOrbit(player, dest);
    // Running while turning goes round a circle of this radius; a point off to
    // the side at angle a is on it (or outside it) from 2 r sin(a) away.
    const float runSpeed = horizontalSpeed > 0.0f ? horizontalSpeed : config_.runSpeed;
    const float turnRadius = runSpeed / estimatedTurnRate_;
    // A clear running arc can still circle a final destination inside its
    // turning radius. Intermediate curve samples are deliberately followed
    // through so the path does not become a series of stop-and-turns.
    const float arcReach = 2.0f * turnRadius * std::sin(std::min(absDelta, kPi / 2.0f)) * config_.arcMargin;
    // Turning round, or setting off the other way: on the spot, on a curve too.
    // From standing a while, as a person sets off: faced first.
    // No walking key down, forward, back or to the side, for a while.
    const TimePoint lastStep = std::max({forwardGate_.releasedAt, backwardGate_.releasedAt, strafeLeftGate_.releasedAt, strafeRightGate_.releasedAt});
    const bool stoodStill = !forward_ && !backward_ && !strafeLeft_ && !strafeRight_ && clock_.now() - lastStep >= config_.stoodStill;
    const float turnAround = forward_ ? config_.turnInPlaceAngle : stoodStill ? config_.setOffInPlaceAngle : config_.startInPlaceAngle;
    const float inPlaceThreshold = forward_ ? 0.70f : 0.50f;
    // Running straight on goes through the arrival circle, a yard of it at
    // the least: no stop to turn for that. The bot came up the side of the
    // way, a little skew, and stopped at 0.9 yd to turn for the last steps.
    const float passBy = dist * std::sin(absDelta);
    const bool runsThrough = absDelta < kPi / 2.0f && passBy < arriveRadius && arriveRadius * arriveRadius - passBy * passBy >= 0.25f;
    const bool inPlace = orbitBreak_ || (!allowMovingTurn && absDelta > inPlaceThreshold) || absDelta > turnAround || (stopOnArrival && !runsThrough && absDelta > config_.turnDeadZone && dist < (followingCurve ? std::min(arcReach, 1.0f) : arcReach));

    float steeringDelta = delta;

    if (!inPlace && allowMovingTurn)
    {
        steeringDelta = turnArcDeltaToTarget(player, dest, turnRadius).value_or(delta);
    }

    steer(player, steeringDelta, inPlace ? config_.faceTolerance : config_.turnDeadZone);
    setBackward(false);
    setStrafeLeft(false);
    setStrafeRight(false);
    setForward(!inPlace);
    return inPlace ? MoveStatus::Turning : MoveStatus::Moving;
}

bool MovementController::face(const PlayerState& player, const Vec3& target, float tolerance)
{
    return faceHeading(player, headingTo(player.position, target), tolerance);
}

bool MovementController::faceHeading(const PlayerState& player, float heading, float tolerance)
{
    if (tolerance < 0.0f)
    {
        tolerance = config_.faceTolerance;
    }

    const float delta = normalizeAngle(heading - player.heading);
    setForward(false);
    setBackward(false);
    setStrafeLeft(false);
    setStrafeRight(false);
    steer(player, delta, tolerance);
    return turn_ == 0 && std::fabs(delta) <= tolerance;
}

bool MovementController::faceWhileMoving(const PlayerState& player, const Vec3& target, float tolerance)
{
    if (tolerance < 0.0f)
    {
        tolerance = config_.faceTolerance;
    }

    const float delta = normalizeAngle(headingTo(player.position, target) - player.heading);
    steer(player, delta, tolerance);
    return turn_ == 0 && std::fabs(delta) <= tolerance;
}

} // namespace og
