/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-14
 *  File: CascMinimap.h
 *
 *  Desc:
 *  Declarations and interface for CascMinimap (minimap extraction, BLP
 *  decoding, and radar map rendering).
 *
 *
 */
#pragma once

#include "og/minimap/Blp.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace og::minimap
{

// Reads minimap tiles out of the CASC storage of the 64-bit clients
// (TBC Anniversary 2.5.6 and the other modern Classic builds).
//
// Files in CASC have no names, only FileDataIDs. A map's WDT lists the ids of
// its minimap tiles in its MAID chunk, and the WDTs of the continents have
// fixed ids, so no listfile is needed. Keys are spelled like MpqMinimap's
// ("expansion01\map20_30.blp") so the cache treats both sources alike.
class CascMinimap
{
public:
    CascMinimap();
    ~CascMinimap();
    CascMinimap(const CascMinimap&) = delete;
    CascMinimap& operator=(const CascMinimap&) = delete;

    // `storage` is the directory holding .build.info, `product` one of the
    // products in it ("wow_anniversary"). False if it cannot be opened;
    // `lastError()` says why.
    bool open(const std::filesystem::path& storage, const std::string& product);
    bool isOpen() const
    {
        return storage_ != nullptr;
    }

    // Tile keys of a continent ("Expansion01"); reads its WDT the first time.
    std::vector<std::string> tilesFor(std::string_view mapDirectory);
    bool hasTile(std::string_view key);
    // Decoded tile, or an empty image when it is missing or unreadable.
    Image readTile(const std::string& key);

    const std::string& lastError() const
    {
        return error_;
    }

    // Raw client data for terrain/collision consumers. This instance is used
    // only on its owner's thread, separate from the radar's storage handle.
    std::vector<std::uint8_t> readFile(std::uint32_t fileDataId) const
    {
        return read(fileDataId);
    }

private:
    using TileIds = std::array<std::uint32_t, 64 * 64>;
    const TileIds* tileIds(const std::string& mapDirectory);
    std::vector<std::uint8_t> read(std::uint32_t fileDataId) const;

    void* storage_ = nullptr;
    std::map<std::string, TileIds, std::less<>> maps_; // lower-case directory -> ids, 0 = no tile
    std::string error_;
};

// Minimap FileDataIDs from a WDT file, indexed row * 64 + col, 0 where a tile has
// none. Empty when the file has no MAID chunk.
std::vector<std::uint32_t> parseWdtMinimapIds(std::span<const std::uint8_t> wdt);

// WDT FileDataID of a continent's directory ("Expansion01"); nullopt for any other map.
std::optional<std::uint32_t> wdtFileDataId(std::string_view mapDirectory);

// The directory holding .build.info at or above `path`: a client's install
// directory sits one level below its storage.
std::optional<std::filesystem::path> findCascStorage(const std::filesystem::path& path);

// The product in a .build.info whose version is `build` ("2.5.6.69795" ->
// "wow_anniversary"), or nullopt.
std::optional<std::string> productForBuild(std::string_view buildInfo, std::uint32_t build);

} // namespace og::minimap
