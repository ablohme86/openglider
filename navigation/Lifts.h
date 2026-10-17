/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-17
 *  File: Lifts.h
 *
 *  Desc:
 *  The lifts of Thunder Bluff and Undercity: where they stop below and
 *  above, and where each car is now.
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Vec3.h"
#include "og/state/GameSnapshot.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace og
{

// One lift: a car going up and down a shaft between two stops, standing a
// few seconds at each (the Mesa Elevators of Thunder Bluff: 5 s below, 10 s
// up, 5 s above, 10 s down).
struct Lift
{
    std::uint32_t mapId = 0;
    // The middle of the car, seen from above.
    float x = 0.0f;
    float y = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;
    // How far from the middle the car's floor holds a character.
    static constexpr float kFloor = 5.0f;

    Vec3 stop(bool upper) const
    {
        return {x, y, upper ? top : bottom};
    }
};

// Where a lift's car is now.
struct LiftCar
{
    float z = 0.0f;
    bool moving = false;
    // How long it has stood where it is.
    Duration still{};
};

// The lifts known - those measured, and those seen going up and down - and
// their cars, from the objects in sight ("Mesa Elevator").
class LiftWatch
{
public:
    explicit LiftWatch(const IClock& clock);

    void update(const GameSnapshot& snapshot);
    const std::vector<Lift>& lifts() const
    {
        return lifts_;
    }
    // Counts up with every lift learnt.
    int revision() const
    {
        return static_cast<int>(lifts_.size());
    }
    // The car of a lift, while it is in sight.
    std::optional<LiftCar> car(const Lift& lift) const;
    // A lift's object: one named so, a car or the shaft it goes in.
    static bool isLift(const GameObjectState& object);

private:
    struct Seen
    {
        Guid guid = 0;
        std::uint32_t mapId = 0;
        Vec3 at;
        float low = 0.0f;
        float high = 0.0f;
        // Where it was a moment ago: moving is judged over a few tenths of a
        // second - from one tick to the next a car standing at a stop wobbled
        // by as much as one starting off moves.
        Vec3 sample;
        TimePoint sampleAt{};
        TimePoint movedAt{};
        TimePoint seenAt{};
    };

    const IClock& clock_;
    std::vector<Lift> lifts_;
    std::vector<Seen> seen_;
};

} // namespace og
