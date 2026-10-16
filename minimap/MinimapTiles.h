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
 *  File: MinimapTiles.h
 *
 *  Desc:
 *  Declarations and interface for MinimapTiles (minimap extraction, BLP
 *  decoding, and radar map rendering).
 *
 *
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace og::minimap
{

// The world is a 64x64 grid of square terrain tiles ("ADTs"). The minimap
// stores one 256x256 image per tile, named map<col>_<row>.blp, so a world
// position maps to a tile plus a pixel inside it.
inline constexpr double kTileYards = 533.0 + 1.0 / 3.0;
inline constexpr int kTilePixels = 256;
inline constexpr int kGrid = 64;
inline constexpr double kYardsPerPixel = kTileYards / kTilePixels;

// The grid runs from the centre of the map outwards, and the client's axes are
// swapped relative to the tile indices: +X is north (rows) and +Y is west
// (columns), both counting down from 32.
struct TileRef
{
    int col = 0; // from world Y
    int row = 0; // from world X
    // Pixel inside the tile, 0..255, top-left origin.
    double pixelX = 0;
    double pixelY = 0;

    bool valid() const
    {
        return col >= 0 && col < kGrid && row >= 0 && row < kGrid;
    }
};

TileRef tileFor(double worldX, double worldY);

// North-west corner of a tile in world coordinates, so the radar can place the
// image without going back through tileFor().
void tileOrigin(int col, int row, double& worldX, double& worldY);

// "Azeroth\map32_48.blp" - the key used by md5translate.trs.
std::string tileFileName(std::string_view mapDirectory, int col, int row);

// The client's directory name for a map id ("Azeroth" for 0). Only the maps a
// grinding bot plausibly visits are listed; nullopt for anything else, which
// simply means no minimap background there.
std::optional<std::string> mapDirectoryFor(std::uint32_t mapId);

} // namespace og::minimap
