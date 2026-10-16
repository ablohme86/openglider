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
 *  File: ZoneBounds.h
 *
 *  Desc:
 *  Declarations and interface for ZoneBounds (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

// Where a zone's map lies in the world, to turn Questie's map coordinates
// (percent across the zone's map) into world ones. The world's x runs down the
// map and its y right to left, as in the client's WorldMapArea:
//   world x = top  - mapY / 100 * (top  - bottom)
//   world y = left - mapX / 100 * (left - right)
class SqliteDb;

// Where a zone's map lies in the world, to turn Questie's map coordinates
// (percent across the zone's map) into world ones. The world's x runs down the
// map and its y right to left, as in the client's WorldMapArea:
//   world x = top  - mapY / 100 * (top  - bottom)
//   world y = left - mapX / 100 * (left - right)
// Zones known from data/quests/zones.json; others are learnt from NPCs seen
// where Questie has their one spawn (calibrate()).
class ZoneBounds
{
public:
    struct Bounds
    {
        double left = 0, right = 0, top = 0, bottom = 0;
    };

    bool loadFromJson(std::string_view text, std::string* error = nullptr);
    bool loadFromFile(const std::string& path, std::string* error = nullptr);
    bool loadFromDb(const SqliteDb& db, std::string* error = nullptr);
    std::string toJson() const;

    void set(std::uint32_t zone, Bounds bounds)
    {
        bounds_[zone] = bounds;
    }

    std::optional<Bounds> find(std::uint32_t zone) const;
    // The world x and y of a spot on the zone's map; the height is left 0.
    std::optional<Vec3> toWorld(std::uint32_t zone, double mapX, double mapY) const;

    // One NPC seen at `world` whose one spawn Questie puts at (mapX, mapY) in
    // the zone. A zone not known yet is worked out once there are samples far
    // enough apart on both axes; true when that happened with this one.
    bool calibrate(std::uint32_t zone, double mapX, double mapY, const Vec3& world);

private:
    struct Sample
    {
        double mapX, mapY, worldX, worldY;
    };
    std::map<std::uint32_t, Bounds> bounds_;
    std::map<std::uint32_t, std::vector<Sample>> samples_;
};

} // namespace og
