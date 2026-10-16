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
 *  File: ProfileRecorder.h
 *
 *  Desc:
 *  Declarations and interface for ProfileRecorder (glider profile
 *  parsing, validation, and recording).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/profiles/GliderProfile.h"

#include <cstddef>
#include <vector>

namespace og
{

// Fast profile building:
//   F7  add waypoint       F9  start automatic recording
//   F8  undo last          F10 stop recording
// (hotkeys are wired by the UI; this class holds the logic).
class ProfileRecorder
{
public:
    enum class Target
    {
        Waypoints,
        GhostWaypoints,
        VendorWaypoints
    };

    explicit ProfileRecorder(float autoDistance = 15.0f) : autoDistance_(autoDistance)
    {
    }

    void setTarget(Target t)
    {
        target_ = t;
    }

    Target target() const
    {
        return target_;
    }

    void addWaypoint(const Vec3& position);
    void moveWaypoint(std::size_t index, const Vec3& position);
    void removeWaypoint(std::size_t index);
    void insertWaypoint(std::size_t index, const Vec3& position);
    bool undo();
    void clear();

    void startAuto()
    {
        recording_ = true;
    }

    void stopAuto()
    {
        recording_ = false;
    }

    bool isRecording() const
    {
        return recording_;
    }

    void setAutoDistance(float yards)
    {
        autoDistance_ = yards;
    }

    float autoDistance() const
    {
        return autoDistance_;
    }

    // Call with every new player position. In automatic mode a waypoint is
    // added whenever the player moved more than autoDistance from the last one.
    // Returns true if a waypoint was added.
    bool update(const Vec3& position);

    const std::vector<Vec3>& points(Target t) const;
    const std::vector<Vec3>& points() const
    {
        return points(target_);
    }

    std::size_t count() const
    {
        return points().size();
    }

    GliderProfile& profile()
    {
        return profile_;
    }

    const GliderProfile& profile() const
    {
        return profile_;
    }

    void setProfile(GliderProfile profile)
    {
        profile_ = std::move(profile);
    }

private:
    std::vector<Vec3>& mutablePoints();

    GliderProfile profile_;
    Target target_ = Target::Waypoints;
    float autoDistance_;
    bool recording_ = false;
};

} // namespace og
