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
 *  File: INavigationProvider.h
 *
 *  Desc:
 *  Declarations and interface for INavigationProvider (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

namespace og
{

struct Path
{
    std::vector<Vec3> points; // excluding the start position, ending at the destination
    // A local section of a longer trip; request the next section at its end.
    bool continues = false;
};

// Finds a path between two positions. The rest of the bot does not care which
// implementation is used (direct line, navmesh, ...).
class INavigationProvider
{
public:
    virtual ~INavigationProvider() = default;
    virtual std::string name() const = 0;
    virtual std::optional<Path> findPath(const Vec3& from, const Vec3& to) = 0;
    virtual void setMap(std::optional<std::uint32_t>)
    {
    }

    virtual bool usesClientGeometry() const
    {
        return false;
    }

    // Speculative planning while keys are held must not load/build map tiles.
    virtual bool canPlanWithoutLoading(const Vec3&, const Vec3&) const
    {
        return !usesClientGeometry();
    }

    // The way through what canPlanWithoutLoading found loaded, and no farther
    // round: a wider look, which loads and builds tiles, is left for a plan
    // made standing.
    virtual std::optional<Path> findLoadedPath(const Vec3& from, const Vec3& to)
    {
        return findPath(from, to);
    }

    // Long ways keep to the roads (where the map knows them) or go as the
    // ground lies.
    virtual void setUseRoads(bool)
    {
    }

    // Water deep enough to swim is crossed only where there is no bridge or
    // way round near (on); a ghost walks on it, and goes straight (off).
    virtual void setAvoidWater(bool)
    {
    }

    // A tall character (a tauren): under a low roof only where there is no
    // way round it.
    virtual void setTall(bool)
    {
    }

    // A shortcut must stay on connected walking surfaces, including its end floor.
    virtual bool canWalkStraight(const Vec3&, const Vec3&) const
    {
        return false;
    }

    // The height of the ground under a point that has none at its own level:
    // the bed of the lake a swimmer - or a ghost, walking on the water - is
    // over. nullopt where the point stands on the map, or nothing lies below.
    virtual std::optional<float> groundBelow(const Vec3&) const
    {
        return std::nullopt;
    }

    // The height of the water's surface over a point (a lake, a river, the
    // sea); nullopt where there is no water, or the map is not known.
    virtual std::optional<float> waterSurface(const Vec3&) const
    {
        return std::nullopt;
    }

    // The area of the map a point lies in (AreaTable ID: a zone, or a subzone
    // of one); nullopt where the map does not tell.
    virtual std::optional<std::uint32_t> areaAt(const Vec3&) const
    {
        return std::nullopt;
    }

    // Circles (a centre and a radius, on the map's plane, and how far up and
    // down they hold) the ways found from
    // here on keep out of, where the map has another way; none to let go of
    // them. For the one search that asks: the navigator plans round zones
    // itself, and falls back on this where its own way round fails.
    struct Avoided
    {
        Vec3 centre;
        float radius = 0.0f;
        float reach = 0.0f; // up and down from the centre; 0: at every height
    };

    virtual void setAvoiding(std::vector<Avoided>)
    {
    }

    virtual bool canAvoid() const
    {
        return false;
    }

    // What the map has at a point the character stands on: whether it is road,
    // and how far the nearest wall (the edge of the walkable ground) is, up to
    // `farthest`. nullopt where the map does not tell.
    struct Ground
    {
        bool road = false;
        float wall = 0.0f;
    };

    virtual std::optional<Ground> groundAt(const Vec3&, float farthest = 12.0f) const
    {
        (void)farthest;
        return std::nullopt;
    }

    // Verifies or snaps a point to a walkable surface with clearance from collision
    // walls/obstacles, returning true ground elevation Z, or std::nullopt if unwalkable.
    virtual std::optional<Vec3> findWalkablePoint(const Vec3& near, float maxDistance = 2.0f, float clearance = 1.0f) const
    {
        return near;
    }
};

// Straight line. This is the classic Glider behaviour: the profile's waypoints
// are assumed to be placed so that walking directly between them works.
class DirectNavigationProvider final : public INavigationProvider
{
public:
    std::string name() const override
    {
        return "direct";
    }

    std::optional<Path> findPath(const Vec3&, const Vec3& to) override
    {
        return Path{{to}};
    }
};

} // namespace og
