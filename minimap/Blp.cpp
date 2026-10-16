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
 *  File: Blp.cpp
 *
 *  Desc:
 *  Implementation of Blp (minimap extraction, BLP decoding, and radar
 *  map rendering).
 *
 *
 */
#include "og/minimap/Blp.h"

#include <cstring>

namespace og::minimap
{

namespace
{

// BLP2 header: magic, type, encoding, alphaDepth, alphaEncoding, hasMips,
// width, height, 16 mip offsets, 16 mip sizes, then a 256-entry BGRA palette.
constexpr std::size_t kHeaderSize = 1172;
constexpr std::size_t kPaletteOffset = 148;
constexpr std::size_t kMipOffsets = 20;
constexpr std::size_t kMipSizes = 84;

constexpr std::uint8_t kEncodingPalette = 1;
constexpr std::uint8_t kEncodingDxt = 2;
constexpr std::uint8_t kEncodingArgb = 3;

std::uint32_t readU32(std::span<const std::uint8_t> d, std::size_t at)
{
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + at, sizeof(v));
    return v;
}

bool fail(std::string* error, const char* message)
{
    if (error)
    {
        *error = message;
    }

    return false;
}

struct Rgba
{
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

Rgba from565(std::uint16_t c)
{
    // Replicate the high bits into the low ones so white stays white.
    const std::uint8_t r5 = (c >> 11) & 0x1F;
    const std::uint8_t g6 = (c >> 5) & 0x3F;
    const std::uint8_t b5 = c & 0x1F;
    return {static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)), static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)), static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)), 255};
}

std::uint8_t lerp(std::uint8_t a, std::uint8_t b, int num, int den)
{
    return static_cast<std::uint8_t>((a * (den - num) + b * num) / den);
}

void put(Image& out, int x, int y, const Rgba& c)
{
    if (x >= out.width || y >= out.height)
    {
        return; // partial edge block
    }

    const std::size_t at = (static_cast<std::size_t>(y) * out.width + x) * 4;
    out.pixels[at + 0] = c.r;
    out.pixels[at + 1] = c.g;
    out.pixels[at + 2] = c.b;
    out.pixels[at + 3] = c.a;
}

// One 4x4 colour block, shared by DXT1/3/5. `opaque` forces the four-colour
// interpretation, which DXT3 and DXT5 always use.
void decodeColorBlock(std::span<const std::uint8_t> block, Image& out, int bx, int by, bool opaque, const std::uint8_t alpha[16])
{
    std::uint16_t c0 = 0, c1 = 0;
    std::memcpy(&c0, block.data(), 2);
    std::memcpy(&c1, block.data() + 2, 2);
    std::uint32_t bits = readU32(block, 4);

    Rgba palette[4];
    palette[0] = from565(c0);
    palette[1] = from565(c1);

    if (opaque || c0 > c1)
    {
        for (int i = 0; i < 3; ++i)
        {
            reinterpret_cast<std::uint8_t*>(&palette[2])[i] = lerp(reinterpret_cast<std::uint8_t*>(&palette[0])[i], reinterpret_cast<std::uint8_t*>(&palette[1])[i], 1, 3);
            reinterpret_cast<std::uint8_t*>(&palette[3])[i] = lerp(reinterpret_cast<std::uint8_t*>(&palette[0])[i], reinterpret_cast<std::uint8_t*>(&palette[1])[i], 2, 3);
        }

        palette[2].a = palette[3].a = 255;
    }
    else
    {
        for (int i = 0; i < 3; ++i)
        {
            reinterpret_cast<std::uint8_t*>(&palette[2])[i] = lerp(reinterpret_cast<std::uint8_t*>(&palette[0])[i], reinterpret_cast<std::uint8_t*>(&palette[1])[i], 1, 2);
        }

        palette[2].a = 255;
        palette[3] = {0, 0, 0, 0}; // the 1-bit-alpha case
    }

    for (int i = 0; i < 16; ++i)
    {
        Rgba c = palette[(bits >> (i * 2)) & 0x3];

        if (alpha)
        {
            c.a = alpha[i];
        }

        put(out, bx + (i % 4), by + (i / 4), c);
    }
}

void decodeDxt(std::span<const std::uint8_t> data, Image& out, int mode)
{
    const int blockBytes = mode == 1 ? 8 : 16;
    std::size_t at = 0;

    for (int by = 0; by < out.height; by += 4)
    {
        for (int bx = 0; bx < out.width; bx += 4)
        {
            if (at + blockBytes > data.size())
            {
                return;
            }

            std::uint8_t alpha[16];
            const std::uint8_t* colour = data.data() + at;

            if (mode == 3)
            {
                for (int i = 0; i < 16; ++i)
                {
                    const std::uint8_t pair = data[at + i / 2];
                    const std::uint8_t nibble = (i % 2) ? (pair >> 4) : (pair & 0x0F);
                    alpha[i] = static_cast<std::uint8_t>(nibble * 17); // 0..15 -> 0..255
                }

                colour += 8;
            }
            else if (mode == 5)
            {
                const std::uint8_t a0 = data[at], a1 = data[at + 1];
                std::uint64_t bits = 0;
                std::memcpy(&bits, data.data() + at + 2, 6);

                for (int i = 0; i < 16; ++i)
                {
                    const int code = static_cast<int>((bits >> (i * 3)) & 0x7);

                    if (code == 0)
                    {
                        alpha[i] = a0;
                    }
                    else if (code == 1)
                    {
                        alpha[i] = a1;
                    }
                    else if (a0 > a1)
                    {
                        alpha[i] = lerp(a0, a1, code - 1, 7);
                    }
                    else if (code == 6)
                    {
                        alpha[i] = 0;
                    }
                    else if (code == 7)
                    {
                        alpha[i] = 255;
                    }
                    else
                    {
                        alpha[i] = lerp(a0, a1, code - 1, 5);
                    }
                }

                colour += 8;
            }

            decodeColorBlock({colour, 8}, out, bx, by, mode != 1, mode == 1 ? nullptr : alpha);
            at += blockBytes;
        }
    }
}

} // namespace

Image decodeBlp(std::span<const std::uint8_t> data, std::string* error)
{
    Image out;

    if (data.size() < kHeaderSize)
    {
        fail(error, "shorter than a BLP2 header");
        return out;
    }

    if (std::memcmp(data.data(), "BLP2", 4) != 0)
    {
        fail(error, "not a BLP2 file");
        return out;
    }

    const std::uint8_t encoding = data[8];
    const std::uint8_t alphaDepth = data[9];
    const std::uint8_t alphaEncoding = data[10];
    const auto width = static_cast<int>(readU32(data, 12));
    const auto height = static_cast<int>(readU32(data, 16));

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        fail(error, "implausible dimensions");
        return out;
    }

    const std::uint32_t offset = readU32(data, kMipOffsets);
    const std::uint32_t size = readU32(data, kMipSizes);

    if (offset == 0 || size == 0 || offset + static_cast<std::size_t>(size) > data.size())
    {
        fail(error, "mip level 0 is missing or truncated");
        return out;
    }

    const auto level = data.subspan(offset, size);

    out.width = width;
    out.height = height;
    out.pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);

    switch (encoding)
    {
    case kEncodingDxt:
    {
        // alphaDepth picks the flavour: none/1-bit is DXT1, 4-bit DXT3, 8-bit
        // is DXT3 or DXT5 depending on alphaEncoding.
        const int mode = alphaDepth <= 1 ? 1 : (alphaDepth == 8 && alphaEncoding == 7) ? 5 : 3;
        decodeDxt(level, out, mode);
        return out;
    }

    case kEncodingPalette:
    {
        const std::size_t pixels = static_cast<std::size_t>(width) * height;

        if (level.size() < pixels)
        {
            fail(error, "palettised data is truncated");
            out = {};
            return out;
        }

        for (std::size_t i = 0; i < pixels; ++i)
        {
            const std::size_t entry = kPaletteOffset + static_cast<std::size_t>(level[i]) * 4;
            std::uint8_t a = 255;

            if (alphaDepth == 8 && level.size() >= pixels * 2)
            {
                a = level[pixels + i];
            }
            else if (alphaDepth == 1 && level.size() >= pixels + (pixels + 7) / 8)
            {
                const std::uint8_t byte = level[pixels + i / 8];
                a = (byte >> (i % 8)) & 1 ? 255 : 0;
            }

            out.pixels[i * 4 + 0] = data[entry + 2]; // palette is BGRA
            out.pixels[i * 4 + 1] = data[entry + 1];
            out.pixels[i * 4 + 2] = data[entry + 0];
            out.pixels[i * 4 + 3] = a;
        }

        return out;
    }

    case kEncodingArgb:
    {
        const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;

        if (level.size() < bytes)
        {
            fail(error, "BGRA data is truncated");
            out = {};
            return out;
        }

        for (std::size_t i = 0; i < bytes; i += 4)
        {
            out.pixels[i + 0] = level[i + 2];
            out.pixels[i + 1] = level[i + 1];
            out.pixels[i + 2] = level[i + 0];
            out.pixels[i + 3] = level[i + 3];
        }

        return out;
    }

    default:
        fail(error, "unknown BLP encoding");
        out = {};
        return out;
    }
}

} // namespace og::minimap
