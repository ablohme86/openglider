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
 *  File: CascMinimap.cpp
 *
 *  Desc:
 *  Implementation of CascMinimap (minimap extraction, BLP decoding, and
 *  radar map rendering).
 *
 *
 */
#include "og/minimap/CascMinimap.h"

#include <CascLib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

namespace og::minimap
{

namespace
{

std::string lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::uint32_t readU32(std::span<const std::uint8_t> data, std::size_t offset)
{
    std::uint32_t v = 0;
    std::memcpy(&v, data.data() + offset, sizeof(v));
    return v;
}

// "expansion01\map20_30.blp" -> directory, col, row.
bool parseKey(std::string_view key, std::string& directory, int& col, int& row)
{
    const std::size_t slash = key.find('\\');

    if (slash == std::string_view::npos)
    {
        return false;
    }

    directory = lower(key.substr(0, slash));
    const std::string file(key.substr(slash + 1));
    return std::sscanf(file.c_str(), "map%d_%d.blp", &col, &row) == 2 && col >= 0 && col < 64 && row >= 0 && row < 64;
}

std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;

    while (true)
    {
        const std::size_t end = text.find(separator, start);
        out.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));

        if (end == std::string_view::npos)
        {
            return out;
        }

        start = end + 1;
    }
}

} // namespace

std::vector<std::uint32_t> parseWdtMinimapIds(std::span<const std::uint8_t> wdt)
{
    // Chunks: a four-character id stored back to front, a u32 size, the body.
    // MAID has eight FileDataIDs per tile; the minimap texture is the last.
    constexpr std::size_t kPerTile = 8 * 4;
    std::size_t offset = 0;

    while (offset + 8 <= wdt.size())
    {
        const std::uint32_t size = readU32(wdt, offset + 4);

        if (offset + 8 + size > wdt.size())
        {
            break;
        }

        if (std::memcmp(wdt.data() + offset, "DIAM", 4) == 0 && size >= 64 * 64 * kPerTile)
        {
            std::vector<std::uint32_t> ids(64 * 64);

            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                ids[i] = readU32(wdt, offset + 8 + i * kPerTile + 28);
            }

            return ids;
        }

        offset += 8 + size;
    }

    return {};
}

std::optional<std::uint32_t> wdtFileDataId(std::string_view mapDirectory)
{
    // world/maps/<dir>/<dir>.wdt in the community listfile.
    static const std::map<std::string, std::uint32_t, std::less<>> kWdts = {{"azeroth", 775971},
                                                                            {"kalimdor", 782779},
                                                                            {"expansion01", 828395},
                                                                            // 1.60.1's new zone (map 2991); its MPHD names 7200160 as its _lgt.wdt.
                                                                            {"2991", 7198644}};
    const auto it = kWdts.find(lower(mapDirectory));

    if (it == kWdts.end())
    {
        return std::nullopt;
    }

    return it->second;
}

std::optional<std::filesystem::path> findCascStorage(const std::filesystem::path& path)
{
    std::error_code ec;

    for (std::filesystem::path dir = path; !dir.empty(); dir = dir.parent_path())
    {
        if (std::filesystem::is_regular_file(dir / ".build.info", ec))
        {
            return dir;
        }

        if (dir == dir.parent_path())
        {
            break;
        }
    }

    return std::nullopt;
}

std::optional<std::string> productForBuild(std::string_view buildInfo, std::uint32_t build)
{
    // "Name!TYPE:size|..." header, then one row per installed product.
    std::istringstream in{std::string(buildInfo)};
    std::string line;

    if (!std::getline(in, line))
    {
        return std::nullopt;
    }

    const auto header = split(line, '|');
    int versionColumn = -1, productColumn = -1;

    for (std::size_t i = 0; i < header.size(); ++i)
    {
        const std::string_view name = header[i].substr(0, header[i].find('!'));

        if (name == "Version")
        {
            versionColumn = static_cast<int>(i);
        }

        if (name == "Product")
        {
            productColumn = static_cast<int>(i);
        }
    }

    if (versionColumn < 0 || productColumn < 0)
    {
        return std::nullopt;
    }

    const std::string suffix = std::format(".{}", build);

    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        const auto fields = split(line, '|');

        if (static_cast<int>(fields.size()) <= std::max(versionColumn, productColumn))
        {
            continue;
        }

        if (fields[static_cast<std::size_t>(versionColumn)].ends_with(suffix))
        {
            return std::string(fields[static_cast<std::size_t>(productColumn)]);
        }
    }

    return std::nullopt;
}

CascMinimap::CascMinimap() = default;

CascMinimap::~CascMinimap()
{
    if (storage_)
    {
        CascCloseStorage(static_cast<HANDLE>(storage_));
    }
}

bool CascMinimap::open(const std::filesystem::path& storage, const std::string& product)
{
    if (storage_)
    {
        CascCloseStorage(static_cast<HANDLE>(storage_));
    }

    storage_ = nullptr;
    maps_.clear();
    const std::string path = storage.string();
    CASC_OPEN_STORAGE_ARGS args{};
    args.Size = sizeof(args);
    args.szLocalPath = path.c_str();
    args.szCodeName = product.c_str();
    args.dwLocaleMask = CASC_LOCALE_ALL;
    HANDLE handle = nullptr;

    if (!CascOpenStorageEx(nullptr, &args, false, &handle))
    {
        error_ = std::format("cannot open the CASC storage in {} for {} (error {})", path, product, GetCascError());
        return false;
    }

    storage_ = handle;
    return true;
}

std::vector<std::uint8_t> CascMinimap::read(std::uint32_t fileDataId) const
{
    std::vector<std::uint8_t> out;
    HANDLE file = nullptr;

    if (!storage_ || !CascOpenFile(static_cast<HANDLE>(storage_), CASC_FILE_DATA_ID(fileDataId), CASC_LOCALE_ALL, CASC_OPEN_BY_FILEID, &file))
    {
        return out;
    }

    ULONGLONG size = 0;

    // Tiles and WDTs are well under a megabyte; anything huge is not one.
    if (CascGetFileSize64(file, &size) && size > 0 && size < (64u << 20))
    {
        out.resize(static_cast<std::size_t>(size));
        DWORD got = 0;

        if (CascReadFile(file, out.data(), static_cast<DWORD>(size), &got))
        {
            out.resize(got);
        }
        else
        {
            out.clear();
        }
    }

    CascCloseFile(file);
    return out;
}

const CascMinimap::TileIds* CascMinimap::tileIds(const std::string& mapDirectory)
{
    const std::string key = lower(mapDirectory);

    if (const auto it = maps_.find(key); it != maps_.end())
    {
        return &it->second;
    }

    const auto wdt = wdtFileDataId(key);

    if (!wdt)
    {
        return nullptr;
    }

    const auto ids = parseWdtMinimapIds(read(*wdt));
    TileIds tiles{};

    if (ids.size() == tiles.size())
    {
        std::copy(ids.begin(), ids.end(), tiles.begin());
    }

    // Remembered even when unreadable, so a missing WDT is not read again for every tile.
    return &maps_.emplace(key, tiles).first->second;
}

std::vector<std::string> CascMinimap::tilesFor(std::string_view mapDirectory)
{
    std::vector<std::string> out;
    const TileIds* ids = tileIds(std::string(mapDirectory));

    if (!ids)
    {
        return out;
    }

    const std::string directory = lower(mapDirectory);

    for (int row = 0; row < 64; ++row)
    {
        for (int col = 0; col < 64; ++col)
        {
            if ((*ids)[static_cast<std::size_t>(row * 64 + col)] != 0)
            {
                out.push_back(std::format("{}\\map{}_{}.blp", directory, col, row));
            }
        }
    }

    return out;
}

bool CascMinimap::hasTile(std::string_view key)
{
    std::string directory;
    int col = 0, row = 0;

    if (!parseKey(key, directory, col, row))
    {
        return false;
    }

    const TileIds* ids = tileIds(directory);
    return ids && (*ids)[static_cast<std::size_t>(row * 64 + col)] != 0;
}

Image CascMinimap::readTile(const std::string& key)
{
    std::string directory;
    int col = 0, row = 0;

    if (!parseKey(key, directory, col, row))
    {
        return {};
    }

    const TileIds* ids = tileIds(directory);

    if (!ids)
    {
        return {};
    }

    const std::uint32_t id = (*ids)[static_cast<std::size_t>(row * 64 + col)];

    if (id == 0)
    {
        return {};
    }

    return decodeBlp(read(id));
}

} // namespace og::minimap
