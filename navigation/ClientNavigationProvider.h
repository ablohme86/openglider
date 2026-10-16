/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-30
 *  File: ClientNavigationProvider.h
 *
 *  Desc:
 *  Declarations and interface for ClientNavigationProvider (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include <functional>
#include "og/navigation/ClientGeometry.h"
#include "og/navigation/INavigationProvider.h"
#include <filesystem>
#include <atomic>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace og
{
class Logger;
class ObstacleMemory;
class IClientGeometrySource
{
public:
    virtual ~IClientGeometrySource() = default;
    virtual CollisionGeometry load(std::uint32_t map, int col, int row, std::stop_token stop) = 0;
    // The ground textures of a map tile (its _tex0 file's MDID ids), to tell
    // which are roads; nullopt where the file could not be read.
    virtual std::optional<std::vector<std::uint32_t>> groundTextures(std::uint32_t map, int col, int row)
    {
        (void)map;
        (void)col;
        (void)row;
        return std::nullopt;
    }
    // The water levels of a map tile (parseAdtWaterLevels); empty where it
    // has none or could not be read.
    virtual std::vector<float> waterLevels(std::uint32_t map, int col, int row)
    {
        (void)map;
        (void)col;
        (void)row;
        return {};
    }
    // The areas of a map tile's chunks (parseAdtAreaIds); empty where it
    // could not be read.
    virtual std::vector<std::uint32_t> areaIds(std::uint32_t map, int col, int row)
    {
        (void)map;
        (void)col;
        (void)row;
        return {};
    }
};
// Reads the attached client's CASC terrain and collision files. Separate handle
// from the radar; all reads happen on the bot thread.
std::unique_ptr<IClientGeometrySource> openClientGeometry(const std::filesystem::path& storage, std::uint32_t build, std::string& error);
class ClientNavigationProvider final : public INavigationProvider
{
public:
    ClientNavigationProvider(std::unique_ptr<IClientGeometrySource> source, Logger& log, ObstacleMemory& obstacles, std::filesystem::path cache);
    ~ClientNavigationProvider();
    std::string name() const override
    {
        return "client pathfinding";
    }

    std::optional<Path> findPath(const Vec3& from, const Vec3& to) override;
    std::optional<Path> findLoadedPath(const Vec3& from, const Vec3& to) override;
    void setMap(std::optional<std::uint32_t> map) override;
    bool usesClientGeometry() const override
    {
        return true;
    }

    bool canPlanWithoutLoading(const Vec3& from, const Vec3& to) const override;
    void setUseRoads(bool on) override
    {
        useRoads_ = on;
    }

    void setAvoidWater(bool on) override
    {
        avoidWater_ = on;
    }

    bool canWalkStraight(const Vec3& from, const Vec3& to) const override;
    std::optional<Vec3> findWalkablePoint(const Vec3& near, float maxDistance = 2.0f, float clearance = 1.0f) const override;
    std::optional<Ground> groundAt(const Vec3& at, float farthest = 12.0f) const override;
    void setAvoiding(std::vector<Avoided> circles) override
    {
        avoiding_ = std::move(circles);
    }

    bool canAvoid() const override
    {
        return true;
    }
    std::optional<float> groundBelow(const Vec3& at) const override;
    std::optional<float> waterSurface(const Vec3& at) const override;
    std::optional<std::uint32_t> areaAt(const Vec3& at) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> useRoads_{true};
    std::atomic<bool> avoidWater_{true};
    std::vector<Avoided> avoiding_;
};
// The small steps out of a way: a corner within `within` yards of the line
// past it, where that line can be walked straight, is left out. Along a road
// the way followed the road's cells, a stair of corners a few yards apart;
// a bend of the road, far off the line, stays.
std::vector<Vec3> smoothSmallSteps(const Vec3& from, const std::vector<Vec3>& points, const std::function<bool(const Vec3&, const Vec3&)>& straight, float within = 3.0f);

} // namespace og
