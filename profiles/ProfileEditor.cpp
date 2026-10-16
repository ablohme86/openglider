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
 *  File: ProfileEditor.cpp
 *
 *  Desc:
 *  Implementation of ProfileEditor (the profile being drawn: its
 *  grind, ghost and vendor waypoints).
 *
 *
 */
#include "og/profiles/ProfileEditor.h"

namespace og
{

std::vector<Vec3>& ProfileEditor::mutablePoints()
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

const std::vector<Vec3>& ProfileEditor::points(Target t) const
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

void ProfileEditor::addWaypoint(const Vec3& position)
{
    mutablePoints().push_back(position);
}

void ProfileEditor::moveWaypoint(std::size_t index, const Vec3& position)
{
    auto& pts = mutablePoints();

    if (index < pts.size())
    {
        pts[index] = position;
    }
}

void ProfileEditor::removeWaypoint(std::size_t index)
{
    auto& pts = mutablePoints();

    if (index < pts.size())
    {
        pts.erase(pts.begin() + index);
    }
}

void ProfileEditor::insertWaypoint(std::size_t index, const Vec3& position)
{
    auto& pts = mutablePoints();

    if (index <= pts.size())
    {
        pts.insert(pts.begin() + index, position);
    }
}

bool ProfileEditor::undo()
{
    auto& pts = mutablePoints();

    if (pts.empty())
    {
        return false;
    }

    pts.pop_back();
    return true;
}

void ProfileEditor::clear()
{
    mutablePoints().clear();
}

} // namespace og
