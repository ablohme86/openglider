/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-15
 *  File: Avoidance.h
 *
 *  Desc:
 *  Declarations and interface for Avoidance (navigation, pathfinding,
 *  movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/navigation/INavigationProvider.h"
#include "og/navigation/ObstacleMemory.h"
#include <span>

namespace og
{
// Zones occupy the XY plane; heights are retained for waypoint steering.
bool clearOfZones(const Vec3& from, const Vec3& to, std::span<const AvoidZone> zones, float margin = 0.0f);
// Bounded A* search, followed by line-of-sight simplification. No path is safer
// than silently crossing a painted area when the search budget is exhausted.
// `step` is the grid's spacing: finer finds narrower gaps, at more work.
std::optional<Path> pathAroundZones(const Vec3& from, const Vec3& to, std::span<const AvoidZone> zones, float margin = 3.0f, float step = 3.0f, std::stop_token stop = {}, const std::function<void(int)>& progress = {});
} // namespace og
