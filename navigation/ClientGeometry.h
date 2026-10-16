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
 *  File: ClientGeometry.h
 *
 *  Desc:
 *  Declarations and interface for ClientGeometry (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once
#include "og/common/Vec3.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace og
{
// What a triangle of ground is (CollisionGeometry::roads).
constexpr std::uint8_t kRoadGround = 1;
constexpr std::uint8_t kSwimmingGround = 2;
// A building's or a model's surface, not the ground's: a stair may be steeper.
constexpr std::uint8_t kObjectSurface = 3;

struct CollisionGeometry
{
    std::vector<Vec3> vertices;
    std::vector<int> triangles;
    // One to a triangle: kRoadGround where the ground is a road,
    // kSwimmingGround the bed of water deep enough to swim. Empty when none is.
    std::vector<std::uint8_t> roads;
    void append(const CollisionGeometry& other);
};
struct MapObjectPlacement
{
    std::uint32_t fileId = 0;
    std::uint32_t uniqueId = 0;
    Vec3 position; // client ADT model coordinates, converted when placed
    Vec3 rotation;
    float scale = 1;
    bool worldModel = false;
    std::uint16_t doodadSet = 0; // a world model's set of furniture, besides the default one
};
using ClientFileReader = std::function<std::vector<std::uint8_t>(std::uint32_t)>;
// The terrain of a root ADT; `roadCells` (from parseAdtRoadCells, or empty)
// marks the triangles of each road cell.
// With `roadsOnly`, only what keepRoads takes for a road is tagged as one -
// looked at once the water has been taken out, which leaves a stream's dirt
// bed as two thin banks.
CollisionGeometry parseAdtTerrain(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> roadCells = {}, bool roadsOnly = false);
// How high the water stands over each of a terrain tile's 256 * 64 cells
// (MH2O), NaN where there is none; empty without any.
std::vector<float> parseAdtWaterLevels(std::span<const std::uint8_t> bytes);
// The water's height over (x, y) in the tile at col (from y) and row (from
// x); nullopt where there is no water or the point is not in the tile.
std::optional<float> waterLevelAt(const std::vector<float>& levels, int col, int row, float x, float y);
// The area (AreaTable ID: a zone, or a subzone of one) of each of a terrain
// tile's 16x16 chunks, in the file's order; empty when they cannot be read.
std::vector<std::uint32_t> parseAdtAreaIds(std::span<const std::uint8_t> bytes);
// The area at a world x and y within map tile (col, row); nullopt outside the
// tile, or where the tile has none.
std::optional<std::uint32_t> areaIdAt(const std::vector<std::uint32_t>& areas, int col, int row, float x, float y);
// Whether a texture is a road's: cobblestone, a stone road, the dirt road
// through Elwynn. By file id, or by name where the file names its textures.
bool isRoadTexture(std::uint32_t fileId);
bool isRoadTexture(std::string_view name);
// From a texture ADT (_tex0): for each of the 256 terrain chunks' 8x8 cells,
// in order, 1 where road textures cover at least half the cell. Empty where
// the tile has no road.
std::vector<std::uint8_t> parseAdtRoadCells(std::span<const std::uint8_t> tex0);
// Of a tile's road cells (parseAdtRoadCells), those that are a road's. The
// same dirt lies round a field, under a silo and along the paths between a
// farm's buildings: the way to Eastvale ran round Brackwell Pumpkin Patch's
// field and right up to its silo. A road goes somewhere: its cells reach the
// tile's edge, or run for a couple of hundred yards. A line of dirt a cell
// wide is no road either. Empty where nothing is left.
std::vector<std::uint8_t> keepRoads(std::vector<std::uint8_t> cells);
// A texture ADT's ground textures by file id (MDID); empty where it names them (MTEX).
std::vector<std::uint32_t> parseAdtTextureIds(std::span<const std::uint8_t> tex0);
std::vector<MapObjectPlacement> parseAdtObjects(std::span<const std::uint8_t> bytes);
CollisionGeometry parseModelCollision(std::span<const std::uint8_t> bytes);
// A world model's walls and floors, and the furniture it places itself -
// candle stands, benches, barrels - from its default doodad set and
// `doodadSet`, each a model of its own with its own collision.
CollisionGeometry parseWorldModelCollision(std::span<const std::uint8_t> root, const ClientFileReader& read, std::uint16_t doodadSet = 0);
CollisionGeometry placeModel(const CollisionGeometry& geometry, const MapObjectPlacement& placement);
std::vector<std::uint32_t> adtFileIds(std::span<const std::uint8_t> wdt, int col, int row);
// The texture ADT (_tex0) of a tile; 0 where there is none.
std::uint32_t adtTextureFileId(std::span<const std::uint8_t> wdt, int col, int row);
} // namespace og
