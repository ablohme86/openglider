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
 *  File: Blp.h
 *
 *  Desc:
 *  Declarations and interface for Blp (minimap extraction, BLP
 *  decoding, and radar map rendering).
 *
 *
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace og::minimap
{

// A decoded image, 8 bits per channel, RGBA order, top row first.
struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // width * height * 4

    bool empty() const
    {
        return width <= 0 || height <= 0 || pixels.empty();
    }
};

// Decodes the top mip level of a BLP2 texture, which is what the client's
// minimap tiles are. Handles the three encodings those files use: DXT1/3/5,
// palettized, and raw BGRA. Returns an empty image on anything malformed;
// `error` then says why.
Image decodeBlp(std::span<const std::uint8_t> data, std::string* error = nullptr);

} // namespace og::minimap
