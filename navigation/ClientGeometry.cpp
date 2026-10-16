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
 *  File: ClientGeometry.cpp
 *
 *  Desc:
 *  Implementation of ClientGeometry (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/ClientGeometry.h"
#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>

namespace og
{
namespace
{
using Bytes = std::span<const std::uint8_t>;
template <class T> T get(Bytes b, std::size_t at)
{
    if (at > b.size() || sizeof(T) > b.size() - at)
    {
        throw std::runtime_error("truncated client map data");
    }

    T out;
    std::memcpy(&out, b.data() + at, sizeof out);
    return out;
}

Vec3 point(Bytes b, std::size_t at)
{
    Vec3 p{get<float>(b, at), get<float>(b, at + 4), get<float>(b, at + 8)};

    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
    {
        throw std::runtime_error("invalid client geometry coordinates");
    }

    return p;
}

template <class F> void chunks(Bytes b, F visit)
{
    std::size_t at = 0;

    while (at + 8 <= b.size())
    {
        const auto size = get<std::uint32_t>(b, at + 4);

        if (size > b.size() - at - 8)
        {
            throw std::runtime_error("invalid client map chunk size");
        }

        char name[4];

        for (int i = 0; i < 4; ++i)
        {
            name[i] = static_cast<char>(b[at + 3 - i]);
        }

        visit(std::string_view(name, 4), b.subspan(at + 8, size));
        at += 8 + size;
    }
}

Bytes chunk(Bytes b, std::string_view name)
{
    Bytes out;
    chunks(b,
           [&](auto key, Bytes data)
           {
               if (key == name)
               {
                   out = data;
               }
           });
    return out;
}

void triangle(CollisionGeometry& g, int a, int b, int c)
{
    // Recast expects an upward normal for walking surfaces. World -> nav
    // swaps Y/Z; choose winding in that frame. Walls keep either winding.
    const auto ab = g.vertices[b] - g.vertices[a], ac = g.vertices[c] - g.vertices[a];

    if (ab.y * ac.x - ab.x * ac.y < 0)
    {
        std::swap(b, c);
    }

    g.triangles.insert(g.triangles.end(), {a, b, c});
}

} // namespace
void CollisionGeometry::append(const CollisionGeometry& other)
{
    const int offset = static_cast<int>(vertices.size());

    if (!roads.empty() || !other.roads.empty())
    {
        roads.resize(triangles.size() / 3, 0);

        if (other.roads.empty())
        {
            roads.resize(roads.size() + other.triangles.size() / 3, 0);
        }
        else
        {
            roads.insert(roads.end(), other.roads.begin(), other.roads.end());
        }
    }

    vertices.insert(vertices.end(), other.vertices.begin(), other.vertices.end());

    for (int i : other.triangles)
    {
        triangles.push_back(offset + i);
    }
}

std::uint32_t adtTextureFileId(Bytes wdt, int col, int row)
{
    if (col < 0 || col >= 64 || row < 0 || row >= 64)
    {
        return 0;
    }

    const auto maid = chunk(wdt, "MAID");
    const std::size_t at = (row * 64 + col) * 32;

    if (maid.size() < at + 32)
    {
        return 0;
    }

    return get<std::uint32_t>(maid, at + 12);
}

namespace
{
// Road textures of the old world, from the community listfile.
constexpr std::pair<std::uint32_t, std::string_view> kRoadTextures[] = {
    {186765, "tileset/aerie peaks/aeriepeaksrockroadbase.blp"},
    {186801, "tileset/alteracmtns/alteracrockroadbase.blp"},
    {186838, "tileset/ashenvale/ashenvaleroad.blp"},
    {186917, "tileset/barrens/barrensroad01.blp"},
    {329440, "tileset/barrens/barrensroad02.blp"},
    {186961, "tileset/burningstepps/burningsteppsspireroad01.blp"},
    {186974, "tileset/darkshore/darkshoreroad.blp"},
    {187034, "tileset/durotar/durotarroad.blp"},
    {187049, "tileset/duskwallow marsh/duskwallowcobblestone01.blp"},
    {187065, "tileset/duskwallow marsh/duskwallowstoneroad.blp"},
    {187071, "tileset/duskwood/duskwoodcobblestone.blp"},
    {187072, "tileset/duskwood/duskwoodcobblestone2.blp"},
    {187099, "tileset/elwynn/elwynncobblestonebase.blp"},
    {187101, "tileset/elwynn/elwynncobblestonebasealpha01.blp"},
    {187102, "tileset/elwynn/elwynncobblestonedarker.blp"},
    // Elwynn's roads, Goldshire to Northshire, are dirt.
    {187113, "tileset/elwynn/elwynndirtbase.blp"},
    {187114, "tileset/elwynn/elwynndirtbase2.blp"},
    {188477, "tileset/felwood/felwoodroad.blp"},
    {188507, "tileset/feralas/feralasroad.blp"},
    {188565, "tileset/ironforge/ironforgerock06road2.blp"},
    // Dun Morogh's roads, by what its minimap shows: "brown cracks", not
    // named a road (the snow-road is barely laid).
    {188571, "tileset/ironforge/ironforgerock09browncracks.blp"},
    {188609, "tileset/loch modan/lochmodanbrickroadbase.blp"},
    {188634, "tileset/moonglade/alphamoongladeroad.blp"},
    {188664, "tileset/mullgore/mullgoreroad01.blp"},
    {188697, "tileset/plaguelands/plaguedroadstone01.blp"},
    {188726, "tileset/plaguelandseast/eastplaguedroadbase.blp"},
    {188762, "tileset/redridge/redridgerockroadbase.blp"},
    {188795, "tileset/silverpine/silverpineroad.blp"},
    {397421, "tileset/silverpine/silverpinenewroad.blp"},
    {188817, "tileset/stranglethorn/stranglethorncobblestoneroad.blp"},
    {188853, "tileset/swamp of sorrows/swampsorrowsstoneroad07.blp"},
    {188910, "tileset/the blasted lands/blastedlandsroad.blp"},
    {188935, "tileset/thousandneedles/thousandneedlesroad.blp"},
    {188963, "tileset/tirisfall/tirisfallstoneroad01.blp"},
    {590178, "tileset/tirisfall/sm_road.blp"},
    {188981, "tileset/westwood/westfallcobblestonebase.blp"},
    // Westfall's roads are dirt.
    {188989, "tileset/westwood/westfalldirtbase.blp"},
    {189025, "tileset/wetlands/wetlandsroad01.blp"},
    {189070, "tileset/zulgurub/zulgurubroad01.blp"},
    {512022, "tileset/zulgurub/zulgurubroad02.blp"},
    {308329, "tileset/generic/icb_cobblestone_a.blp"},
    {308522, "tileset/generic/icb_cobblestone_b.blp"},
    {320467, "tileset/stormwindcity/sw_cobble_a.blp"},
    {320469, "tileset/stormwindcity/sw_cobble_brokena.blp"},
    {327763, "tileset/orgrimmar/orgmetalcobblea.blp"},
    {327822, "tileset/orgrimmar/orgmetalcobbleb.blp"},
    {350303, "tileset/orgrimmar/orgmetalcobblec.blp"},
    {365791, "tileset/orgrimmar/orgrim_road_01.blp"},
    // The same textures as the ground's texture files list them (MDID): the
    // _s variants, which carry the specular map as well.
    {186766, "tileset/aerie peaks/aeriepeaksrockroadbase_s.blp"},
    {186802, "tileset/alteracmtns/alteracrockroadbase_s.blp"},
    {186839, "tileset/ashenvale/ashenvaleroad_s.blp"},
    {186918, "tileset/barrens/barrensroad01_s.blp"},
    {329441, "tileset/barrens/barrensroad02_s.blp"},
    {186962, "tileset/burningstepps/burningsteppsspireroad01_s.blp"},
    {186975, "tileset/darkshore/darkshoreroad_s.blp"},
    {187035, "tileset/durotar/durotarroad_s.blp"},
    {187066, "tileset/duskwallow marsh/duskwallowstoneroad_s.blp"},
    {187074, "tileset/duskwood/duskwoodcobblestone_s.blp"},
    {187073, "tileset/duskwood/duskwoodcobblestone2_s.blp"},
    {187100, "tileset/elwynn/elwynncobblestonebase_s.blp"},
    {187116, "tileset/elwynn/elwynndirtbase_s.blp"},
    {187115, "tileset/elwynn/elwynndirtbase2_s.blp"},
    {188478, "tileset/felwood/felwoodroad_s.blp"},
    {188508, "tileset/feralas/feralasroad_s.blp"},
    {188566, "tileset/ironforge/ironforgerock06road2_s.blp"},
    {188572, "tileset/ironforge/ironforgerock09browncracks_s.blp"},
    {188610, "tileset/loch modan/lochmodanbrickroadbase_s.blp"},
    {188665, "tileset/mullgore/mullgoreroad01_s.blp"},
    {188727, "tileset/plaguelandseast/eastplaguedroadbase_s.blp"},
    {188763, "tileset/redridge/redridgerockroadbase_s.blp"},
    {188796, "tileset/silverpine/silverpineroad_s.blp"},
    {397422, "tileset/silverpine/silverpinenewroad_s.blp"},
    {188818, "tileset/stranglethorn/stranglethorncobblestoneroad_s.blp"},
    {188854, "tileset/swamp of sorrows/swampsorrowsstoneroad07_s.blp"},
    {188911, "tileset/the blasted lands/blastedlandsroad_s.blp"},
    {188936, "tileset/thousandneedles/thousandneedlesroad_s.blp"},
    {188964, "tileset/tirisfall/tirisfallstoneroad01_s.blp"},
    {590180, "tileset/tirisfall/sm_road_s.blp"},
    {188982, "tileset/westwood/westfallcobblestonebase_s.blp"},
    {188990, "tileset/westwood/westfalldirtbase_s.blp"},
    {189026, "tileset/wetlands/wetlandsroad01_s.blp"},
    {189071, "tileset/zulgurub/zulgurubroad01_s.blp"},
    {512023, "tileset/zulgurub/zulgurubroad02_s.blp"},
    {308330, "tileset/generic/icb_cobblestone_a_s.blp"},
    {308523, "tileset/generic/icb_cobblestone_b_s.blp"},
    {320468, "tileset/stormwindcity/sw_cobble_a_s.blp"},
    {320470, "tileset/stormwindcity/sw_cobble_brokena_s.blp"},
    {327764, "tileset/orgrimmar/orgmetalcobblea_s.blp"},
    {327823, "tileset/orgrimmar/orgmetalcobbleb_s.blp"},
    {350304, "tileset/orgrimmar/orgmetalcobblec_s.blp"},
    {365792, "tileset/orgrimmar/orgrim_road_01_s.blp"},
};
// One layer's 64x64 alpha map from MCAL at `at`: 8-bit, RLE-compressed or
// 4-bit, as the flags and the room it takes say.
std::array<std::uint8_t, 4096> alphaMap(Bytes mcal, std::size_t at, std::size_t size, bool compressed)
{
    std::array<std::uint8_t, 4096> out{};

    if (at > mcal.size())
    {
        return out;
    }

    const Bytes data = mcal.subspan(at, std::min(size, mcal.size() - at));

    if (compressed)
    {
        std::size_t in = 0, put = 0;

        while (put < out.size() && in < data.size())
        {
            const std::uint8_t head = data[in++];
            const std::size_t count = std::min<std::size_t>(head & 0x7f, out.size() - put);

            if (head & 0x80)
            {
                if (in >= data.size())
                {
                    break;
                }

                std::fill_n(out.begin() + put, count, data[in++]);
            }
            else
            {
                const std::size_t n = std::min(count, data.size() - in);
                std::copy_n(data.begin() + in, n, out.begin() + put);
                in += n;
            }

            put += count;
        }
    }
    else if (data.size() >= 4096)
    {
        std::copy_n(data.begin(), 4096, out.begin());
    }
    else
    {
        for (std::size_t i = 0; i < std::min<std::size_t>(data.size(), 2048); ++i)
        {
            out[i * 2] = static_cast<std::uint8_t>((data[i] & 0x0f) * 17);
            out[i * 2 + 1] = static_cast<std::uint8_t>((data[i] >> 4) * 17);
        }
    }

    return out;
}

} // namespace
bool isRoadTexture(std::uint32_t fileId)
{
    return std::any_of(std::begin(kRoadTextures), std::end(kRoadTextures), [&](const auto& t) { return t.first == fileId; });
}

bool isRoadTexture(std::string_view name)
{
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
    return std::any_of(std::begin(kRoadTextures), std::end(kRoadTextures), [&](const auto& t) { return t.second == lower; });
}

std::vector<std::uint32_t> parseAdtTextureIds(Bytes tex0)
{
    std::vector<std::uint32_t> out;
    const auto ids = chunk(tex0, "MDID");

    for (std::size_t at = 0; at + 4 <= ids.size(); at += 4)
    {
        out.push_back(get<std::uint32_t>(ids, at));
    }

    return out;
}

std::vector<std::uint8_t> parseAdtRoadCells(Bytes tex0)
{
    // Which of the tile's textures are roads: MDID lists them by file id,
    // older files by name in MTEX.
    std::vector<bool> road;

    if (const auto ids = chunk(tex0, "MDID"); !ids.empty())
    {
        for (std::size_t at = 0; at + 4 <= ids.size(); at += 4)
        {
            road.push_back(isRoadTexture(get<std::uint32_t>(ids, at)));
        }
    }
    else if (const auto names = chunk(tex0, "MTEX"); !names.empty())
    {
        std::size_t start = 0;

        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == 0)
            {
                if (i > start)
                {
                    road.push_back(isRoadTexture(std::string_view(reinterpret_cast<const char*>(names.data()) + start, i - start)));
                }

                start = i + 1;
            }
        }
    }

    if (std::none_of(road.begin(), road.end(), [](bool r) { return r; }))
    {
        return {};
    }

    std::vector<std::uint8_t> cells(256 * 64, 0);
    bool any = false;
    int index = 0;
    chunks(tex0,
           [&](auto name, Bytes body)
           {
               if (name != "MCNK")
               {
                   return;
               }

               const int chunkIndex = index++;

               if (chunkIndex >= 256)
               {
                   return;
               }

               const auto layers = chunk(body, "MCLY");
               const auto mcal = chunk(body, "MCAL");
               const std::size_t count = layers.size() / 16;

               if (count == 0)
               {
                   return;
               }

               // How much of each of the 64x64 texels shows road: each layer is
               // laid over those before it as much as its alpha says.
               const auto base = get<std::uint32_t>(layers, 0);
               std::array<int, 4096> weight{};
               weight.fill(base < road.size() && road[base] ? 255 : 0);

               for (std::size_t l = 1; l < count; ++l)
               {
                   const auto texture = get<std::uint32_t>(layers, l * 16);
                   const auto flags = get<std::uint32_t>(layers, l * 16 + 4);
                   const auto offset = get<std::uint32_t>(layers, l * 16 + 8);

                   if (!(flags & 0x100))
                   {
                       continue;
                   }

                   const bool isRoad = texture < road.size() && road[texture];
                   // A layer's map runs to where the next one's begins.
                   std::size_t size = mcal.size() > offset ? mcal.size() - offset : 0;

                   for (std::size_t k = 1; k < count; ++k)
                   {
                       const auto other = get<std::uint32_t>(layers, k * 16 + 8);

                       if (k != l && other > offset && (get<std::uint32_t>(layers, k * 16 + 4) & 0x100))
                       {
                           size = std::min<std::size_t>(size, other - offset);
                       }
                   }

                   const auto alpha = alphaMap(mcal, offset, size, (flags & 0x200) != 0);

                   for (std::size_t t = 0; t < 4096; ++t)
                   {
                       weight[t] = (weight[t] * (255 - alpha[t]) + (isRoad ? 255 * alpha[t] : 0)) / 255;
                   }
               }

               for (int r = 0; r < 8; ++r)
               {
                   for (int c = 0; c < 8; ++c)
                   {
                       int sum = 0;

                       for (int y = 0; y < 8; ++y)
                       {
                           for (int x = 0; x < 8; ++x)
                           {
                               sum += weight[(r * 8 + y) * 64 + c * 8 + x];
                           }
                       }

                       if (sum * 2 >= 255 * 64)
                       {
                           cells[chunkIndex * 64 + r * 8 + c] = 1;
                           any = true;
                       }
                   }
               }
           });

    if (!any)
    {
        return {};
    }

    return cells;
}

std::vector<std::uint8_t> keepRoads(std::vector<std::uint8_t> cells)
{
    constexpr int kSide = 128; // 16 chunks of 8 cells, 4.17 yd each
    // A road's cell has road about it: of the 25 cells round it, this many.
    // A line one cell wide has five.
    constexpr int kRoadAbout = 8;
    // Not reaching the tile's edge, a road is this long at the least (250 yd).
    constexpr int kRoadLong = 60;
    // A dirt road shows through the grass in stretches: cells this near one
    // another are the same road (12 yd).
    constexpr int kSameRoad = 3;

    if (cells.size() != static_cast<std::size_t>(kSide) * kSide)
    {
        return cells;
    }

    // The cells come chunk by chunk; laid out as the tile is, by row and column.
    const auto at = [](int row, int col) { return static_cast<std::size_t>((row / 8 * 16 + col / 8) * 64 + (row % 8) * 8 + col % 8); };
    std::vector<std::uint8_t> road(cells.size(), 0);

    for (int row = 0; row < kSide; ++row)
    {
        for (int col = 0; col < kSide; ++col)
        {
            if (!cells[at(row, col)])
            {
                continue;
            }

            int about = 0, seen = 0;

            for (int r = row - 2; r <= row + 2; ++r)
            {
                for (int c = col - 2; c <= col + 2; ++c)
                {
                    if (r >= 0 && r < kSide && c >= 0 && c < kSide)
                    {
                        ++seen;
                        about += cells[at(r, c)] ? 1 : 0;
                    }
                }
            }

            // At the tile's edge, of what can be seen of it.
            road[at(row, col)] = about * 25 >= kRoadAbout * seen ? 1 : 0;
        }
    }

    std::vector<std::uint8_t> kept(cells.size(), 0), visited(cells.size(), 0);
    std::vector<std::pair<int, int>> stack, piece;
    bool any = false;

    for (int row = 0; row < kSide; ++row)
    {
        for (int col = 0; col < kSide; ++col)
        {
            if (!road[at(row, col)] || visited[at(row, col)])
            {
                continue;
            }

            piece.clear();
            stack.assign(1, {row, col});
            visited[at(row, col)] = 1;
            int top = row, bottom = row, left = col, right = col;
            bool edge = false;

            while (!stack.empty())
            {
                const auto [r, c] = stack.back();
                stack.pop_back();
                piece.emplace_back(r, c);
                top = std::min(top, r);
                bottom = std::max(bottom, r);
                left = std::min(left, c);
                right = std::max(right, c);
                edge = edge || r == 0 || c == 0 || r == kSide - 1 || c == kSide - 1;

                for (int dr = -kSameRoad; dr <= kSameRoad; ++dr)
                {
                    for (int dc = -kSameRoad; dc <= kSameRoad; ++dc)
                    {
                        const int nr = r + dr, nc = c + dc;

                        if (nr >= 0 && nr < kSide && nc >= 0 && nc < kSide && road[at(nr, nc)] && !visited[at(nr, nc)])
                        {
                            visited[at(nr, nc)] = 1;
                            stack.emplace_back(nr, nc);
                        }
                    }
                }
            }

            // On to the next tile, or long enough to be going somewhere.
            if (edge || std::max(bottom - top, right - left) + 1 >= kRoadLong)
            {
                for (const auto& [r, c] : piece)
                {
                    kept[at(r, c)] = 1;
                }

                any = true;
            }
        }
    }

    return any ? kept : std::vector<std::uint8_t>{};
}

std::vector<std::uint32_t> adtFileIds(Bytes wdt, int col, int row)
{
    if (col < 0 || col >= 64 || row < 0 || row >= 64)
    {
        return {};
    }

    const auto maid = chunk(wdt, "MAID");
    const std::size_t at = (row * 64 + col) * 32;

    if (maid.size() < at + 32)
    {
        return {};
    }

    return {get<std::uint32_t>(maid, at), get<std::uint32_t>(maid, at + 4), get<std::uint32_t>(maid, at + 8)};
}

namespace
{

// How high the water (or lava) stands over each of the tile's 256 * 64 cells,
// from MH2O; NaN where there is none. A riverbed is painted with the dirt the
// roads are, and only this tells the two apart. Empty without any liquid.
std::vector<float> liquidLevels(Bytes mh2o)
{
    std::vector<float> levels;
    const auto has = [&](std::size_t at, std::size_t size) { return at <= mh2o.size() && size <= mh2o.size() - at; };

    for (std::size_t chunkIndex = 0; chunkIndex < 256 && has(chunkIndex * 12, 12); ++chunkIndex)
    {
        const auto instances = get<std::uint32_t>(mh2o, chunkIndex * 12);
        const auto layers = get<std::uint32_t>(mh2o, chunkIndex * 12 + 4);

        for (std::uint32_t l = 0; l < layers && has(instances + std::size_t{l} * 24, 24); ++l)
        {
            const std::size_t at = instances + std::size_t{l} * 24;
            const auto format = get<std::uint16_t>(mh2o, at + 2);
            const float flat = get<float>(mh2o, at + 8);
            const int x0 = mh2o[at + 12], y0 = mh2o[at + 13], width = mh2o[at + 14], height = mh2o[at + 15];
            const auto exists = get<std::uint32_t>(mh2o, at + 16);
            const auto vertices = get<std::uint32_t>(mh2o, at + 20);

            if (x0 + width > 8 || y0 + height > 8 || !std::isfinite(flat))
            {
                continue;
            }

            // Heights at the cells' corners, where the layer has its own
            // (formats 0, 1 and 3); a level surface otherwise, the sea.
            const bool sloped = vertices != 0 && (format == 0 || format == 1 || format == 3) && has(vertices, static_cast<std::size_t>((width + 1) * (height + 1)) * 4);
            const bool masked = exists != 0 && has(exists, static_cast<std::size_t>(width * height + 7) / 8);

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const int bit = y * width + x;

                    if (masked && !((mh2o[exists + bit / 8] >> (bit % 8)) & 1))
                    {
                        continue;
                    }

                    float level = flat;

                    if (sloped)
                    {
                        level = get<float>(mh2o, vertices + static_cast<std::size_t>(y * (width + 1) + x) * 4);

                        if (!std::isfinite(level))
                        {
                            continue;
                        }
                    }

                    if (levels.empty())
                    {
                        levels.assign(256 * 64, std::numeric_limits<float>::quiet_NaN());
                    }

                    float& cell = levels[chunkIndex * 64 + static_cast<std::size_t>((y0 + y) * 8 + x0 + x)];
                    cell = std::isnan(cell) ? level : std::max(cell, level);
                }
            }
        }
    }

    return levels;
}

// Deeper than this over a cell's middle, the ground is a riverbed, not a road.
constexpr float kRoadUnderWater = 0.2f;
// Deeper than this, the character swims: the ground is a lake's bed.
constexpr float kSwimmingDepth = 1.5f;

} // namespace

std::vector<float> parseAdtWaterLevels(Bytes bytes)
{
    return liquidLevels(chunk(bytes, "MH2O"));
}

std::optional<float> waterLevelAt(const std::vector<float>& levels, int col, int row, float x, float y)
{
    constexpr float kAdt = 533.333333f;
    constexpr float kChunk = kAdt / 16;
    constexpr float kCell = kChunk / 8;

    if (levels.size() != 256 * 64)
    {
        return std::nullopt;
    }

    // From the tile's corner of greatest x and y: chunks and cells run
    // towards smaller x (rows) and smaller y (columns).
    const float fx = (32 - row) * kAdt - x;
    const float fy = (32 - col) * kAdt - y;

    if (fx < 0 || fy < 0 || fx >= kAdt || fy >= kAdt)
    {
        return std::nullopt;
    }

    const int chunkRow = static_cast<int>(fx / kChunk), chunkCol = static_cast<int>(fy / kChunk);
    const int cellRow = std::min(7, static_cast<int>((fx - chunkRow * kChunk) / kCell));
    const int cellCol = std::min(7, static_cast<int>((fy - chunkCol * kChunk) / kCell));
    const float level = levels[static_cast<std::size_t>((chunkRow * 16 + chunkCol) * 64 + cellRow * 8 + cellCol)];
    return std::isnan(level) ? std::nullopt : std::optional(level);
}

std::vector<std::uint32_t> parseAdtAreaIds(Bytes bytes)
{
    std::vector<std::uint32_t> areas;
    areas.reserve(256);
    chunks(bytes,
           [&](auto name, Bytes body)
           {
               // The chunk's header: its area at 0x34.
               if (name == "MCNK" && body.size() >= 128 && areas.size() < 256)
               {
                   areas.push_back(get<std::uint32_t>(body, 0x34));
               }
           });

    if (areas.size() != 256)
    {
        areas.clear();
    }

    return areas;
}

std::optional<std::uint32_t> areaIdAt(const std::vector<std::uint32_t>& areas, int col, int row, float x, float y)
{
    constexpr float kAdt = 533.333333f;
    constexpr float kChunk = kAdt / 16;

    if (areas.size() != 256)
    {
        return std::nullopt;
    }

    // As waterLevelAt: from the tile's corner of greatest x and y.
    const float fx = (32 - row) * kAdt - x;
    const float fy = (32 - col) * kAdt - y;

    if (fx < 0 || fy < 0 || fx >= kAdt || fy >= kAdt)
    {
        return std::nullopt;
    }

    const int chunkRow = std::min(15, static_cast<int>(fx / kChunk)), chunkCol = std::min(15, static_cast<int>(fy / kChunk));
    const std::uint32_t area = areas[static_cast<std::size_t>(chunkRow * 16 + chunkCol)];
    return area ? std::optional<std::uint32_t>{area} : std::nullopt;
}

CollisionGeometry parseAdtTerrain(Bytes bytes, Bytes roadCells, bool roadsOnly)
{
    CollisionGeometry geometry;
    const bool roads = roadCells.size() == 256 * 64;
    // The cells tagged road, and the first of each one's four triangles.
    std::vector<std::uint8_t> tagged_(roads && roadsOnly ? 256 * 64 : 0, 0);
    std::vector<std::size_t> firstOf(tagged_.size(), 0);
    const auto water = liquidLevels(chunk(bytes, "MH2O"));
    const bool tagged = roads || !water.empty();
    int chunkIndex = 0;
    const auto version = chunk(bytes, "MVER");

    if (version.size() != 4 || get<std::uint32_t>(version, 0) != 18)
    {
        throw std::runtime_error("unsupported ADT version");
    }

    chunks(bytes,
           [&](auto name, Bytes body)
           {
               if (name != "MCNK")
               {
                   return;
               }

               if (body.size() < 128)
               {
                   throw std::runtime_error("truncated terrain chunk");
               }

               const int thisChunk = chunkIndex++;
               const auto origin = point(body, 104);
               const auto flags = get<std::uint32_t>(body, 0);
               const auto holes = get<std::uint16_t>(body, 60);
               const auto heights = chunk(body.subspan(128), "MCVT");

               if (heights.size() != 145 * 4)
               {
                   throw std::runtime_error("missing terrain heights");
               }

               const int base = static_cast<int>(geometry.vertices.size());
               constexpr float cell = (533.333333f / 16) / 8;

               // ADT outer rows have 9 vertices; staggered inner rows have 8.
               for (int row = 0; row < 17; ++row)
               {
                   for (int col = 0; col < (row % 2 ? 8 : 9); ++col)
                   {
                       const int index = row / 2 * 17 + (row % 2 ? 9 : 0) + col;
                       const float h = get<float>(heights, index * 4);

                       if (!std::isfinite(h))
                       {
                           throw std::runtime_error("invalid terrain height");
                       }

                       geometry.vertices.push_back({origin.x - row * cell / 2, origin.y - (col + (row % 2 ? 0.5f : 0)) * cell, origin.z + h});
                   }
               }

               for (int r = 0; r < 8; ++r)
               {
                   for (int c = 0; c < 8; ++c)
                   {
                       const bool hole = (flags & 0x10000) ? ((body[20 + r] >> c) & 1) : ((holes >> (r / 2 * 4 + c / 2)) & 1);

                       if (hole)
                       {
                           continue;
                       }

                       const int a = base + r * 17 + c, b = a + 1, d = a + 17, e = d + 1, m = a + 9;
                       triangle(geometry, a, b, m);
                       triangle(geometry, b, e, m);
                       triangle(geometry, e, d, m);
                       triangle(geometry, d, a, m);

                       if (tagged)
                       {
                           const std::size_t cellIndex = static_cast<std::size_t>(thisChunk * 64 + r * 8 + c);
                           // The same dirt lies on the bed of a river: no road under water.
                           const float depth = thisChunk < 256 && !water.empty() ? water[cellIndex] - geometry.vertices[m].z : std::numeric_limits<float>::quiet_NaN();
                           const bool underWater = depth > kRoadUnderWater;
                           const std::uint8_t tag = depth > kSwimmingDepth ? kSwimmingGround : roads && thisChunk < 256 && !underWater && roadCells[cellIndex] ? kRoadGround : 0;
                           geometry.roads.resize(geometry.triangles.size() / 3 - 4, 0);

                           if (tag == kRoadGround && !tagged_.empty())
                           {
                               tagged_[cellIndex] = 1;
                               firstOf[cellIndex] = geometry.roads.size();
                           }

                           geometry.roads.insert(geometry.roads.end(), 4, tag);
                       }
                   }
               }
           });

    if (geometry.triangles.empty())
    {
        throw std::runtime_error("no terrain surface");
    }

    if (tagged)
    {
        geometry.roads.resize(geometry.triangles.size() / 3, 0);
    }

    if (!tagged_.empty())
    {
        const auto kept = keepRoads(tagged_);

        for (std::size_t cell = 0; cell < tagged_.size(); ++cell)
        {
            if (tagged_[cell] && (kept.empty() || !kept[cell]))
            {
                std::fill_n(geometry.roads.begin() + static_cast<std::ptrdiff_t>(firstOf[cell]), 4, std::uint8_t{0});
            }
        }
    }

    return geometry;
}

std::vector<MapObjectPlacement> parseAdtObjects(Bytes bytes)
{
    std::vector<MapObjectPlacement> out;
    chunks(bytes,
           [&](auto name, Bytes body)
           {
               if (name != "MODF" && name != "MDDF")
               {
                   return;
               }

               const bool wmo = name == "MODF";
               const std::size_t stride = wmo ? 64 : 36;

               if (body.size() % stride)
               {
                   throw std::runtime_error("invalid model placement size");
               }

               for (std::size_t at = 0; at < body.size(); at += stride)
               {
                   const auto flags = get<std::uint16_t>(body, at + (wmo ? 56 : 34));

                   if (!(flags & (wmo ? 8 : 64)))
                   {
                       throw std::runtime_error("named model references require legacy archive support");
                   }

                   const auto scale = get<std::uint16_t>(body, at + (wmo ? 62 : 32));
                   out.push_back({get<std::uint32_t>(body, at), get<std::uint32_t>(body, at + 4), point(body, at + 8), point(body, at + 20), scale ? scale / 1024.0f : 1.0f, wmo, wmo ? get<std::uint16_t>(body, at + 58) : std::uint16_t{0}});
               }
           });
    return out;
}

CollisionGeometry parseModelCollision(Bytes bytes)
{
    // MD21 contains the old MD20 header and arrays with offsets relative to it.
    Bytes data = bytes;

    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "MD21", 4) == 0)
    {
        const auto size = get<std::uint32_t>(bytes, 4);

        if (size > bytes.size() - 8)
        {
            throw std::runtime_error("truncated M2 collision model");
        }

        data = bytes.subspan(8, size);
    }

    if (data.size() < 240 || std::memcmp(data.data(), "MD20", 4) != 0)
    {
        throw std::runtime_error("unsupported M2 collision header");
    }

    const auto ni = get<std::uint32_t>(data, 216), io = get<std::uint32_t>(data, 220);
    const auto nv = get<std::uint32_t>(data, 224), vo = get<std::uint32_t>(data, 228);

    if (ni % 3 || nv > 1000000 || ni > 3000000 || vo > data.size() || nv > (data.size() - vo) / 12 || io > data.size() || ni > (data.size() - io) / 2)
    {
        throw std::runtime_error("invalid M2 collision arrays");
    }

    CollisionGeometry g;

    for (std::uint32_t i = 0; i < nv; ++i)
    {
        g.vertices.push_back(point(data, vo + i * 12));
    }

    for (std::uint32_t i = 0; i < ni; i += 3)
    {
        int a = get<std::uint16_t>(data, io + i * 2), b = get<std::uint16_t>(data, io + (i + 1) * 2), c = get<std::uint16_t>(data, io + (i + 2) * 2);

        if (a >= static_cast<int>(nv) || b >= static_cast<int>(nv) || c >= static_cast<int>(nv))
        {
            throw std::runtime_error("invalid M2 triangle");
        }

        triangle(g, a, b, c);
    }

    return g; // an empty collision mesh is legitimate (grass, effects, etc.)
}

namespace
{
// The furniture a world model places itself: its default doodad set (0) and
// `set`, each doodad a model at a position, a rotation (a quaternion) and a
// scale in the world model's own space. One that cannot be read is left out,
// as before, rather than losing the whole building.
CollisionGeometry worldModelDoodads(Bytes root, const ClientFileReader& read, std::uint16_t set)
{
    CollisionGeometry out;
    const auto sets = chunk(root, "MODS"), files = chunk(root, "MODI"), doodads = chunk(root, "MODD");

    if (sets.empty() || files.empty() || doodads.empty() || sets.size() % 32 || files.size() % 4 || doodads.size() % 40)
    {
        return out;
    }

    std::map<std::uint32_t, CollisionGeometry> models;
    const auto place = [&](std::uint32_t index)
    {
        const std::size_t at = index * 40;

        if (at + 40 > doodads.size())
        {
            return;
        }

        const auto name = get<std::uint32_t>(doodads, at) & 0xFFFFFF;

        if ((name + 1) * 4 > files.size())
        {
            return;
        }

        const auto file = get<std::uint32_t>(files, name * 4);

        if (!file)
        {
            return;
        }

        auto model = models.find(file);

        if (model == models.end())
        {
            CollisionGeometry collision;

            try
            {
                const auto bytes = read(file);

                if (!bytes.empty())
                {
                    collision = parseModelCollision(bytes);
                }
            }
            catch (const std::exception&)
            {
            }

            model = models.emplace(file, std::move(collision)).first;
        }

        if (model->second.triangles.empty())
        {
            return;
        }

        const Vec3 position = point(doodads, at + 4);
        const float qx = get<float>(doodads, at + 16), qy = get<float>(doodads, at + 20), qz = get<float>(doodads, at + 24), qw = get<float>(doodads, at + 28);
        const float scale = get<float>(doodads, at + 32);

        if (!std::isfinite(qx + qy + qz + qw + scale) || scale <= 0)
        {
            return;
        }

        CollisionGeometry placed = model->second;

        for (auto& v : placed.vertices)
        {
            const Vec3 p = v * scale;
            // p rotated by the unit quaternion (qx, qy, qz, qw).
            const Vec3 u{qx, qy, qz};
            const Vec3 t{2 * (u.y * p.z - u.z * p.y), 2 * (u.z * p.x - u.x * p.z), 2 * (u.x * p.y - u.y * p.x)};
            const Vec3 r = p + t * qw + Vec3{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
            v = r + position;
        }

        out.append(placed);
    };
    // The default set, and the chosen one when it is another.
    std::vector<std::uint16_t> chosen{0};

    if (set != 0)
    {
        chosen.push_back(set);
    }

    for (const std::uint16_t s : chosen)
    {
        if ((s + 1u) * 32u > sets.size())
        {
            continue;
        }

        const auto first = get<std::uint32_t>(sets, s * 32 + 20), count = get<std::uint32_t>(sets, s * 32 + 24);

        for (std::uint32_t i = 0; i < count && i < 100000; ++i)
        {
            place(first + i);
        }
    }

    return out;
}

} // namespace
CollisionGeometry parseWorldModelCollision(Bytes root, const ClientFileReader& read, std::uint16_t doodadSet)
{
    const auto ids = chunk(root, "GFID");

    if (ids.empty() || ids.size() % 4)
    {
        throw std::runtime_error("missing WMO group file IDs");
    }

    const auto header = chunk(root, "MOHD");
    const auto groupCount = get<std::uint32_t>(header, 4);

    if (groupCount > ids.size() / 4)
    {
        throw std::runtime_error("truncated WMO group IDs");
    }

    CollisionGeometry geometry;

    for (std::uint32_t i = 0; i < groupCount; ++i)
    {
        const auto bytes = read(get<std::uint32_t>(ids, i * 4));

        if (bytes.empty())
        {
            throw std::runtime_error("missing WMO collision group");
        }

        const auto group = chunk(bytes, "MOGP");

        if (group.size() < 68)
        {
            throw std::runtime_error("invalid WMO group header");
        }

        const auto subs = group.subspan(68), vertices = chunk(subs, "MOVT"), indices = chunk(subs, "MOVI");

        if (vertices.size() % 12 || indices.size() % 6)
        {
            throw std::runtime_error("invalid WMO geometry arrays");
        }

        CollisionGeometry g;

        for (std::size_t j = 0; j < vertices.size(); j += 12)
        {
            g.vertices.push_back(point(vertices, j));
        }

        const auto collisionRefs = chunk(subs, "MOBR");
        const auto materials = chunk(subs, "MOPY");
        std::vector<bool> collidable(indices.size() / 6, false);

        if (!collisionRefs.empty())
        {
            if (collisionRefs.size() % 2)
            {
                throw std::runtime_error("invalid WMO collision references");
            }

            for (std::size_t j = 0; j < collisionRefs.size(); j += 2)
            {
                auto t = get<std::uint16_t>(collisionRefs, j);

                if (t >= collidable.size())
                {
                    throw std::runtime_error("invalid WMO collision triangle reference");
                }

                collidable[t] = true;
            }
        }
        else
        {
            for (std::size_t t = 0; t < collidable.size(); ++t)
            {
                if (materials.size() < (t + 1) * 2)
                {
                    throw std::runtime_error("missing WMO triangle materials");
                }

                const auto f = materials[t * 2];
                collidable[t] = (f & 0x08) || ((f & 0x20) && !(f & 0x04)) || materials[t * 2 + 1] == 0xff;
            }
        }

        for (std::size_t j = 0; j < indices.size(); j += 6)
        {
            if (!collidable[j / 6])
            {
                continue;
            }

            int a = get<std::uint16_t>(indices, j), b = get<std::uint16_t>(indices, j + 2), c = get<std::uint16_t>(indices, j + 4);

            if (a >= static_cast<int>(g.vertices.size()) || b >= static_cast<int>(g.vertices.size()) || c >= static_cast<int>(g.vertices.size()))
            {
                throw std::runtime_error("invalid WMO triangle");
            }

            triangle(g, a, b, c);
        }

        geometry.append(g);
    }

    geometry.append(worldModelDoodads(root, read, doodadSet));
    return geometry;
}

CollisionGeometry placeModel(const CollisionGeometry& geometry, const MapObjectPlacement& placement)
{
    CollisionGeometry out = geometry;
    constexpr float origin = 17066.666667f;
    // ADT rotations are around the client's X/Y/Z axes; yaw has a 180 degree
    // placement offset. Convert from model space to server world coordinates.
    const float rx = placement.rotation.z * std::numbers::pi_v<float> / 180;
    const float ry = placement.rotation.x * std::numbers::pi_v<float> / 180;
    const float rz = (placement.rotation.y - 180) * std::numbers::pi_v<float> / 180;

    for (auto& p : out.vertices)
    {
        p = p * placement.scale;
        const float y = p.y * std::cos(rx) - p.z * std::sin(rx), z = p.y * std::sin(rx) + p.z * std::cos(rx);
        p.y = y;
        p.z = z;
        const float x = p.x * std::cos(ry) + p.z * std::sin(ry), zz = -p.x * std::sin(ry) + p.z * std::cos(ry);
        p.x = x;
        p.z = zz;
        const float xx = p.x * std::cos(rz) - p.y * std::sin(rz), yy = p.x * std::sin(rz) + p.y * std::cos(rz);
        p = {xx + origin - placement.position.z, yy + origin - placement.position.x, p.z + placement.position.y};
    }

    return out;
}

} // namespace og
