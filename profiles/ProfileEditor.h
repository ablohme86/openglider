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
 *  File: ProfileEditor.h
 *
 *  Desc:
 *  Declarations and interface for ProfileEditor (the profile being
 *  drawn: its grind, ghost and vendor waypoints).
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

// The profile being edited: waypoints drawn, moved and removed on the map,
// one route at a time (the UI does the drawing; this class holds the logic).
class ProfileEditor
{
public:
    enum class Target
    {
        Waypoints,
        GhostWaypoints,
        VendorWaypoints
    };

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
};

} // namespace og
