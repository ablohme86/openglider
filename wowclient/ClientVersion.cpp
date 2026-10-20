/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: ClientVersion.cpp
 *
 *  Desc:
 *  Implementation of ClientVersion (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ClientVersion.h"

#include "og/wowclient/ClientLayout.h"
#include "og/wowclient/ProcessMemory.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace og::wowclient
{

std::string ClientVersion::toString() const
{
    return std::format("{}.{}.{}.{}", major, minor, patch, realBuild > 0 ? realBuild : build);
}

std::uint64_t findImageBase(const IProcessMemory& memory)
{
    // Where the process says its .exe is (Windows, with ASLR).
    if (const std::uint64_t base = memory.mainModuleBase(); base != 0 && memory.read<std::uint16_t>(base).value_or(0) == 0x5A4D)
    {
        return base;
    }

    for (const std::uint64_t base : {std::uint64_t{consts::ImageBase}, consts::ImageBase64})
    {
        if (memory.read<std::uint16_t>(base).value_or(0) == 0x5A4D)
        {
            return base; // "MZ"
        }
    }

    return consts::ImageBase;
}

std::optional<ClientVersion> readClientVersion(const IProcessMemory& memory, std::uint64_t imageBase)
{
    // DOS header -> PE header -> section table; the version info lives in .rsrc.
    const auto lfanew = memory.read<std::uint32_t>(imageBase + 0x3C);

    if (!lfanew || *lfanew > 0x1000)
    {
        return std::nullopt;
    }

    const std::uint64_t pe = imageBase + *lfanew;

    if (memory.read<std::uint32_t>(pe).value_or(0) != 0x00004550)
    {
        return std::nullopt; // "PE\0\0"
    }

    const auto sections = memory.read<std::uint16_t>(pe + 6).value_or(0);
    const auto optionalSize = memory.read<std::uint16_t>(pe + 20).value_or(0);
    const std::uint64_t table = pe + 24 + optionalSize;

    for (std::uint16_t i = 0; i < std::min<std::uint16_t>(sections, 96); ++i)
    {
        std::uint8_t header[40] = {};

        if (!memory.read(table + i * 40u, header, sizeof(header)))
        {
            return std::nullopt;
        }

        if (std::memcmp(header, ".rsrc", 5) != 0)
        {
            continue;
        }

        std::uint32_t virtualSize = 0, virtualAddress = 0;
        std::memcpy(&virtualSize, header + 8, 4);
        std::memcpy(&virtualAddress, header + 12, 4);
        std::vector<std::uint8_t> rsrc(std::min<std::uint32_t>(virtualSize, 16u << 20));

        if (rsrc.size() < 64 || !memory.read(imageBase + virtualAddress, rsrc.data(), rsrc.size()))
        {
            return std::nullopt;
        }

        // VS_FIXEDFILEINFO: signature, struct version, file version MS, file version LS.
        for (std::size_t off = 0; off + 16 <= rsrc.size(); off += 4)
        {
            std::uint32_t signature = 0;
            std::memcpy(&signature, rsrc.data() + off, 4);

            if (signature != 0xFEEF04BD)
            {
                continue;
            }

            std::uint32_t ms = 0, ls = 0;
            std::memcpy(&ms, rsrc.data() + off + 8, 4);
            std::memcpy(&ls, rsrc.data() + off + 12, 4);
            const int major = static_cast<int>(ms >> 16);

            // The 64-bit Classic clients spread their digits over the 16-bit
            // fields, because a five-digit build does not fit one: 2.5.6.69795
            // is stored as 205.6.6979.5.
            const std::uint32_t rawBuild = (major >= 100)
                ? static_cast<std::uint32_t>((ls >> 16) * 10 + (ls & 0xFFFF))
                : static_cast<std::uint32_t>(ls & 0xFFFF);

            if (major >= 100)
            {
                return ClientVersion{major / 100, major % 100, static_cast<int>(ms & 0xFFFF), rawBuild, false, rawBuild};
            }

            return ClientVersion{major, static_cast<int>(ms & 0xFFFF), static_cast<int>(ls >> 16), rawBuild, false, rawBuild};
        }

        return std::nullopt;
    }

    return std::nullopt;
}

} // namespace og::wowclient
