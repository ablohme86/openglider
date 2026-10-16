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
 *  File: ClientMapSource.cpp
 *
 *  Desc:
 *  Implementation of ClientMapSource (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/ClientNavigationProvider.h"
#ifdef OG_CLIENT_MAP_CASC
#include "og/minimap/CascMinimap.h"
#include "og/minimap/MinimapTiles.h"
#endif
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace og
{
#ifdef OG_CLIENT_MAP_CASC
namespace
{
class Source final : public IClientGeometrySource
{
public:
    minimap::CascMinimap archive;
    std::map<std::uint32_t, std::vector<std::uint8_t>> wdts;
    std::map<std::pair<std::uint32_t, std::uint16_t>, CollisionGeometry> models;
    std::map<std::uint32_t, std::vector<std::uint8_t>>::iterator wdt(std::uint32_t map)
    {
        auto found = wdts.find(map);

        if (found == wdts.end())
        {
            const auto dir = minimap::mapDirectoryFor(map);
            const auto id = dir ? minimap::wdtFileDataId(*dir) : std::nullopt;

            if (!id)
            {
                throw std::runtime_error("client map directory is not supported");
            }

            auto bytes = archive.readFile(*id);

            if (bytes.empty())
            {
                throw std::runtime_error("client WDT is unavailable locally");
            }

            found = wdts.emplace(map, std::move(bytes)).first;
        }

        return found;
    }

    std::optional<std::vector<std::uint32_t>> groundTextures(std::uint32_t map, int col, int row) override
    {
        try
        {
            const auto tex0 = adtTextureFileId(wdt(map)->second, col, row);

            if (!tex0)
            {
                return std::nullopt;
            }

            const auto bytes = archive.readFile(tex0);

            if (bytes.empty())
            {
                return std::nullopt;
            }

            return parseAdtTextureIds(bytes);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    std::vector<float> waterLevels(std::uint32_t map, int col, int row) override
    {
        try
        {
            const auto ids = adtFileIds(wdt(map)->second, col, row);

            if (ids.empty() || !ids[0])
            {
                return {};
            }

            const auto terrain = archive.readFile(ids[0]);
            return terrain.empty() ? std::vector<float>{} : parseAdtWaterLevels(terrain);
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    std::vector<std::uint32_t> areaIds(std::uint32_t map, int col, int row) override
    {
        try
        {
            const auto ids = adtFileIds(wdt(map)->second, col, row);

            if (ids.empty() || !ids[0])
            {
                return {};
            }

            const auto terrain = archive.readFile(ids[0]);
            return terrain.empty() ? std::vector<std::uint32_t>{} : parseAdtAreaIds(terrain);
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    CollisionGeometry load(std::uint32_t map, int col, int row, std::stop_token stop) override
    {
        if (stop.stop_requested())
        {
            throw std::runtime_error("map loading cancelled");
        }

        const auto found = wdt(map);
        const auto ids = adtFileIds(found->second, col, row);

        if (ids.empty() || !ids[0])
        {
            return {}; // Outside the map: an empty tile, not missing archive data.
        }

        const auto terrain = archive.readFile(ids[0]);

        if (terrain.empty())
        {
            throw std::runtime_error("client terrain tile is unavailable locally");
        }

        // Roads from the texture tile; without it the ground is all alike.
        std::vector<std::uint8_t> roads;

        if (const auto tex0 = adtTextureFileId(found->second, col, row))
        {
            if (const auto textures = archive.readFile(tex0); !textures.empty())
            {
                try
                {
                    roads = parseAdtRoadCells(textures);
                }
                catch (const std::exception&)
                {
                    roads.clear();
                }
            }
        }

        auto geometry = parseAdtTerrain(terrain, roads, true);
        std::set<std::pair<bool, std::uint32_t>> seen;

        for (auto id : {ids[1], ids[2]})
        {
            if (!id)
            {
                continue;
            }

            const auto bytes = archive.readFile(id);

            if (bytes.empty())
            {
                throw std::runtime_error("client object tile is unavailable locally");
            }

            for (const auto& placement : parseAdtObjects(bytes))
            {
                if (stop.stop_requested())
                {
                    throw std::runtime_error("map loading cancelled");
                }

                if (!seen.emplace(placement.worldModel, placement.uniqueId).second)
                {
                    continue;
                }

                // A world model's furniture differs by doodad set.
                const auto key = std::pair{placement.fileId, placement.worldModel ? placement.doodadSet : 0};
                auto model = models.find(key);

                if (model == models.end())
                {
                    const auto data = archive.readFile(placement.fileId);

                    if (data.empty())
                    {
                        throw std::runtime_error("client collision model is unavailable locally");
                    }

                    auto collision = placement.worldModel ? parseWorldModelCollision(data, [this](auto fid) { return archive.readFile(fid); }, placement.doodadSet) : parseModelCollision(data);

                    if (models.size() >= 128)
                    {
                        models.clear();
                    }

                    model = models.emplace(key, std::move(collision)).first;
                }

                auto placed = placeModel(model->second, placement);
                placed.roads.assign(placed.triangles.size() / 3, kObjectSurface);
                geometry.append(placed);
            }
        }

        return geometry;
    }
};
} // namespace
#endif
std::unique_ptr<IClientGeometrySource> openClientGeometry(const std::filesystem::path& storage, std::uint32_t build, std::string& error)
{
#ifdef OG_CLIENT_MAP_CASC
    std::ifstream file(storage / ".build.info");
    std::ostringstream text;
    text << file.rdbuf();
    const auto product = minimap::productForBuild(text.str(), build);

    if (!product)
    {
        error = "Cannot locate CASC data for the attached client build.";
        return {};
    }

    auto source = std::make_unique<Source>();

    if (!source->archive.open(storage, *product))
    {
        error = source->archive.lastError();
        return {};
    }

    return source;
#else
    (void)storage;
    (void)build;
    error = "This build cannot read client terrain data (CASC support is "
            "unavailable).";
    return {};
#endif
}

} // namespace og
