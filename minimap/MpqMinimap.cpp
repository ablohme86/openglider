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
 *  File: MpqMinimap.cpp
 *
 *  Desc:
 *  Implementation of MpqMinimap (minimap extraction, BLP decoding, and
 *  radar map rendering).
 *
 *
 */
#include "og/minimap/MpqMinimap.h"

#include <StormLib.h>

#include <algorithm>
#include <cctype>
#include <format>

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

// Archives are applied in this order, so a later one wins. Locale archives sit
// in a sub-directory (enUS, deDE, ...) and patch those again.
const char* const kBaseOrder[] = {"common.MPQ", "common-2.MPQ", "expansion.MPQ", "lichking.MPQ", "patch.MPQ", "patch-2.MPQ", "patch-3.MPQ"};

std::vector<std::uint8_t> readWhole(HANDLE archive, const char* name)
{
    std::vector<std::uint8_t> out;
    HANDLE file = nullptr;

    if (!SFileOpenFileEx(archive, name, 0, &file))
    {
        return out;
    }

    const DWORD size = SFileGetFileSize(file, nullptr);

    if (size != 0 && size != SFILE_INVALID_SIZE)
    {
        out.resize(size);
        DWORD got = 0;

        if (!SFileReadFile(file, out.data(), size, &got, nullptr))
        {
            out.clear();
        }
        else
        {
            out.resize(got);
        }
    }

    SFileCloseFile(file);
    return out;
}

} // namespace

struct MpqMinimap::Archives
{
    std::vector<HANDLE> handles;
    ~Archives()
    {
        for (HANDLE h : handles)
        {
            if (h)
            {
                SFileCloseArchive(h);
            }
        }
    }
};

MpqMinimap::MpqMinimap() : archives_(std::make_unique<Archives>())
{
}

MpqMinimap::~MpqMinimap() = default;

bool MpqMinimap::isOpen() const
{
    return !archives_->handles.empty();
}

std::vector<std::filesystem::path> MpqMinimap::archiveChain(const std::filesystem::path& dataDirectory)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;

    if (!std::filesystem::is_directory(dataDirectory, ec))
    {
        return out;
    }

    // Case-insensitive match: installs differ on "patch.MPQ" vs "patch.mpq".
    std::map<std::string, std::filesystem::path> present;

    for (const auto& entry : std::filesystem::directory_iterator(dataDirectory, ec))
    {
        if (entry.is_regular_file(ec))
        {
            present[lower(entry.path().filename().string())] = entry.path();
        }
    }

    for (const char* name : kBaseOrder)
    {
        const auto it = present.find(lower(name));

        if (it != present.end())
        {
            out.push_back(it->second);
        }
    }

    // Locale archives last so their patches win.
    for (const auto& entry : std::filesystem::directory_iterator(dataDirectory, ec))
    {
        if (!entry.is_directory(ec))
        {
            continue;
        }

        std::vector<std::filesystem::path> locale;

        for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec))
        {
            if (!file.is_regular_file(ec))
            {
                continue;
            }

            if (lower(file.path().extension().string()) == ".mpq")
            {
                locale.push_back(file.path());
            }
        }

        std::sort(locale.begin(), locale.end());
        out.insert(out.end(), locale.begin(), locale.end());
    }

    return out;
}

std::map<std::string, std::string, std::less<>> parseTranslateTable(std::string_view text)
{
    std::map<std::string, std::string, std::less<>> out;
    std::size_t start = 0;

    while (start <= text.size())
    {
        std::size_t end = text.find('\n', start);

        if (end == std::string_view::npos)
        {
            end = text.size();
        }

        std::string_view line = text.substr(start, end - start);
        start = end + 1;

        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        // Section headers ("dir: Azeroth") are not needed: every data line
        // already carries the full "Map\mapX_Y.blp" key.
        if (line.empty() || line.starts_with("dir:"))
        {
            continue;
        }

        const std::size_t tab = line.find('\t');

        if (tab == std::string_view::npos)
        {
            continue;
        }

        std::string key = lower(line.substr(0, tab));
        std::string value(line.substr(tab + 1));

        if (key.empty() || value.empty())
        {
            continue;
        }

        out[std::move(key)] = std::move(value);

        if (end == text.size())
        {
            break;
        }
    }

    return out;
}

bool MpqMinimap::loadTranslateTable(void* archive)
{
    for (const char* name : {"textures\\Minimap\\md5translate.trs", "textures\\minimap\\md5translate.trs"})
    {
        const auto body = readWhole(archive, name);

        if (body.empty())
        {
            continue;
        }

        auto table = parseTranslateTable(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));

        // Later archives override earlier entries.
        for (auto& [key, value] : table)
        {
            translate_[key] = std::move(value);
        }

        return true;
    }

    return false;
}

bool MpqMinimap::open(const std::filesystem::path& dataDirectory)
{
    const auto chain = archiveChain(dataDirectory);

    if (chain.empty())
    {
        error_ = std::format("no MPQ archives in {}", dataDirectory.string());
        return false;
    }

    for (const auto& path : chain)
    {
        HANDLE handle = nullptr;

        if (!SFileOpenArchive(path.string().c_str(), 0, MPQ_OPEN_READ_ONLY, &handle))
        {
            continue;
        }

        archives_->handles.push_back(handle);
        loadTranslateTable(handle);
    }

    if (archives_->handles.empty())
    {
        error_ = std::format("could not open any archive in {}", dataDirectory.string());
        return false;
    }

    if (translate_.empty())
    {
        error_ = "no md5translate.trs in the archives - minimap tiles cannot be located";
        return false;
    }

    return true;
}

std::vector<std::string> MpqMinimap::tilesFor(std::string_view mapDirectory) const
{
    const std::string prefix = lower(mapDirectory) + "\\";
    std::vector<std::string> out;

    for (auto it = translate_.lower_bound(prefix); it != translate_.end(); ++it)
    {
        if (!std::string_view(it->first).starts_with(prefix))
        {
            break;
        }

        out.push_back(it->first);
    }

    return out;
}

bool MpqMinimap::hasTile(std::string_view key) const
{
    return translate_.count(lower(key)) != 0;
}

Image MpqMinimap::readTile(const std::string& key) const
{
    const auto it = translate_.find(lower(key));

    if (it == translate_.end())
    {
        return {};
    }

    const std::string name = "textures\\Minimap\\" + it->second;

    // Newest archive first: it holds the current version of the tile.
    for (auto handle = archives_->handles.rbegin(); handle != archives_->handles.rend(); ++handle)
    {
        const auto body = readWhole(*handle, name.c_str());

        if (body.empty())
        {
            continue;
        }

        Image image = decodeBlp(body);

        if (!image.empty())
        {
            return image;
        }
    }

    return {};
}

} // namespace og::minimap
