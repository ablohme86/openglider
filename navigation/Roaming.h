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
 *  File: Roaming.h
 *
 *  Desc:
 *  Declarations and interface for Roaming (navigation, pathfinding,
 *  movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace og
{

class ObstacleMemory;
class INavigationProvider;

// Roaming inside a route: instead of walking the waypoints in order, the bot
// walks to a random spot within the area they outline, then picks another.

// Whether `point` lies inside the polygon the route's waypoints outline (x and y only).
bool insideRoute(const std::vector<Vec3>& route, const Vec3& point);
// Whether the route outlines an area to roam in, rather than a path walked
// there and back.
bool outlinesArea(const std::vector<Vec3>& route);

// A random spot to walk to: inside the area the route outlines, at least
// `minDistance` from `from`, outside every area `obstacles` avoids and with a
// roamPath() to it. A route that outlines no area (fewer than
// three points, or one walked there and back) gives spots near its waypoints
// instead. The height is the nearest waypoint's. nullopt when nothing fits.
// `state` is the random generator's state, advanced on every draw.
std::optional<Vec3> pickRoamPoint(const std::vector<Vec3>& route, const Vec3& from, const ObstacleMemory* obstacles, std::uint64_t& state, float minDistance, int attempts = 60, INavigationProvider* provider = nullptr, std::uint32_t zone = 0);

// Whether a point lies in the zone (0: any), by the map's own areas: a zone's
// outline on the world map is a rectangle that takes in the edges of its
// neighbours. A point the map tells nothing of counts as inside.
bool insideZone(const INavigationProvider* provider, std::uint32_t zone, const Vec3& point);

// The way from `from` to `to` that stays inside the area the route outlines:
// the straight line when it does, otherwise round the corners it would cut,
// each taken `inset` yards inside the area. Starting outside the area (a chase
// took the bot out), it first heads back in at the nearest point. Every leg
// stays clear of what `obstacles` avoids. Ends at `to`; empty when no way fits.
// A route that outlines no area gives the straight line.
std::vector<Vec3> roamPath(const std::vector<Vec3>& route, const Vec3& from, const Vec3& to, const ObstacleMemory* obstacles, float inset = 3.0f);

// "Build Route" (experimental): a closed circuit through the area the route
// outlines, to walk point by point and replace with a new one after each lap.
// Anchors spread over small areas are joined by a short tour; large areas
// get alternating lanes through the interior. Legs go round corners and
// obstacles through nearby reachable anchors. They are cut into pieces of
// about 8 yards, bent into a gentle sway and
// rounded off, so the bot does not cross the area in long straight lines.
// Every point and leg stays inside the area and at
// least `clearance` yards clear of No Go zones and learned obstacles - little
// enough to pass between two that stand close together. Starts at the point
// nearest `from`; empty when the route outlines no area.
std::vector<Vec3> buildCircuit(const std::vector<Vec3>& area, const Vec3& from, const ObstacleMemory* obstacles, std::uint64_t& state, float clearance = 1.0f);

// The circuit buildCircuit() lays out, walked on the client map: large areas
// are covered in alternating interior lanes joined through local walkable
// anchors. The completed loop is followed until the lap ends. Each leg goes
// straight where the map allows it and round what stands in the way where it
// does not. A point with no way to it inside the area is left out.
std::vector<Vec3> buildClientCircuit(const std::vector<Vec3>& area, const Vec3& from, INavigationProvider& provider, const ObstacleMemory* obstacles, std::uint64_t& state);

// A No Go zone or obstacle came up: each stretch of `circuit` from `from` on
// that now runs into one is replaced by a way round it, and the rest - the
// part already walked included - kept as it was. False, leaving the circuit
// alone, when there is no way round (build a new one then).
bool repairCircuit(const std::vector<Vec3>& area, std::vector<Vec3>& circuit, std::size_t from, const ObstacleMemory* obstacles, float clearance = 1.0f);

// The way back onto a circuit after a fight or a gather: round No Go zones and
// obstacles with `clearance` to spare, as the circuit itself is built, ending at
// `to`. From outside the area (a chase took the bot out), first back in.
std::vector<Vec3> circuitWay(const std::vector<Vec3>& area, const Vec3& from, const Vec3& to, const ObstacleMemory* obstacles, float clearance = 1.0f);

// A gathering detour: from `from` through each of `stops` in turn, every leg
// found as circuitWay() finds it, then cut into pieces of about 8 yards pushed
// gently sideways and its corners rounded off, so the bot does not walk a
// ruler-straight line to a node. The stops themselves are kept exactly;
// `stopEnds`, when given, gets each stop's index in the result. Without an
// area only No Go zones and learned obstacles bound the sway.
std::vector<Vec3> detourPath(const std::vector<Vec3>& area, const Vec3& from, const std::vector<Vec3>& stops, const ObstacleMemory* obstacles, std::uint64_t& state, std::vector<std::size_t>* stopEnds = nullptr);

} // namespace og
