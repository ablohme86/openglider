/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-12
 *  File: MinimapTiles.cpp
 *
 *  Desc:
 *  Implementation of MinimapTiles (minimap extraction, BLP decoding,
 *  and radar map rendering).
 *
 *
 */
#include "og/minimap/MinimapTiles.h"

#include <cmath>
#include <format>
#include <unordered_map>

namespace og::minimap
{

TileRef tileFor(double worldX, double worldY)
{
    const double col = 32.0 - worldY / kTileYards;
    const double row = 32.0 - worldX / kTileYards;
    TileRef ref;
    ref.col = static_cast<int>(std::floor(col));
    ref.row = static_cast<int>(std::floor(row));
    ref.pixelX = (col - ref.col) * kTilePixels;
    ref.pixelY = (row - ref.row) * kTilePixels;
    return ref;
}

void tileOrigin(int col, int row, double& worldX, double& worldY)
{
    worldY = (32.0 - col) * kTileYards;
    worldX = (32.0 - row) * kTileYards;
}

std::string tileFileName(std::string_view mapDirectory, int col, int row)
{
    return std::format("{}\\map{}_{}.blp", mapDirectory, col, row);
}

std::optional<std::string> mapDirectoryFor(std::uint32_t mapId)
{
    // From Map.dbc. Only the outdoor maps matter for a route-running bot;
    // reading the DBC for the rest is not worth the dependency.
    static const std::unordered_map<std::uint32_t, std::string> kMaps = {
        {0, "Azeroth"},
        {1, "Kalimdor"},
        {530, "Expansion01"},
        {571, "Northrend"},
        {609, "DeathKnightStart"},
        {33, "Shadowfang"},
        {36, "DeadminesInstance"},
        // 1.60.1's new zone: newer maps are named after their id.
        {2991, "2991"},
    };
    const auto it = kMaps.find(mapId);

    if (it == kMaps.end())
    {
        return std::nullopt;
    }

    return it->second;
}

} // namespace og::minimap
