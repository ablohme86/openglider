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
 *  File: MpqMinimap.h
 *
 *  Desc:
 *  Declarations and interface for MpqMinimap (minimap extraction, BLP
 *  decoding, and radar map rendering).
 *
 *
 */
#pragma once

#include "og/minimap/Blp.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace og::minimap
{

// Reads minimap tiles out of a client's Data directory.
//
// The archives store tiles under hashed names, with textures/Minimap/
// md5translate.trs mapping "Azeroth\map32_48.blp" to the hash. Later patch
// archives override earlier ones, so the chain is opened newest-last and the
// translate tables merged in that order.
class MpqMinimap
{
public:
    MpqMinimap();
    ~MpqMinimap();
    MpqMinimap(const MpqMinimap&) = delete;
    MpqMinimap& operator=(const MpqMinimap&) = delete;

    // Opens every archive in `dataDirectory` in patch order. False if none
    // could be read; `lastError()` says why.
    bool open(const std::filesystem::path& dataDirectory);
    bool isOpen() const;

    // Tile keys ("Azeroth\map32_48.blp") known for a map directory.
    std::vector<std::string> tilesFor(std::string_view mapDirectory) const;
    // Keys are matched case-insensitively: the translate table's spelling does
    // not always match the map directory name we build keys from.
    bool hasTile(std::string_view key) const;

    // Decoded tile, or an empty image when it is missing or unreadable.
    Image readTile(const std::string& key) const;

    std::size_t tileCount() const
    {
        return translate_.size();
    }

    const std::string& lastError() const
    {
        return error_;
    }

    // Archive order for a WoW data directory, base first and patches last.
    static std::vector<std::filesystem::path> archiveChain(const std::filesystem::path& dataDirectory);

private:
    bool loadTranslateTable(void* archive);

    struct Archives;
    std::unique_ptr<Archives> archives_;
    // "Azeroth\map32_48.blp" -> "1388bd....blp"
    std::map<std::string, std::string, std::less<>> translate_;
    std::string error_;
};

// Parses an md5translate.trs body into key -> hashed name. Exposed for tests.
std::map<std::string, std::string, std::less<>> parseTranslateTable(std::string_view text);

} // namespace og::minimap
