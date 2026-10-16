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
 *  File: ProfileRecorder.cpp
 *
 *  Desc:
 *  Implementation of ProfileRecorder (glider profile parsing,
 *  validation, and recording).
 *
 *
 */
#include "og/profiles/ProfileRecorder.h"

namespace og
{

std::vector<Vec3>& ProfileRecorder::mutablePoints()
{
    switch (target_)
    {
    case Target::GhostWaypoints:
        return profile_.ghostWaypoints;
    case Target::VendorWaypoints:
        return profile_.vendorWaypoints;
    case Target::Waypoints:
        break;
    }

    return profile_.waypoints;
}

const std::vector<Vec3>& ProfileRecorder::points(Target t) const
{
    switch (t)
    {
    case Target::GhostWaypoints:
        return profile_.ghostWaypoints;
    case Target::VendorWaypoints:
        return profile_.vendorWaypoints;
    case Target::Waypoints:
        break;
    }

    return profile_.waypoints;
}

void ProfileRecorder::addWaypoint(const Vec3& position)
{
    mutablePoints().push_back(position);
}

void ProfileRecorder::moveWaypoint(std::size_t index, const Vec3& position)
{
    auto& pts = mutablePoints();

    if (index < pts.size())
    {
        pts[index] = position;
    }
}

void ProfileRecorder::removeWaypoint(std::size_t index)
{
    auto& pts = mutablePoints();

    if (index < pts.size())
    {
        pts.erase(pts.begin() + index);
    }
}

void ProfileRecorder::insertWaypoint(std::size_t index, const Vec3& position)
{
    auto& pts = mutablePoints();

    if (index <= pts.size())
    {
        pts.insert(pts.begin() + index, position);
    }
}

bool ProfileRecorder::undo()
{
    auto& pts = mutablePoints();

    if (pts.empty())
    {
        return false;
    }

    pts.pop_back();
    return true;
}

void ProfileRecorder::clear()
{
    mutablePoints().clear();
}

bool ProfileRecorder::update(const Vec3& position)
{
    if (!recording_)
    {
        return false;
    }

    auto& pts = mutablePoints();

    if (pts.empty() || distance(pts.back(), position) >= autoDistance_)
    {
        pts.push_back(position);
        return true;
    }

    return false;
}

} // namespace og
