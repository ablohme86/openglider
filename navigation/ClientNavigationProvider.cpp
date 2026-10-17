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
 *  File: ClientNavigationProvider.cpp
 *
 *  Desc:
 *  Implementation of ClientNavigationProvider (navigation, pathfinding,
 *  movement control, and stuck recovery).
 *
 *
 */
#include "og/navigation/ClientNavigationProvider.h"
#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"
#include "og/common/Log.h"
#include "og/navigation/ObstacleMemory.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace og
{
namespace
{
constexpr float kTile = 64;
constexpr float kAdt = 533.333333f;
// A route is searched through the tiles holding both ends and this many round
// them, for a way round; at most this many tiles at once.
constexpr int kMargin = 3;
constexpr int kMaxWindow = 196;
// Too far for one window: the tiles along the coarse way instead, this many
// to either side of it - a corridor as long as the mesh has room for, and the
// whole way at once where the corridor reaches the destination.
constexpr int kCorridorMargin = 2;
constexpr std::size_t kMaxCorridor = 230; // of the 240 the mesh keeps loaded
// With no coarse way: a leg this long towards the destination.
constexpr float kLeg = 400;
using Key = std::pair<int, int>;
using Floats = std::array<float, 3>;

// Both ends on the map, and no way between them within the tiles looked at:
// a wider look may find one. Any other failure (no ground at an end) it won't.
struct NoRouteWithin : std::runtime_error
{
    NoRouteWithin() : std::runtime_error("no complete walking route through client geometry")
    {
    }
};
Floats nav(Vec3 p)
{
    return {p.x, p.z, p.y};
}

Vec3 world(const float* p)
{
    return {p[0], p[2], p[1]};
}

// The tiles within `margin` tiles of a point, added to `tiles`.
void addAround(std::set<Key>& tiles, const Vec3& p, int margin)
{
    const int x = static_cast<int>(std::floor(p.x / kTile)), y = static_cast<int>(std::floor(p.y / kTile));

    for (int dx = -margin; dx <= margin; ++dx)
    {
        for (int dy = -margin; dy <= margin; ++dy)
        {
            tiles.insert({x + dx, y + dy});
        }
    }
}

int windowTiles(const Vec3& a, const Vec3& b)
{
    const int x0 = static_cast<int>(std::floor(std::min(a.x, b.x) / kTile)) - kMargin;
    const int x1 = static_cast<int>(std::floor(std::max(a.x, b.x) / kTile)) + kMargin;
    const int y0 = static_cast<int>(std::floor(std::min(a.y, b.y) / kTile)) - kMargin;
    const int y1 = static_cast<int>(std::floor(std::max(a.y, b.y) / kTile)) + kMargin;
    return (x1 - x0 + 1) * (y1 - y0 + 1);
}

// How a navigation mesh is cut and sampled. The fine one is what the bot walks
// by; the coarse one, with tiles four times as wide and cells four times as
// big, only finds the way in broad strokes across a zone or two - over the
// pass at Three Corners rather than into the mountains round Redridge - for
// fine legs to follow.
struct Grid
{
    float tile;         // yards a tile is wide
    float cs, ch;       // cell size and height
    int walkableHeight; // in cells of ch
    int walkableClimb;  // in cells of ch
    int walkableRadius; // in cells of cs
    int borderSize;     // in cells of cs
    float detailError;
    const char* folder; // cache folder under the map's, "" for none
};
// A character fits under 1.6 yards and steps up 1 yard, as the game lets it -
// and as the server's own maps (TrinityCore's mmaps) have it. With 2 yards of
// headroom and 0.6 of step, the tight, steep spiral stair up the tower of
// Northshire Abbey was left out of the map, and the way to Brother Neals ended
// on the floor below him.
// Refine tiles with overlapping floors: half-yard cells lose narrow stairs
// such as Bliztik's. Keep the same half-yard character radius and tile border.
// Separate fine caches prevent reuse of the old disconnected stair tiles.
constexpr Grid kFine{kTile, 0.5f, 0.2f, 8, 5, 1, 5, 0.2f, "fine-v6"};
constexpr Grid kIndoor{kTile, 0.25f, 0.2f, 8, 5, 2, 10, 0.2f, "fine-v6"};
// Two-yard samples on a walkable slope can differ by almost three yards.
// Keeping the fine mesh's one-yard step limit here disconnected ordinary
// hills (including the approach to Lakeshire) and lost the whole-zone route.
// This mesh only guides the search; the fine mesh still verifies one-yard
// steps before returning a route the character can actually walk.
constexpr Grid kCoarse{256.0f, 2.0f, 0.5f, 3, 6, 0, 3, 1.0f, "coarse-v5"};
// When the full coarse window does not fit, plan this far towards the goal.
constexpr float kCoarseReach = kCoarse.tile * 10;
// The fine mesh resolves the last doorway/floor around the coarse endpoint.
constexpr float kCoarseGoalTolerance = 32;
// Kept out of what is avoided, a goal found on its own floor is not given up
// for another floor further from it than this (up or down).
constexpr float kGoalFloorReach = 4.0f;
// Steeper than this is a wall (the game's own limit lies about here): a
// building's stair or a model's surface. The tight spiral stair up the tower
// of Northshire Abbey needs it.
constexpr float kWalkableSlope = 55;
// The ground: a hillside this steep is slid down in the game. At 55 degrees
// the way to a quest's place in Dun Morogh went straight up a mountainside,
// and the bot stood stuck on it.
constexpr float kGroundSlope = 50;

// Sample upward walkable triangles at the same horizontal positions. A
// height range here means stacked surfaces, not merely sloping terrain.
// Sampling every two yards is enough to detect a landing or room; Recast
// itself still rasterizes all triangles at the selected resolution.
bool hasStackedFloors(const CollisionGeometry& geometry, float x0, float y0)
{
    constexpr float spacing = 2.0f;
    constexpr int side = static_cast<int>(kTile / spacing);
    std::array<float, side * side> low, high;
    low.fill(std::numeric_limits<float>::max());
    high.fill(std::numeric_limits<float>::lowest());
    const float minUp = std::cos(kWalkableSlope * 3.14159265f / 180.0f);

    for (std::size_t t = 0; t < geometry.triangles.size(); t += 3)
    {
        const Vec3 a = geometry.vertices[geometry.triangles[t]];
        const Vec3 ab = geometry.vertices[geometry.triangles[t + 1]] - a;
        const Vec3 ac = geometry.vertices[geometry.triangles[t + 2]] - a;
        const float nx = ab.y * ac.z - ab.z * ac.y;
        const float ny = ab.z * ac.x - ab.x * ac.z;
        const float nz = ab.x * ac.y - ab.y * ac.x;
        // World XY swaps Recast's Y/Z, so upward triangles have negative nz.
        if (nz >= 0 || nz * nz < minUp * minUp * (nx * nx + ny * ny + nz * nz))
        {
            continue;
        }

        const Vec3 b = a + ab, c = a + ac;
        const int firstX = std::max(0, static_cast<int>(std::ceil((std::min({a.x, b.x, c.x}) - x0) / spacing - 0.5f)));
        const int lastX = std::min(side - 1, static_cast<int>(std::floor((std::max({a.x, b.x, c.x}) - x0) / spacing - 0.5f)));
        const int firstY = std::max(0, static_cast<int>(std::ceil((std::min({a.y, b.y, c.y}) - y0) / spacing - 0.5f)));
        const int lastY = std::min(side - 1, static_cast<int>(std::floor((std::max({a.y, b.y, c.y}) - y0) / spacing - 0.5f)));
        for (int x = firstX; x <= lastX; ++x)
        {
            for (int y = firstY; y <= lastY; ++y)
            {
                const float dx = x0 + (x + 0.5f) * spacing - a.x;
                const float dy = y0 + (y + 0.5f) * spacing - a.y;
                const float u = (dx * ac.y - dy * ac.x) / nz;
                const float v = (ab.x * dy - ab.y * dx) / nz;
                if (u < 0 || v < 0 || u + v > 1)
                {
                    continue;
                }
                const float z = a.z + u * ab.z + v * ac.z;
                const int index = x * side + y;
                low[index] = std::min(low[index], z);
                high[index] = std::max(high[index], z);
                if (high[index] - low[index] > 3.0f)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// Ground steeper than kSteepSlope is technically walkable, but costly at every
// distance. This makes the route prefer a gentler way back out of a crater;
// the steep way remains available when it is the only connected route.
constexpr float kSteepSlope = 35;
constexpr unsigned char kSteepArea = 1; // area id while building; 1 in the mesh
constexpr float kSteepCost = 8;
// Under a roof lower than kHeadroom: the map has a character fit under 1.6
// yards (for the tight stairs of towers), and a tauren does not - from the
// inn of Orgrimmar to its flight tower the way ran under the low eaves of the
// bank, some 2.8 yd up, and the tauren stuck against them, a ramp up beside
// them. For a tall character such ground costs kLowCost: kept to where it is
// the only way.
constexpr float kHeadroom = 3.0f;
constexpr unsigned char kLowArea = 5; // area id while building; 5 in the mesh
constexpr float kLowCost = 8;
// A hillside, kHillSlope up to kSteepSlope, costs kHillCost: walked where it
// is the way, not climbed for a shorter line - the way from Crystal Lake to
// Milly's farm went over the ridge north of it, 60 yards up, beside the trail
// round it.
constexpr float kHillSlope = 20;
constexpr unsigned char kHillArea = 4; // area id while building; 4 in the mesh
constexpr float kHillCost = 2.5f;
constexpr float kRoadNearGoal = 50;
// Roads (the ground's road textures) cost kRoadCost of open ground from as
// far off: a long way keeps to them as a player does - the road from
// Goldshire to Northshire, not across the fields - as long as the road is
// not much longer. Near the destination they cost as much as any ground.
constexpr unsigned char kRoadArea = 2; // area id while building; 2 in the mesh
constexpr float kRoadCost = 0.6f;
// From this far, roads cost kLongRoadCost: across the Barrens from Thunder
// Bluff to Camp Taurajo the way kept to the road for 3 of 154 polygons, the
// open ground straight across cheaper than the road round. A long way keeps
// to the roads it can, a short one only where they are hardly longer.
constexpr float kLongRoadFrom = 300;
constexpr float kLongRoadCost = 0.25f;
// The costs, scaled so none is under 1: Detour's search takes a yard to cost
// at least 1, and with the road cheaper it went straight across the field.
// The bed of water deep enough to swim: a bridge, or the way round, rather
// than across - by Bloodhoof Village the way swam over the river beside the
// bridge. A ghost walks on the water: no dearer for it.
constexpr unsigned char kWaterArea = 3; // area id while building; 3 in the mesh
constexpr float kWaterCost = 10;

// The polygons under circles to keep out of, closed for one search: flagged
// so the search's filter passes them by, and opened again after it. Those
// the ends of the way stand on stay open - there is no way from or to a
// closed one.
constexpr unsigned short kClosedFlag = 2;

class Closed
{
public:
    // A circle: its middle (as the mesh has it, height second), its radius,
    // and how far up and down it holds (0: every floor under it).
    struct Circle
    {
        Floats centre;
        float radius;
        float reach;
    };

    Closed(dtNavMesh& mesh, const dtNavMeshQuery& query, const std::vector<Circle>& circles, const Floats& from, const Floats& to) : mesh_(mesh)
    {
        if (circles.empty())
        {
            return;
        }

        dtQueryFilter open;
        open.setIncludeFlags(1);
        const float nearby[3]{2, 4, 2};
        dtPolyRef first = 0, last = 0;
        float at[3]{};
        query.findNearestPoly(from.data(), nearby, &open, &first, at);
        query.findNearestPoly(to.data(), nearby, &open, &last, at);
        std::vector<dtPolyRef> found(2048);

        for (const auto& [centre, radius, reach] : circles)
        {
            const float extent[3]{radius, reach > 0.0f ? reach : 40.0f, radius};
            int count = 0;

            if (dtStatusFailed(query.queryPolygons(centre.data(), extent, &open, found.data(), &count, static_cast<int>(found.size()))))
            {
                continue;
            }

            for (int j = 0; j < count; ++j)
            {
                const dtPolyRef ref = found[static_cast<std::size_t>(j)];
                const dtMeshTile* tile = nullptr;
                const dtPoly* poly = nullptr;

                if (ref == first || ref == last || dtStatusFailed(mesh.getTileAndPolyByRef(ref, &tile, &poly)) || !poly || poly->vertCount == 0)
                {
                    continue;
                }

                // By its middle: a long polygon with a corner in the circle may
                // be the only way past it.
                float x = 0, y = 0, z = 0;

                for (int v = 0; v < poly->vertCount; ++v)
                {
                    x += tile->verts[poly->verts[v] * 3];
                    y += tile->verts[poly->verts[v] * 3 + 1];
                    z += tile->verts[poly->verts[v] * 3 + 2];
                }

                x /= static_cast<float>(poly->vertCount);
                y /= static_cast<float>(poly->vertCount);
                z /= static_cast<float>(poly->vertCount);

                if (std::hypot(x - centre[0], z - centre[2]) > radius || (reach > 0.0f && std::abs(y - centre[1]) > reach))
                {
                    continue;
                }

                unsigned short flags = 0;

                if (dtStatusSucceed(mesh.getPolyFlags(ref, &flags)) && !(flags & kClosedFlag) && dtStatusSucceed(mesh.setPolyFlags(ref, static_cast<unsigned short>(flags | kClosedFlag))))
                {
                    closed_.push_back(ref);
                }
            }
        }
    }

    ~Closed()
    {
        for (const dtPolyRef ref : closed_)
        {
            unsigned short flags = 0;

            if (dtStatusSucceed(mesh_.getPolyFlags(ref, &flags)))
            {
                mesh_.setPolyFlags(ref, static_cast<unsigned short>(flags & ~kClosedFlag));
            }
        }
    }

    Closed(const Closed&) = delete;
    Closed& operator=(const Closed&) = delete;

    bool any() const
    {
        return !closed_.empty();
    }

private:
    dtNavMesh& mesh_;
    std::vector<dtPolyRef> closed_;
};

void setCosts(dtQueryFilter& filter, float span, bool roads, bool avoidWater, bool tall = false)
{
    const bool far = span > kRoadNearGoal;
    const float ground = far && roads ? 1 / (span > kLongRoadFrom ? kLongRoadCost : kRoadCost) : 1.0f;
    filter.setAreaCost(0, ground);
    filter.setAreaCost(1, kSteepCost * ground);
    filter.setAreaCost(2, far && roads ? 1.0f : ground);
    filter.setAreaCost(3, avoidWater ? kWaterCost * ground : ground);
    filter.setAreaCost(4, kHillCost * ground);
    filter.setAreaCost(5, tall ? kLowCost * ground : ground);
}

// The road's edge, kRoadMargin wide, is left as open ground: a way along the
// road, pulled tight round its bends, then keeps that far in from its edge
// rather than running along it.
constexpr float kRoadMargin = 1.0f;
void shrinkArea(rcCompactHeightfield& chf, unsigned char area, int cells)
{
    std::vector<int> edge;

    for (int pass = 0; pass < cells; ++pass)
    {
        edge.clear();

        for (int y = 0; y < chf.height; ++y)
        {
            for (int x = 0; x < chf.width; ++x)
            {
                const rcCompactCell& cell = chf.cells[x + y * chf.width];

                for (int i = static_cast<int>(cell.index), end = static_cast<int>(cell.index + cell.count); i < end; ++i)
                {
                    if (chf.areas[i] != area)
                    {
                        continue;
                    }

                    const rcCompactSpan& span = chf.spans[i];

                    for (int dir = 0; dir < 4; ++dir)
                    {
                        const int con = rcGetCon(span, dir);
                        bool outside = con == RC_NOT_CONNECTED;

                        if (!outside)
                        {
                            const int ax = x + rcGetDirOffsetX(dir), ay = y + rcGetDirOffsetY(dir);
                            outside = chf.areas[chf.cells[ax + ay * chf.width].index + con] != area;
                        }

                        if (outside)
                        {
                            edge.push_back(i);
                            break;
                        }
                    }
                }
            }
        }

        for (const int i : edge)
        {
            chf.areas[i] = RC_WALKABLE_AREA;
        }
    }
}

template <class T, void (*Free)(T*)> using Owned = std::unique_ptr<T, decltype(Free)>;
bool readCache(const std::filesystem::path& path, std::vector<unsigned char>& out)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto size = in.tellg();

    if (size <= 0 || size > 16 * 1024 * 1024)
    {
        return false;
    }

    out.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    return bool(in.read(reinterpret_cast<char*>(out.data()), size));
}

} // namespace
struct ClientNavigationProvider::Impl
{
    std::unique_ptr<IClientGeometrySource> source;
    Logger& log;
    ObstacleMemory& obstacles;
    std::filesystem::path cache;
    std::optional<std::uint32_t> map;
    Owned<dtNavMesh, dtFreeNavMesh> mesh{nullptr, dtFreeNavMesh};
    Owned<dtNavMeshQuery, dtFreeNavMeshQuery> query{nullptr, dtFreeNavMeshQuery};
    std::map<Key, CollisionGeometry> geometry;
    std::map<Key, dtTileRef> loaded;
    std::map<Key, std::vector<float>> water; // by map tile (col, row): waterSurface's levels
    std::map<Key, std::vector<std::uint32_t>> areas; // by map tile (col, row): areaAt's areas
    Owned<dtNavMesh, dtFreeNavMesh> coarseMesh{nullptr, dtFreeNavMesh};
    Owned<dtNavMeshQuery, dtFreeNavMeshQuery> coarseQuery{nullptr, dtFreeNavMeshQuery};
    std::map<Key, dtTileRef> coarseLoaded;
    std::map<std::string, std::chrono::steady_clock::time_point> errorSaidAt;
    std::optional<Vec3> roadsSaidFor; // the destination whose way's roads were last told
    // Planning ahead while walking (findLoadedPath): no wider look round.
    bool onlyLoaded = false;
    Impl(std::unique_ptr<IClientGeometrySource> s, Logger& l, ObstacleMemory& o, std::filesystem::path c) : source(std::move(s)), log(l), obstacles(o), cache(std::move(c))
    {
    }

    void reset()
    {
        mesh.reset(dtAllocNavMesh());
        query.reset(dtAllocNavMeshQuery());
        dtNavMeshParams p{};
        p.tileWidth = kTile;
        p.tileHeight = kTile;
        p.maxTiles = 256;
        p.maxPolys = 16384;

        if (!mesh || !query || dtStatusFailed(mesh->init(&p)) || dtStatusFailed(query->init(mesh.get(), 65535)))
        {
            throw std::runtime_error("cannot initialize client navigation mesh");
        }

        loaded.clear();
        geometry.clear();
        coarseMesh.reset();
        coarseQuery.reset();
        coarseLoaded.clear();
    }

    void resetCoarse()
    {
        coarseMesh.reset(dtAllocNavMesh());
        coarseQuery.reset(dtAllocNavMeshQuery());
        dtNavMeshParams p{};
        p.tileWidth = kCoarse.tile;
        p.tileHeight = kCoarse.tile;
        p.maxTiles = 256;
        p.maxPolys = 16384;

        if (!coarseMesh || !coarseQuery || dtStatusFailed(coarseMesh->init(&p)) || dtStatusFailed(coarseQuery->init(coarseMesh.get(), 65535)))
        {
            throw std::runtime_error("cannot initialize the coarse navigation mesh");
        }

        coarseLoaded.clear();
    }

    void cancelled()
    {
        if (obstacles.stopToken().stop_requested())
        {
            throw std::runtime_error("route calculation cancelled");
        }
    }

    std::vector<unsigned char> build(Key key, const Grid& requestedGrid)
    {
        cancelled();
        const float width = requestedGrid.tile;
        const float x0 = key.first * width, y0 = key.second * width;
        const float border = requestedGrid.borderSize * requestedGrid.cs;
        CollisionGeometry all;
        const int r0 = static_cast<int>(std::floor(32 - (x0 + width + border) / kAdt));
        const int r1 = static_cast<int>(std::floor(32 - (x0 - border) / kAdt));
        const int c0 = static_cast<int>(std::floor(32 - (y0 + width + border) / kAdt));
        const int c1 = static_cast<int>(std::floor(32 - (y0 - border) / kAdt));

        for (int row = r0; row <= r1; ++row)
        {
            for (int col = c0; col <= c1; ++col)
            {
                cancelled();
                const Key at{col, row};
                auto tile = geometry.find(at);

                if (tile == geometry.end())
                {
                    if (geometry.size() >= 12)
                    {
                        geometry.clear();
                    }

                    tile = geometry.emplace(at, source->load(*map, col, row, obstacles.stopToken())).first;
                }

                // Only triangles overlapping this navigation tile plus its seam border.
                const auto& g = tile->second;

                for (std::size_t i = 0; i < g.triangles.size(); i += 3)
                {
                    const auto a = g.vertices[g.triangles[i]], b = g.vertices[g.triangles[i + 1]], c = g.vertices[g.triangles[i + 2]];

                    if (std::max({a.x, b.x, c.x}) < x0 - border || std::min({a.x, b.x, c.x}) > x0 + width + border || std::max({a.y, b.y, c.y}) < y0 - border || std::min({a.y, b.y, c.y}) > y0 + width + border)
                    {
                        continue;
                    }

                    const int offset = static_cast<int>(all.vertices.size());
                    all.vertices.insert(all.vertices.end(), {a, b, c});
                    all.triangles.insert(all.triangles.end(), {offset, offset + 1, offset + 2});
                    all.roads.push_back(i / 3 < g.roads.size() ? g.roads[i / 3] : 0);
                }
            }
        }

        if (all.triangles.empty())
        {
            return {};
        }

        const Grid& grid = &requestedGrid == &kFine && hasStackedFloors(all, x0, y0) ? kIndoor : requestedGrid;

        std::vector<float> vertices;
        vertices.reserve(all.vertices.size() * 3);
        float low = all.vertices.front().z, high = low;

        for (auto p : all.vertices)
        {
            vertices.insert(vertices.end(), {p.x, p.z, p.y});
            low = std::min(low, p.z);
            high = std::max(high, p.z);
        }

        if (high - low > 10000)
        {
            throw std::runtime_error("invalid client collision height range");
        }

        rcConfig cfg{};
        cfg.cs = grid.cs;
        cfg.ch = grid.ch;
        cfg.walkableSlopeAngle = kWalkableSlope;
        cfg.walkableHeight = grid.walkableHeight;
        cfg.walkableClimb = grid.walkableClimb;
        cfg.walkableRadius = grid.walkableRadius;
        cfg.tileSize = static_cast<int>(grid.tile / grid.cs);
        cfg.borderSize = grid.borderSize;
        cfg.width = cfg.height = cfg.tileSize + cfg.borderSize * 2;
        cfg.maxEdgeLen = 24;
        cfg.maxSimplificationError = 1.1f;
        cfg.minRegionArea = 0;
        cfg.mergeRegionArea = 16;
        cfg.maxVertsPerPoly = 6;
        cfg.detailSampleDist = 3;
        cfg.detailSampleMaxError = grid.detailError;
        cfg.bmin[0] = x0 - border;
        cfg.bmin[1] = low - 2;
        cfg.bmin[2] = y0 - border;
        cfg.bmax[0] = x0 + width + border;
        cfg.bmax[1] = high + 3;
        cfg.bmax[2] = y0 + width + border;
        rcContext context;
        Owned<rcHeightfield, rcFreeHeightField> hf(rcAllocHeightfield(), rcFreeHeightField);
        Owned<rcCompactHeightfield, rcFreeCompactHeightfield> chf(rcAllocCompactHeightfield(), rcFreeCompactHeightfield);
        Owned<rcContourSet, rcFreeContourSet> contours(rcAllocContourSet(), rcFreeContourSet);
        Owned<rcPolyMesh, rcFreePolyMesh> polys(rcAllocPolyMesh(), rcFreePolyMesh);
        Owned<rcPolyMeshDetail, rcFreePolyMeshDetail> detail(rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);

        if (!hf || !chf || !contours || !polys || !detail)
        {
            throw std::runtime_error("navigation allocation failed");
        }

        std::vector<unsigned char> areas(all.triangles.size() / 3);
        rcMarkWalkableTriangles(&context, cfg.walkableSlopeAngle, vertices.data(), static_cast<int>(all.vertices.size()), all.triangles.data(), static_cast<int>(areas.size()), areas.data());
        const float steepY = std::cos(kSteepSlope / 180.0f * 3.14159265f);
        const float hillY = std::cos(kHillSlope / 180.0f * 3.14159265f);
        const float groundY = std::cos(kGroundSlope / 180.0f * 3.14159265f);

        for (std::size_t t = 0; t < areas.size(); ++t)
        {
            if (areas[t] == RC_NULL_AREA)
            {
                continue;
            }

            if (all.roads[t] == kSwimmingGround)
            {
                areas[t] = kWaterArea;
                continue;
            }

            if (all.roads[t] == kRoadGround)
            {
                areas[t] = kRoadArea;
                continue;
            }

            const float* a = vertices.data() + all.triangles[t * 3] * 3;
            const float* b = vertices.data() + all.triangles[t * 3 + 1] * 3;
            const float* c = vertices.data() + all.triangles[t * 3 + 2] * 3;
            const float e0[3]{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e1[3]{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float nx = e0[1] * e1[2] - e0[2] * e1[1], ny = e0[2] * e1[0] - e0[0] * e1[2], nz = e0[0] * e1[1] - e0[1] * e1[0];
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);

            // Ground steeper than the game lets a character up: no ground.
            if (length > 0 && all.roads[t] != kObjectSurface && std::abs(ny) / length < groundY)
            {
                areas[t] = RC_NULL_AREA;
                continue;
            }

            if (length > 0 && std::abs(ny) / length < steepY)
            {
                areas[t] = kSteepArea;
            }
            else if (length > 0 && all.roads[t] != kObjectSurface && std::abs(ny) / length < hillY)
            {
                areas[t] = kHillArea;
            }
        }

        if (!rcCreateHeightfield(&context, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch) || !rcRasterizeTriangles(&context, vertices.data(), static_cast<int>(all.vertices.size()), all.triangles.data(), areas.data(), static_cast<int>(areas.size()), *hf, cfg.walkableClimb))
        {
            throw std::runtime_error("cannot rasterize client collision geometry");
        }

        cancelled();
        rcFilterLowHangingWalkableObstacles(&context, cfg.walkableClimb, *hf);
        rcFilterLedgeSpans(&context, cfg.walkableHeight, cfg.walkableClimb, *hf);
        rcFilterWalkableLowHeightSpans(&context, cfg.walkableHeight, *hf);

        if (!rcBuildCompactHeightfield(&context, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf) || !rcErodeWalkableArea(&context, cfg.walkableRadius, *chf))
        {
            throw std::runtime_error("cannot build client walking surfaces");
        }

        shrinkArea(*chf, kRoadArea, static_cast<int>(kRoadMargin / grid.cs));

        // Ground with a roof low over it: the space up to the next surface
        // above, in cells of ch. Not on the coarse map, whose two-yard cells
        // it broke into slivers its detail could not be built for.
        const int headroom = static_cast<int>(std::ceil(kHeadroom / grid.ch));

        for (int c = 0; grid.cs < 1.0f && c < chf->width * chf->height; ++c)
        {
            const rcCompactCell& cell = chf->cells[c];

            for (int s = static_cast<int>(cell.index), end = static_cast<int>(cell.index + cell.count); s < end; ++s)
            {
                if (chf->areas[s] != RC_NULL_AREA && chf->areas[s] != kSteepArea && chf->spans[s].h < headroom)
                {
                    chf->areas[s] = kLowArea;
                }
            }
        }

        if (!rcBuildDistanceField(&context, *chf) || !rcBuildRegions(&context, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea) || !rcBuildContours(&context, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours) || !rcBuildPolyMesh(&context, *contours, cfg.maxVertsPerPoly, *polys) || !rcBuildPolyMeshDetail(&context, *polys, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *detail))
        {
            throw std::runtime_error("cannot build client walking surfaces");
        }

        cancelled();

        if (!polys->npolys)
        {
            return {};
        }

        for (int i = 0; i < polys->npolys; ++i)
        {
            polys->flags[i] = 1;
            polys->areas[i] = polys->areas[i] == kSteepArea ? 1 : polys->areas[i] == kRoadArea ? 2 : polys->areas[i] == kWaterArea ? 3 : polys->areas[i] == kHillArea ? 4 : polys->areas[i] == kLowArea ? 5 : 0;
        }

        dtNavMeshCreateParams p{};
        p.verts = polys->verts;
        p.vertCount = polys->nverts;
        p.polys = polys->polys;
        p.polyAreas = polys->areas;
        p.polyFlags = polys->flags;
        p.polyCount = polys->npolys;
        p.nvp = polys->nvp;
        p.detailMeshes = detail->meshes;
        p.detailVerts = detail->verts;
        p.detailVertsCount = detail->nverts;
        p.detailTris = detail->tris;
        p.detailTriCount = detail->ntris;
        p.walkableHeight = grid.walkableHeight * grid.ch;
        p.walkableRadius = std::max(0.5f, grid.walkableRadius * grid.cs);
        p.walkableClimb = grid.walkableClimb * grid.ch;
        p.tileX = key.first;
        p.tileY = key.second;
        p.cs = cfg.cs;
        p.ch = cfg.ch;
        p.buildBvTree = true;
        std::copy(polys->bmin, polys->bmin + 3, p.bmin);
        std::copy(polys->bmax, polys->bmax + 3, p.bmax);
        unsigned char* data = nullptr;
        int size = 0;

        if (!dtCreateNavMeshData(&p, &data, &size))
        {
            throw std::runtime_error("cannot encode client navigation tile");
        }

        std::vector<unsigned char> result(data, data + size);
        dtFree(data);
        return result;
    }

    // On the coarse map, the ground at `p` is the bed of water deep enough to swim.
    bool coarseOnWater(const Vec3& p) const
    {
        if (!coarseMesh || !coarseQuery)
        {
            return false;
        }

        dtQueryFilter filter;
        filter.setIncludeFlags(1);
        const auto at = nav(p);
        const float extent[3]{3, 60, 3};
        dtPolyRef ref = 0;
        float nearest[3]{};
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        return dtStatusSucceed(coarseQuery->findNearestPoly(at.data(), extent, &filter, &ref, nearest)) && ref && dtStatusSucceed(coarseMesh->getTileAndPolyByRef(ref, &tile, &poly)) && poly && poly->getArea() == kWaterArea;
    }

    // An intermediate destination has no reason to leave the road. The real
    // destination still uses its exact position (a building, quest NPC, etc.).
    std::vector<Vec3> coarseRoadGoals(const Vec3& at, const Vec3& from, const Vec3& destination, float radius) const
    {
        // None just now: a full mesh started afresh takes the coarse one with
        // it (reset), and a way that failed after that came here for a road
        // to try - and the bot was gone, on its way over the bridge to Westfall.
        if (!coarseMesh || !coarseQuery)
        {
            return {};
        }

        dtQueryFilter filter;
        filter.setIncludeFlags(1);
        const auto center = nav(at);
        const float extent[3]{radius, 128, radius};
        struct Roads final : dtPolyQuery
        {
            std::vector<dtPolyRef> refs;
            void process(const dtMeshTile*, dtPoly** polys, dtPolyRef* batch, int count) override
            {
                for (int j = 0; j < count; ++j)
                {
                    if (polys[j]->getArea() == kRoadArea)
                    {
                        refs.push_back(batch[j]);
                    }
                }
            }
        } roads;
        // Stream the roads only. A fixed buffer of all nearby polygons fills
        // with ordinary terrain before the road through a large valley fits.
        const auto status = coarseQuery->queryPolygons(center.data(), extent, &filter, &roads);
        if (dtStatusFailed(status))
        {
            return {};
        }

        std::vector<Vec3> candidates;
        for (const auto ref : roads.refs)
        {
            const dtMeshTile* tile = nullptr;
            const dtPoly* poly = nullptr;
            float point[3]{};
            if (dtStatusFailed(coarseMesh->getTileAndPolyByRef(ref, &tile, &poly)) || !poly || poly->getArea() != kRoadArea ||
                dtStatusFailed(coarseQuery->closestPointOnPoly(ref, center.data(), point, nullptr)))
            {
                continue;
            }
            const Vec3 candidate = world(point);
            const float d = distance2D(at, candidate);
            if (d < radius && distance2D(candidate, destination) < distance2D(from, destination) && distance2D(from, candidate) >= 30)
            {
                candidates.push_back(candidate);
            }
        }
        // A road high on a ridge is a worse intermediate goal than the road
        // on the valley floor. Try several distinct places: the nearest road
        // may be disconnected or lie on the other side of the mountains.
        const auto cost = [&](const Vec3& p) { return distance2D(at, p) + 2 * std::abs(p.z - from.z); };
        std::stable_sort(candidates.begin(), candidates.end(), [&](const Vec3& a, const Vec3& b) { return cost(a) < cost(b); });
        std::vector<Vec3> goals;
        for (const auto& candidate : candidates)
        {
            if (std::none_of(goals.begin(), goals.end(), [&](const Vec3& p) { return distance2D(p, candidate) < 64; }))
            {
                goals.push_back(candidate);
                if (goals.size() == 12)
                {
                    break;
                }
            }
        }
        return goals;
    }

    std::optional<Vec3> coarseRoadGoal(const Vec3& at, const Vec3& from, const Vec3& destination, float radius) const
    {
        const auto goals = coarseRoadGoals(at, from, destination, radius);
        return goals.empty() ? std::nullopt : std::optional<Vec3>{goals.front()};
    }

    void tile(Key key)
    {
        addTile(key, kFine, *mesh, loaded);
    }

    void coarseTile(Key key)
    {
        addTile(key, kCoarse, *coarseMesh, coarseLoaded);
    }

    void addTile(Key key, const Grid& grid, dtNavMesh& into, std::map<Key, dtTileRef>& tiles)
    {
        if (tiles.contains(key))
        {
            return;
        }

        cancelled();
        const auto path = cache / std::to_string(*map) / grid.folder / (std::to_string(key.first) + "_" + std::to_string(key.second) + ".navtile");
        std::vector<unsigned char> data;
        bool cached = !cache.empty() && readCache(path, data);
        const auto add = [&]
        {
            auto* raw = static_cast<unsigned char*>(dtAlloc(data.size(), DT_ALLOC_PERM));

            if (!raw)
            {
                throw std::runtime_error("navigation tile allocation failed");
            }

            std::copy(data.begin(), data.end(), raw);
            dtTileRef ref = 0;

            if (dtStatusFailed(into.addTile(raw, static_cast<int>(data.size()), DT_TILE_FREE_DATA, 0, &ref)))
            {
                dtFree(raw);
                return false;
            }

            tiles.emplace(key, ref);
            return true;
        };

        if (cached && add())
        {
            return;
        }

        data = build(key, grid);

        if (data.empty())
        {
            tiles.emplace(key, 0);
            return;
        }

        if (!add())
        {
            throw std::runtime_error("cannot load client navigation tile");
        }

        if (!cache.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            const auto temp = path.string() + ".tmp";
            std::ofstream out(temp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(data.data()), data.size());
            out.close();

            if (out)
            {
                std::filesystem::rename(temp, path, ec);
            }
        }
    }

    // The way from `from` towards `to` on the coarse mesh, as its corner
    // points from `from` on; empty when there is none. The window round the
    // two widens until a way is found or it holds too many tiles; past what
    // one window holds, the way towards a point on the straight line.
    std::vector<Vec3> coarseWay(const Vec3& from, const Vec3& to, const ObstacleMemory::Calculation& calculation, bool roads, bool avoidWater)
    {
        if (!coarseMesh)
        {
            resetCoarse();
        }

        Vec3 goal = to;
        const int goalColumns = static_cast<int>(std::floor(std::max(from.x, to.x) / kCoarse.tile)) - static_cast<int>(std::floor(std::min(from.x, to.x) / kCoarse.tile)) + 5;
        const int goalRows = static_cast<int>(std::floor(std::max(from.y, to.y) / kCoarse.tile)) - static_cast<int>(std::floor(std::min(from.y, to.y) / kCoarse.tile)) + 5;
        const bool clipped = distance2D(from, to) > kCoarseReach && goalColumns * goalRows > kMaxWindow;

        if (clipped)
        {
            goal = from + (to - from) * (kCoarseReach / distance2D(from, to));
        }

        for (const int margin : {2, 4, 6})
        {
            cancelled();
            const int x0 = static_cast<int>(std::floor(std::min(from.x, goal.x) / kCoarse.tile)) - margin;
            const int x1 = static_cast<int>(std::floor(std::max(from.x, goal.x) / kCoarse.tile)) + margin;
            const int y0 = static_cast<int>(std::floor(std::min(from.y, goal.y) / kCoarse.tile)) - margin;
            const int y1 = static_cast<int>(std::floor(std::max(from.y, goal.y) / kCoarse.tile)) + margin;
            const int count = (x1 - x0 + 1) * (y1 - y0 + 1);

            if (count > kMaxWindow)
            {
                break;
            }

            int missing = 0;

            for (int x = x0; x <= x1; ++x)
            {
                for (int y = y0; y <= y1; ++y)
                {
                    missing += !coarseLoaded.contains({x, y});
                }
            }

            if (coarseLoaded.size() + missing > 240)
            {
                resetCoarse();
            }

            int done = 0;

            for (int x = x0; x <= x1; ++x)
            {
                for (int y = y0; y <= y1; ++y)
                {
                    coarseTile({x, y});
                    calculation.report(++done * 100 / count);
                }
            }

            dtQueryFilter filter;
            filter.setIncludeFlags(1);
            setCosts(filter, distance2D(from, to), roads, avoidWater);
            const auto roadGoal = roads && clipped ? coarseRoadGoal(goal, from, to, kCoarse.tile) : std::nullopt;
            // A nearby road may belong to a disconnected surface. Retain the
            // original endpoint as a fallback rather than losing a valid way.
            const std::vector<Vec3> goals = roadGoal ? std::vector<Vec3>{*roadGoal, goal} : std::vector<Vec3>{goal};
            for (const auto& endpoint : goals)
            {
                const auto a = nav(from), b = nav(endpoint);
                float start[3]{}, end[3]{};
                dtPolyRef first = 0, last = 0;
                const float nearExtent[3]{4, 8, 4}, farExtent[3]{8, 128, 8};

                if (dtStatusFailed(coarseQuery->findNearestPoly(a.data(), nearExtent, &filter, &first, start)) || !first)
                {
                    coarseQuery->findNearestPoly(a.data(), farExtent, &filter, &first, start);
                }

                if (dtStatusFailed(coarseQuery->findNearestPoly(b.data(), nearExtent, &filter, &last, end)) || !last)
                {
                    coarseQuery->findNearestPoly(b.data(), farExtent, &filter, &last, end);
                }

                if (!first || !last)
                {
                    log.debug(logcat::Move, "coarse route: no ground at {} (start {}) or {} (end {})", from, first, endpoint, last);
                    continue;
                }

                constexpr int limit = 4096;
                std::vector<dtPolyRef> corridor(limit);
                int n = 0;
                float reached[3]{};
                // A corridor from `first` to the endpoint, or to within the
                // tolerance of it (`reached`).
                enum class Found
                {
                    None,
                    Near,
                    Whole
                };
                const auto search = [&]
                {
                    const auto status = coarseQuery->findPath(first, last, start, end, &filter, corridor.data(), &n, limit);

                    if (dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL) || dtStatusDetail(status, DT_OUT_OF_NODES) || n == 0)
                    {
                        n = 0;
                        return Found::None;
                    }

                    if (!dtStatusDetail(status, DT_PARTIAL_RESULT) && corridor[n - 1] == last)
                    {
                        return Found::Whole;
                    }

                    const bool near = dtStatusSucceed(coarseQuery->closestPointOnPoly(corridor[n - 1], end, reached, nullptr)) && distance2D(world(reached), endpoint) <= kCoarseGoalTolerance;
                    return near ? Found::Near : Found::None;
                };
                auto found = search();

                // None from where the character stands: inside a house the
                // floor is an island at this resolution, its doorway closed.
                // From the nearest ground off that island then - the fine mesh
                // has the way out through the door. From the inn at Lakeshire
                // there was no coarse way to Stormwind, and the leg on the air
                // line instead went north into the hills.
                if (found == Found::None && n > 0)
                {
                    log.debug(logcat::Move, "coarse route: margin {}, {} polygons, from {} to {}, reached {}", margin, n, from, endpoint, world(reached));
                    constexpr int kIsland = 256, kStarts = 8;
                    constexpr float kOutside = 48;
                    std::array<dtPolyRef, kIsland> island{};
                    int islandSize = 0;
                    const auto around = coarseQuery->findPolysAroundCircle(first, start, kOutside * 2, &filter, island.data(), nullptr, nullptr, &islandSize, kIsland);
                    std::array<dtPolyRef, 256> nearby{};
                    int nearbySize = 0;
                    const float outside[3]{kOutside, 16, kOutside};

                    if (dtStatusSucceed(around) && !dtStatusDetail(around, DT_BUFFER_TOO_SMALL) && dtStatusSucceed(coarseQuery->queryPolygons(a.data(), outside, &filter, nearby.data(), &nearbySize, static_cast<int>(nearby.size()))))
                    {
                        struct Start
                        {
                            float off;
                            dtPolyRef ref;
                            Floats point;
                        };
                        std::vector<Start> starts;

                        for (int j = 0; j < nearbySize; ++j)
                        {
                            Floats p{};

                            if (std::find(island.begin(), island.begin() + islandSize, nearby[j]) == island.begin() + islandSize && dtStatusSucceed(coarseQuery->closestPointOnPoly(nearby[j], a.data(), p.data(), nullptr)))
                            {
                                starts.push_back({std::hypot(p[0] - a[0], p[2] - a[2]) + std::abs(p[1] - a[1]), nearby[j], p});
                            }
                        }

                        std::sort(starts.begin(), starts.end(), [](const Start& l, const Start& r) { return l.off < r.off; });

                        for (std::size_t j = 0; j < starts.size() && j < kStarts && found == Found::None; ++j)
                        {
                            cancelled();
                            first = starts[j].ref;
                            std::copy(starts[j].point.begin(), starts[j].point.end(), start);
                            found = search();
                        }
                    }
                }

                if (found == Found::None)
                {
                    continue;
                }

                if (found == Found::Near)
                {
                    // Doorways and stacked floors can disappear at this
                    // resolution. A route to the destination's immediate
                    // surroundings still guides the fine mesh through the
                    // correct valley and city gate. Only the fine query is
                    // allowed to return a walking route, and it must finish.
                    std::copy(std::begin(reached), std::end(reached), end);
                }

                std::vector<float> points(limit * 3);
                int size = 0;
                const auto straight = coarseQuery->findStraightPath(start, end, corridor.data(), n, points.data(), nullptr, nullptr, &size, limit);

                if (dtStatusFailed(straight) || size < 2)
                {
                    continue;
                }

                std::vector<Vec3> way{from};

                for (int j = 1; j < size; ++j)
                {
                    way.push_back(world(points.data() + j * 3));
                }

                return way;
            }
        }

        return {};
    }
};
ClientNavigationProvider::ClientNavigationProvider(std::unique_ptr<IClientGeometrySource> source, Logger& log, ObstacleMemory& obstacles, std::filesystem::path cache) : impl_(std::make_unique<Impl>(std::move(source), log, obstacles, std::move(cache)))
{
}

ClientNavigationProvider::~ClientNavigationProvider() = default;
void ClientNavigationProvider::setMap(std::optional<std::uint32_t> map)
{
    if (map == impl_->map)
    {
        return;
    }

    impl_->map = map;
    impl_->errorSaidAt.clear();
    impl_->water.clear();
    impl_->areas.clear();
    impl_->reset();
}

std::optional<float> ClientNavigationProvider::waterSurface(const Vec3& at) const
{
    auto& i = *impl_;

    if (!i.map || !i.source)
    {
        return std::nullopt;
    }

    const int col = static_cast<int>(std::floor(32 - at.y / kAdt));
    const int row = static_cast<int>(std::floor(32 - at.x / kAdt));
    auto found = i.water.find({col, row});

    if (found == i.water.end())
    {
        found = i.water.emplace(Key{col, row}, i.source->waterLevels(*i.map, col, row)).first;
    }

    return waterLevelAt(found->second, col, row, at.x, at.y);
}

std::optional<std::uint32_t> ClientNavigationProvider::areaAt(const Vec3& at) const
{
    auto& i = *impl_;

    if (!i.map || !i.source)
    {
        return std::nullopt;
    }

    const int col = static_cast<int>(std::floor(32 - at.y / kAdt));
    const int row = static_cast<int>(std::floor(32 - at.x / kAdt));
    auto found = i.areas.find({col, row});

    if (found == i.areas.end())
    {
        found = i.areas.emplace(Key{col, row}, i.source->areaIds(*i.map, col, row)).first;
    }

    return areaIdAt(found->second, col, row, at.x, at.y);
}

bool ClientNavigationProvider::canPlanWithoutLoading(const Vec3& from, const Vec3& to) const
{
    const auto& i = *impl_;

    if (!i.map || !i.mesh || i.loaded.size() > 240 || windowTiles(from, to) > kMaxWindow)
    {
        return false;
    }

    const int x0 = static_cast<int>(std::floor(std::min(from.x, to.x) / kTile)) - kMargin;
    const int x1 = static_cast<int>(std::floor(std::max(from.x, to.x) / kTile)) + kMargin;
    const int y0 = static_cast<int>(std::floor(std::min(from.y, to.y) / kTile)) - kMargin;
    const int y1 = static_cast<int>(std::floor(std::max(from.y, to.y) / kTile)) + kMargin;

    for (int x = x0; x <= x1; ++x)
    {
        for (int y = y0; y <= y1; ++y)
        {
            if (!i.loaded.contains({x, y}))
            {
                return false;
            }
        }
    }

    return true;
}

std::optional<Path> ClientNavigationProvider::findLoadedPath(const Vec3& from, const Vec3& to)
{
    struct OnlyLoaded
    {
        bool& flag;
        ~OnlyLoaded()
        {
            flag = false;
        }
    } only{impl_->onlyLoaded};
    only.flag = true;
    return findPath(from, to);
}

std::optional<Path> ClientNavigationProvider::findPath(const Vec3& from, const Vec3& to)
{
    auto& i = *impl_;
    const ObstacleMemory::Calculation calculation(&i.obstacles);

    try
    {
        if (!i.map)
        {
            throw std::runtime_error("waiting for the client's map ID");
        }

        if (!i.mesh)
        {
            i.reset();
        }

        // One route from `from` to `goal` through `tiles`; throws when there
        // is none.
        const auto routeThrough = [&](const Vec3& goal, bool continues, const std::set<Key>& tiles)
        {
            const int count = static_cast<int>(tiles.size());

            if (count > 240)
            {
                throw std::runtime_error("destination too far for a local client-map route");
            }

            const auto missing = std::count_if(tiles.begin(), tiles.end(), [&](const Key& key) { return !i.loaded.contains(key); });

            if (i.loaded.size() + static_cast<std::size_t>(missing) > 240)
            {
                i.reset();
            }

            int done = 0;

            for (const Key& key : tiles)
            {
                i.tile(key);
                calculation.report(++done * 85 / count);
            }

            const auto a = nav(from), b = nav(goal);
            float start[3]{}, end[3]{};
            dtPolyRef first = 0, last = 0;
            dtQueryFilter filter;
            filter.setIncludeFlags(1);
            setCosts(filter, distance2D(from, to), useRoads_, avoidWater_, tall_);
            // What the caller asked this way to keep out of (setAvoiding).
            std::vector<Closed::Circle> circles;

            for (const auto& [centre, radius, reach] : avoiding_)
            {
                circles.push_back({nav(centre), radius, reach});
            }

            // For the searches only, and opened again before the way is
            // smoothed: canWalkStraight loads tiles and may start the mesh
            // afresh, with the polygons closed here gone from under them.
            std::optional<Closed> closed(std::in_place, *i.mesh, *i.query, circles, a, b);

            if (closed->any())
            {
                filter.setExcludeFlags(kClosedFlag);
            }

            const float extent[3]{2, 4, 2};

            if (dtStatusFailed(i.query->findNearestPoly(a.data(), extent, &filter, &first, start)) || !first)
            {
                throw std::runtime_error("route start is not on a walkable client surface");
            }

            // At the rim of a pit or slope, findNearestPoly can prefer a
            // nearby surface below the character even though the character's
            // own floor is also in the mesh. Try nearby polygons at the
            // character's actual height before giving up on this route.
            if (std::abs(start[1] - a[1]) > 3)
            {
                const float startExtent[3]{1.5f, 3.0f, 1.5f};
                std::array<dtPolyRef, 128> candidates{};
                int found = 0;
                const auto candidateStatus = i.query->queryPolygons(a.data(), startExtent, &filter, candidates.data(), &found, static_cast<int>(candidates.size()));

                if (dtStatusSucceed(candidateStatus) && !dtStatusDetail(candidateStatus, DT_BUFFER_TOO_SMALL))
                {
                    struct Start
                    {
                        dtPolyRef ref;
                        Floats point;
                        float score;
                    };
                    std::vector<Start> starts;

                    for (int j = 0; j < found; ++j)
                    {
                        Floats point{};

                        if (dtStatusFailed(i.query->closestPointOnPoly(candidates[j], a.data(), point.data(), nullptr)))
                        {
                            continue;
                        }

                        const float xy = std::hypot(point[0] - a[0], point[2] - a[2]);
                        const float dz = std::abs(point[1] - a[1]);

                        if (xy <= 1.5f && dz <= 3.0f)
                        {
                            starts.push_back({candidates[j], point, xy + dz});
                        }
                    }

                    std::sort(starts.begin(), starts.end(), [](const Start& left, const Start& right) { return left.score < right.score; });

                    if (!starts.empty())
                    {
                        first = starts.front().ref;
                        std::copy(starts.front().point.begin(), starts.front().point.end(), start);
                    }
                }
            }

            auto endStatus = i.query->findNearestPoly(b.data(), extent, &filter, &last, end);
            // The goal's own floor, found at its height: a mob's place, not
            // Questie's rough one.
            const std::optional<float> goalFloor = dtStatusSucceed(endStatus) && last ? std::optional<float>(end[1]) : std::nullopt;

            if (dtStatusFailed(endStatus) || !last)
            {
                const float approximateHeightExtent[3]{2, 128, 2};
                endStatus = i.query->findNearestPoly(b.data(), approximateHeightExtent, &filter, &last, end);
            }

            if (dtStatusFailed(endStatus) || !last)
            {
                const float wideExtent[3]{5, 128, 5};
                endStatus = i.query->findNearestPoly(b.data(), wideExtent, &filter, &last, end);
            }

            // Questie has no heights: a place up a mountain is asked for at the
            // character's own, far below. Dwarven Digging's lies on a ridge 136
            // yards over the plain the bot stood on, and no ground was found.
            if (dtStatusFailed(endStatus) || !last)
            {
                const float tallExtent[3]{5, 600, 5};
                endStatus = i.query->findNearestPoly(b.data(), tallExtent, &filter, &last, end);
            }

            if (dtStatusFailed(endStatus) || !last)
            {
                throw std::runtime_error("route destination is not on a walkable client surface");
            }

            if (std::abs(start[1] - a[1]) > 3)
            {
                throw std::runtime_error("route starts on another floor");
            }

            // Profile/Questie Z can be approximate; use the actual client floor at
            // the destination, never synthesize a direct route through geometry.
            constexpr int limit = 4096;
            std::vector<dtPolyRef> corridor(limit);
            int n = 0;
            auto status = i.query->findPath(first, last, start, end, &filter, corridor.data(), &n, limit);
            const auto complete = [&] { return dtStatusSucceed(status) && !dtStatusDetail(status, DT_PARTIAL_RESULT) && !dtStatusDetail(status, DT_BUFFER_TOO_SMALL) && !dtStatusDetail(status, DT_OUT_OF_NODES) && n > 0 && corridor[n - 1] == last; };

            // Quest/map coordinates often carry terrain height below a building's
            // floor. Consider other floors at the same XY, but accept one only if
            // a complete walking corridor reaches it. Never follow a partial path.
            if (!complete())
            {
                const float floorsExtent[3]{5, 128, 5};
                std::array<dtPolyRef, 256> candidates{};
                int found = 0;
                const auto candidatesStatus = i.query->queryPolygons(b.data(), floorsExtent, &filter, candidates.data(), &found, candidates.size());
                struct Floor
                {
                    dtPolyRef ref;
                    Floats point;
                    float score;
                };
                std::vector<Floor> floors;

                if (dtStatusSucceed(candidatesStatus) && !dtStatusDetail(candidatesStatus, DT_BUFFER_TOO_SMALL))
                {
                    for (int j = 0; j < found; ++j)
                    {
                        Floats p{};

                        if (dtStatusFailed(i.query->closestPointOnPoly(candidates[j], b.data(), p.data(), nullptr)))
                        {
                            continue;
                        }

                        const float xy = std::hypot(p[0] - b[0], p[2] - b[2]);

                        // Kept out of what is avoided, the goal's floor is cut
                        // off by it - not the place to look for another: by
                        // Echo Ridge Mine the way round the kobolds' pack ended
                        // on the hill 59 yd over the mob in the mine. No way,
                        // then, and the caller goes past them.
                        if (closed->any() && goalFloor && std::abs(p[1] - *goalFloor) > kGoalFloorReach)
                        {
                            continue;
                        }

                        if (xy <= 5)
                        {
                            floors.push_back({candidates[j], p, xy * 100 + std::abs(p[1] - b[1])});
                        }
                    }

                    std::sort(floors.begin(), floors.end(), [](const auto& left, const auto& right) { return left.score < right.score; });

                    for (const auto& floor : floors)
                    {
                        i.cancelled();
                        last = floor.ref;
                        std::copy(floor.point.begin(), floor.point.end(), end);
                        status = i.query->findPath(first, last, start, end, &filter, corridor.data(), &n, limit);

                        if (complete())
                        {
                            break;
                        }
                    }

                    // Still none: the start may be the wrong one. Standing at
                    // the edge of a slope, the polygon under the character is
                    // taken - the ground 2 yd below, cut off from the rest -
                    // rather than the one it stands on beside it: a ghost at
                    // the mouth of a cave in the Valley of Trials never got in
                    // to its body, though from the body out there was a way.
                    if (!complete() && !floors.empty())
                    {
                        const float startExtent[3]{1.5f, 3, 1.5f};
                        std::array<dtPolyRef, 64> starts{};
                        int startsFound = 0;

                        if (dtStatusSucceed(i.query->queryPolygons(a.data(), startExtent, &filter, starts.data(), &startsFound, starts.size())))
                        {
                            std::vector<std::pair<float, Floor>> nearby;

                            for (int j = 0; j < startsFound; ++j)
                            {
                                Floats p{};

                                if (starts[j] == first || dtStatusFailed(i.query->closestPointOnPoly(starts[j], a.data(), p.data(), nullptr)))
                                {
                                    continue;
                                }

                                const float xy = std::hypot(p[0] - a[0], p[2] - a[2]);
                                const float dz = std::abs(p[1] - a[1]);

                                if (xy <= 1.5f && dz <= 3)
                                {
                                    nearby.push_back({xy + dz, Floor{starts[j], p, 0}});
                                }
                            }

                            std::sort(nearby.begin(), nearby.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
                            // As the first start left it: a nearby start that is cut off
                            // too ends farther off, and must not stand in for it.
                            const dtPolyRef keptFirst = first, keptLast = last;
                            Floats keptStart{start[0], start[1], start[2]}, keptEnd{end[0], end[1], end[2]};
                            const auto keptStatus = status;
                            const int keptN = n;
                            const std::vector<dtPolyRef> keptCorridor(corridor.begin(), corridor.begin() + n);

                            for (const auto& [score, candidate] : nearby)
                            {
                                i.cancelled();
                                first = candidate.ref;
                                std::copy(candidate.point.begin(), candidate.point.end(), start);
                                last = floors.front().ref;
                                std::copy(floors.front().point.begin(), floors.front().point.end(), end);
                                status = i.query->findPath(first, last, start, end, &filter, corridor.data(), &n, limit);

                                if (complete())
                                {
                                    break;
                                }
                            }

                            if (!complete())
                            {
                                first = keptFirst;
                                last = keptLast;
                                std::copy(keptStart.begin(), keptStart.end(), start);
                                std::copy(keptEnd.begin(), keptEnd.end(), end);
                                status = keptStatus;
                                n = keptN;
                                std::copy(keptCorridor.begin(), keptCorridor.end(), corridor.begin());
                            }
                        }
                    }
                }
            }

            if (dtStatusFailed(status) || dtStatusDetail(status, DT_PARTIAL_RESULT) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL) || dtStatusDetail(status, DT_OUT_OF_NODES) || n == 0 || corridor[n - 1] != last)
            {
                float reached[3]{};

                if (n)
                {
                    i.query->closestPointOnPoly(corridor[n - 1], end, reached, nullptr);
                }

                const float reachedXy = std::hypot(reached[0] - b[0], reached[2] - b[2]);
                const float reachedZ = std::abs(reached[1] - b[1]);

                // Not on the floor kept out of above either (kGoalFloorReach).
                const bool offGoalFloor = closed && closed->any() && goalFloor && std::abs(reached[1] - *goalFloor) > kGoalFloorReach;

                if (n > 0 && reachedXy <= 5.0f && reachedZ <= 8.0f && !offGoalFloor)
                {
                    std::copy(reached, reached + 3, end);
                }
                else
                {
                    i.log.debug(logcat::Move,
                                "partial client route: {} polygons, status {}, start ({}, {}, {}), end ({}, {}, {}), reached "
                                "({}, {}, {})",
                                n, status, start[0], start[2], start[1], end[0], end[2], end[1], reached[0], reached[2], reached[1]);
                    throw NoRouteWithin();
                }
            }

            // Ended on another floor than the goal's: the map has no way up
            // (or down) there - a stair left out, or a floor not reached.
            if (const float off = end[1] - b[1]; !continues && std::abs(off) > 3)
            {
                const std::string said = "another floor";
                const auto when = std::chrono::steady_clock::now();

                if (const auto it = i.errorSaidAt.find(said); it == i.errorSaidAt.end() || when - it->second >= std::chrono::seconds(10))
                {
                    // Debug: many goals (Questie's spots) have only a rough height.
                    i.log.debug(logcat::Move, "client pathfinding: the way to {} ends {:.0f} yd {} it - no way {} found in the map", goal, std::abs(off), off < 0 ? "below" : "above", off < 0 ? "up" : "down");
                    i.errorSaidAt[said] = when;
                }
            }

            // Once to a far destination: how much of the way is road, and how
            // much road the map round it has. None at all: the ground's road
            // textures were not recognised there.
            if (distance2D(from, to) > kRoadNearGoal && (!i.roadsSaidFor || distance(*i.roadsSaidFor, to) > 1))
            {
                i.roadsSaidFor = to;
                const auto areaOf = [&](dtPolyRef ref)
                {
                    const dtMeshTile* tile = nullptr;
                    const dtPoly* poly = nullptr;
                    return dtStatusSucceed(i.mesh->getTileAndPolyByRef(ref, &tile, &poly)) && poly ? poly->getArea() : 0;
                };
                int onRoad = 0;

                for (int j = 0; j < n; ++j)
                {
                    onRoad += areaOf(corridor[j]) == 2;
                }

                int roadPolygons = 0;

                for (const auto& [key, ref] : i.loaded)
                {
                    const dtMeshTile* tile = ref ? i.mesh->getTileByRef(ref) : nullptr;

                    if (!tile || !tile->header)
                    {
                        continue;
                    }

                    for (int k = 0; k < tile->header->polyCount; ++k)
                    {
                        roadPolygons += tile->polys[k].getArea() == 2;
                    }
                }

                i.log.info(logcat::Move,
                           "client pathfinding: the way towards {} crosses {} polygons, {} of them road; the map "
                           "round it has {} road polygons",
                           goal, n, onRoad, roadPolygons);

                if (roadPolygons == 0)
                {
                    // Which ground textures the tile the bot stands on has: the
                    // road among them is not on the list.
                    const int col = static_cast<int>(std::floor(32 - from.y / kAdt));
                    const int row = static_cast<int>(std::floor(32 - from.x / kAdt));
                    const auto textures = i.source->groundTextures(*i.map, col, row);
                    std::string list;

                    for (const auto id : textures.value_or(std::vector<std::uint32_t>{}))
                    {
                        list += (list.empty() ? "" : ", ") + std::to_string(id);
                    }

                    i.log.info(logcat::Move, "client pathfinding: map tile {}_{} ground textures: {}", col, row, textures ? (list.empty() ? std::string("none listed by id") : list) : std::string("its texture file could not be read"));
                }
            }

            std::vector<float> points(limit * 3);
            std::vector<dtPolyRef> refs(limit);
            int size = 0;
            closed.reset();
            const auto straight = i.query->findStraightPath(start, end, corridor.data(), n, points.data(), nullptr, refs.data(), &size, limit);

            if (dtStatusFailed(straight) || dtStatusDetail(straight, DT_BUFFER_TOO_SMALL) || size < 2)
            {
                throw std::runtime_error("cannot follow client-map route");
            }

            Path path;
            path.continues = continues;

            for (int j = 1; j < size; ++j)
            {
                // A corner's height is interpolated along a polygon edge, which
                // over a hilltop can lie yards under the ground; the bot then
                // never stood on the corner's level and circled round it. The
                // detail mesh has the height of the ground there.
                float* p = points.data() + j * 3;
                // A corner lies on a polygon's edge, where getPolyHeight fails
                // as often as not; the closest point on the polygon has the
                // detail mesh's height there too.
                float closest[3]{};

                if (refs[j] && dtStatusSucceed(i.query->closestPointOnPoly(refs[j], p, closest, nullptr)) && std::hypot(closest[0] - p[0], closest[2] - p[2]) < 0.5f)
                {
                    p[1] = closest[1];
                }

                path.points.push_back(world(p));
            }

            path.points = smoothSmallSteps(from, path.points, [this](const Vec3& a, const Vec3& b) { return canWalkStraight(a, b); });
            calculation.report(100);
            return path;
        };
        // The same through the window of tiles holding both ends.
        const auto route = [&](const Vec3& goal, bool continues)
        {
            if (windowTiles(from, goal) > kMaxWindow)
            {
                throw std::runtime_error("destination too far for a local client-map route");
            }

            // No way within the window: a wider one, while the mesh holds it.
            // The way into Chief Sharptusk's camp goes round by the south, a
            // tile past the window from Camp Narache, and none was found.
            constexpr int kWiderBy = 2;
            constexpr std::size_t kMostTiles = 240;

            for (int margin = kMargin;; ++margin)
            {
                const int x0 = static_cast<int>(std::floor(std::min(from.x, goal.x) / kTile)) - margin;
                const int x1 = static_cast<int>(std::floor(std::max(from.x, goal.x) / kTile)) + margin;
                const int y0 = static_cast<int>(std::floor(std::min(from.y, goal.y) / kTile)) - margin;
                const int y1 = static_cast<int>(std::floor(std::max(from.y, goal.y) / kTile)) + margin;
                std::set<Key> tiles;

                for (int x = x0; x <= x1; ++x)
                {
                    for (int y = y0; y <= y1; ++y)
                    {
                        tiles.insert({x, y});
                    }
                }

                const int wider = (x1 - x0 + 3) * (y1 - y0 + 3);
                const bool last = i.onlyLoaded || margin >= kMargin + kWiderBy || static_cast<std::size_t>(wider) > kMostTiles;

                try
                {
                    return routeThrough(goal, continues, tiles);
                }
                catch (const NoRouteWithin&)
                {
                    i.cancelled();

                    if (last)
                    {
                        throw;
                    }
                }
            }
        };

        // The whole way at once while one window of tiles holds both ends. A
        // leg cut off on the straight line ended wherever that was - up on the
        // ridge round Northshire Valley, in a lake - with no route to it, and
        // the destination was given up although a way there existed.
        // With none there, the coarse way, which sees farther round: from Camp
        // Narache the way down to Bloodhoof Village goes west first, out of
        // any window round the two ends.
        if (windowTiles(from, to) <= kMaxWindow)
        {
            try
            {
                return route(to, false);
            }
            catch (const NoRouteWithin&)
            {
                i.cancelled();

                if (i.onlyLoaded || distance2D(from, to) < kCoarse.tile)
                {
                    throw;
                }
            }
        }

        // Farther: along the coarse way, through the tiles beside it rather
        // than every tile of the square both ends lie in - the whole way where
        // those fit the mesh, and else a leg as far along it as they do,
        // planned again at its end. A nearer point when that fails.
        const auto way = i.coarseWay(from, to, calculation, useRoads_, avoidWater_);

        if (way.size() >= 2)
        {
            std::vector<Vec3> along;
            std::set<Key> beside;
            addAround(beside, from, kMargin);
            // The coarse way ends at the destination, not on the line to one
            // past its reach.
            bool whole = distance2D(way.back(), to) <= kCoarseGoalTolerance;

            for (std::size_t j = 1; j < way.size(); ++j)
            {
                const float length = distance2D(way[j - 1], way[j]);
                const int steps = std::max(1, static_cast<int>(length / 16));
                bool fits = true;

                for (int k = 1; k <= steps && fits; ++k)
                {
                    const Vec3 p = way[j - 1] + (way[j] - way[j - 1]) * (static_cast<float>(k) / steps);
                    // With the wider margin a way's end has round it.
                    auto ending = beside;
                    addAround(ending, p, kMargin);
                    fits = ending.size() <= kMaxCorridor;

                    if (fits)
                    {
                        addAround(beside, p, kCorridorMargin);
                        along.push_back(p);
                    }
                }

                if (!fits)
                {
                    whole = false;
                    break;
                }
            }

            const auto corridor = [&](std::size_t count, const Vec3& goal)
            {
                std::set<Key> tiles;
                addAround(tiles, from, kMargin);

                for (std::size_t j = 0; j < count; ++j)
                {
                    addAround(tiles, along[j], kCorridorMargin);
                }

                addAround(tiles, goal, kMargin);
                return tiles;
            };

            for (const float share : {1.0f, 0.75f, 0.5f, 0.25f})
            {
                if (along.empty())
                {
                    break;
                }

                auto at = static_cast<std::size_t>(std::ceil(share * along.size())) - 1;

                // Not a leg ending in water: the coarse way's points are a few
                // yards off, and one in a lake took the fine way straight out
                // into it, to turn back to the shore for the next leg.
                while (at > 0 && i.coarseOnWater(along[at]))
                {
                    --at;
                }

                if (distance2D(from, along[at]) < 30)
                {
                    break;
                }

                const bool all = whole && share == 1.0f;
                const auto roadGoal = !all && useRoads_ ? i.coarseRoadGoal(along[at], from, to, kTile) : std::nullopt;
                const Vec3 original = all ? to : along[at];
                const std::vector<Vec3> goals = roadGoal ? std::vector<Vec3>{*roadGoal, original} : std::vector<Vec3>{original};
                for (const auto& goal : goals)
                {
                    try
                    {
                        return routeThrough(goal, !all, corridor(at + 1, goal));
                    }
                    catch (const std::exception&)
                    {
                        i.cancelled();
                    }
                }
            }
        }

        // No coarse way, or none of it could be walked: try nearby roads,
        // then points on the air line, a little to either side, and shorter.
        const float separation = distance2D(from, to);
        const float ax = (to.x - from.x) / separation, ay = (to.y - from.y) / separation;
        std::string firstError;

        for (const float length : {kLeg, kLeg * 0.75f, kLeg * 0.5f})
        {
            for (const float turn : {0.0f, 0.35f, -0.35f, 0.7f, -0.7f})
            {
                const float c = std::cos(turn), s = std::sin(turn);
                const Vec3 goal{from.x + (ax * c - ay * s) * length, from.y + (ax * s + ay * c) * length, from.z + (to.z - from.z) * (length / separation)};

                // The coarse search can fail when its projected endpoint is
                // on inaccessible terrain. Its loaded roads still tell us
                // where to continue: try a reachable road before walking
                // towards another arbitrary point on the air line.
                auto goals = useRoads_ ? i.coarseRoadGoals(goal, from, to, kLeg * 2) : std::vector<Vec3>{};
                goals.push_back(goal);
                for (const auto& endpoint : goals)
                {
                    try
                    {
                        return route(endpoint, true);
                    }
                    catch (const std::exception& error)
                    {
                        i.cancelled();

                        if (firstError.empty())
                        {
                            firstError = error.what();
                        }
                    }
                }
            }
        }

        throw std::runtime_error(firstError);
    }
    catch (const std::exception& error)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto said = i.errorSaidAt.find(error.what());

        if (!i.obstacles.stopToken().stop_requested() && (said == i.errorSaidAt.end() || now - said->second >= std::chrono::seconds(5)))
        {
            i.log.warn(logcat::Move, "client pathfinding: {}", error.what());
            i.errorSaidAt[error.what()] = now;
        }

        return std::nullopt;
    }
}

bool ClientNavigationProvider::canWalkStraight(const Vec3& from, const Vec3& to) const
{
    auto& i = *impl_;

    if (i.obstacles.stopToken().stop_requested())
    {
        return false;
    }

    if (!i.map)
    {
        return false;
    }

    if (!i.mesh)
    {
        i.reset();
    }

    const int x0 = static_cast<int>(std::floor(std::min(from.x, to.x) / kTile)) - 1;
    const int x1 = static_cast<int>(std::floor(std::max(from.x, to.x) / kTile)) + 1;
    const int y0 = static_cast<int>(std::floor(std::min(from.y, to.y) / kTile)) - 1;
    const int y1 = static_cast<int>(std::floor(std::max(from.y, to.y) / kTile)) + 1;
    const int count = (x1 - x0 + 1) * (y1 - y0 + 1);

    if (count <= 36)
    {
        if (i.loaded.size() + count > 240)
        {
            i.reset();
        }

        for (int x = x0; x <= x1; ++x)
        {
            for (int y = y0; y <= y1; ++y)
            {
                i.tile({x, y});
            }
        }
    }

    const auto a = nav(from), b = nav(to);
    const float extent[3]{1, 4, 1};
    float start[3]{};
    dtPolyRef first = 0;
    dtQueryFilter filter;
    filter.setIncludeFlags(1);

    if (dtStatusFailed(i.query->findNearestPoly(a.data(), extent, &filter, &first, start)) || !first || std::hypot(start[0] - a[0], start[2] - a[2]) > 0.75f)
    {
        return false;
    }

    std::array<dtPolyRef, 512> visited{};
    int countVisited = 0;
    float hit = 0, normal[3]{};
    const auto status = i.query->raycast(first, start, b.data(), &filter, &hit, normal, visited.data(), &countVisited, visited.size());

    if (dtStatusFailed(status) || dtStatusDetail(status, DT_BUFFER_TOO_SMALL) || hit < 1 || countVisited == 0)
    {
        return false;
    }

    float height = 0;
    return dtStatusSucceed(i.query->getPolyHeight(visited[countVisited - 1], b.data(), &height)) && std::abs(height - b[1]) <= 2;
}

std::optional<float> ClientNavigationProvider::groundBelow(const Vec3& at) const
{
    auto& i = *impl_;

    if (i.obstacles.stopToken().stop_requested() || !i.map)
    {
        return std::nullopt;
    }

    if (!i.mesh)
    {
        i.reset();
    }

    i.tile({static_cast<int>(std::floor(at.x / kTile)), static_cast<int>(std::floor(at.y / kTile))});
    const auto a = nav(at);
    dtQueryFilter filter;
    filter.setIncludeFlags(1);
    // On the map where it stands, by the measure a route's start is taken
    // by (findPath). The search box alone will not do: the sloping bed of a
    // lake reaches up into it at the bank, with the ground under the point
    // itself yards below - a ghost on Lake Everstill "stood on the map".
    {
        const float extent[3]{2, 4, 2};
        dtPolyRef poly = 0;
        float pt[3]{};

        if (dtStatusSucceed(i.query->findNearestPoly(a.data(), extent, &filter, &poly, pt)) && poly && std::abs(pt[1] - a[1]) <= 3)
        {
            return std::nullopt;
        }
    }

    // The first ground below, layer by layer: the nearest in one tall box
    // could as well be a cave under the lake. Straight under the point, or
    // nearly: a bank beside it is no ground to stand on.
    constexpr float kBeside = 2.0f;
    constexpr float kLayer = 4.0f;
    constexpr float kDeepest = 120.0f;

    for (float depth = kLayer; depth <= kDeepest; depth += kLayer)
    {
        const float from[3]{a[0], a[1] - depth, a[2]};
        const float extent[3]{kBeside, kLayer, kBeside};
        dtPolyRef poly = 0;
        float pt[3]{};

        if (dtStatusFailed(i.query->findNearestPoly(from, extent, &filter, &poly, pt)) || !poly || std::hypot(pt[0] - a[0], pt[2] - a[2]) > kBeside || pt[1] > a[1] - 3)
        {
            continue;
        }

        return pt[1];
    }

    return std::nullopt;
}

std::optional<Vec3> ClientNavigationProvider::findWalkablePoint(const Vec3& near, float maxDistance, float clearance) const
{
    auto& i = *impl_;

    if (i.obstacles.stopToken().stop_requested())
    {
        return std::nullopt;
    }

    if (!i.map)
    {
        return std::nullopt;
    }

    if (!i.mesh)
    {
        i.reset();
    }

    const int x = static_cast<int>(std::floor(near.x / kTile));
    const int y = static_cast<int>(std::floor(near.y / kTile));

    if (i.loaded.size() + 9 > 240)
    {
        i.reset();
    }

    for (int dx = -1; dx <= 1; ++dx)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            i.tile({x + dx, y + dy});
        }
    }

    const auto a = nav(near);
    dtQueryFilter filter;
    filter.setIncludeFlags(1);
    const float extent[3]{maxDistance, 128.0f, maxDistance};
    dtPolyRef poly = 0;
    float pt[3]{};

    if (dtStatusFailed(i.query->findNearestPoly(a.data(), extent, &filter, &poly, pt)) || !poly)
    {
        return std::nullopt;
    }

    const float dist2D = std::hypot(pt[0] - a[0], pt[2] - a[2]);

    if (dist2D > maxDistance)
    {
        return std::nullopt;
    }

    if (clearance > 0.0f)
    {
        float hitDist = 0.0f;
        float hitPos[3]{};
        float hitNormal[3]{};

        if (dtStatusSucceed(i.query->findDistanceToWall(poly, pt, clearance + 0.5f, &filter, &hitDist, hitPos, hitNormal)))
        {
            if (hitDist < clearance)
            {
                return std::nullopt;
            }
        }
    }

    return world(pt);
}

std::optional<INavigationProvider::Ground> ClientNavigationProvider::groundAt(const Vec3& at, float farthest) const
{
    auto& i = *impl_;

    if (i.obstacles.stopToken().stop_requested() || !i.map)
    {
        return std::nullopt;
    }

    // Asked while the player has the keys, twice a second for as long as they
    // play: no route is planned then to start a full mesh afresh, and a tile
    // that cannot be built (or a Stop while it is) must not end the bot's loop.
    try
    {
        if (!i.mesh || i.loaded.size() + 1 > 240)
        {
            i.reset();
        }

        // The tile stood on only: it must not build a neighbourhood.
        i.tile({static_cast<int>(std::floor(at.x / kTile)), static_cast<int>(std::floor(at.y / kTile))});
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }

    const auto a = nav(at);
    dtQueryFilter filter;
    filter.setIncludeFlags(1);
    const float extent[3]{2.0f, 6.0f, 2.0f};
    dtPolyRef ref = 0;
    float pt[3]{};

    if (dtStatusFailed(i.query->findNearestPoly(a.data(), extent, &filter, &ref, pt)) || !ref)
    {
        return std::nullopt;
    }

    const dtMeshTile* tile = nullptr;
    const dtPoly* poly = nullptr;

    if (dtStatusFailed(i.mesh->getTileAndPolyByRef(ref, &tile, &poly)) || !poly)
    {
        return std::nullopt;
    }

    Ground ground;
    ground.road = poly->getArea() == kRoadArea;
    ground.wall = farthest;
    float hitDist = 0.0f;
    float hitPos[3]{};
    float hitNormal[3]{};

    if (dtStatusSucceed(i.query->findDistanceToWall(ref, pt, farthest, &filter, &hitDist, hitPos, hitNormal)))
    {
        ground.wall = std::min(hitDist, farthest);
    }

    return ground;
}

std::vector<Vec3> smoothSmallSteps(const Vec3& from, const std::vector<Vec3>& points, const std::function<bool(const Vec3&, const Vec3&)>& straight, float within)
{
    std::vector<Vec3> out;
    out.reserve(points.size());
    Vec3 anchor = from;
    // The corners left out since the last one kept: all of them stay near the
    // line, or a long gentle bend would be cut off the road a step at a time.
    std::vector<Vec3> skipped;

    for (std::size_t j = 0; j < points.size(); ++j)
    {
        // The last point is where the way goes: always kept.
        if (j + 1 < points.size())
        {
            const Vec3& next = points[j + 1];
            const float dx = next.x - anchor.x, dy = next.y - anchor.y;
            const float length = std::hypot(dx, dy);
            const auto off = [&](const Vec3& p) { return length > 0.01f ? std::abs((p.x - anchor.x) * dy - (p.y - anchor.y) * dx) / length : distance2D(p, anchor); };
            skipped.push_back(points[j]);

            if (std::all_of(skipped.begin(), skipped.end(), [&](const Vec3& p) { return off(p) <= within; }) && straight(anchor, next))
            {
                continue;
            }

            skipped.clear();
        }

        out.push_back(points[j]);
        anchor = points[j];
    }

    return out;
}

} // namespace og
