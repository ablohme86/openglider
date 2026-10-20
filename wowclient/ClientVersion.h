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
 *  File: ClientVersion.h
 *
 *  Desc:
 *  Declarations and interface for ClientVersion (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace og::wowclient
{

class IProcessMemory;

// Version of the running WoW.exe, from the VS_FIXEDFILEINFO in its version
// resource (e.g. 1.12.1.5875, 3.3.5.12340).
struct ClientVersion
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::uint32_t build = 0;
    bool isChina = false;
    std::uint32_t realBuild = 0;

    std::string toString() const;
};

// Reads the version resource of the PE image mapped at imageBase. Works the
// same for a native process and for WoW.exe under wine, and needs no
// build-specific addresses.
std::optional<ClientVersion> readClientVersion(const IProcessMemory& memory, std::uint64_t imageBase);

// Where the client's image is mapped: the main module's base where the
// process reports it (Windows loads a 64-bit client where ASLR puts it), else
// 0x400000 for the 32-bit clients, 0x140000000 (the default base of a 64-bit
// PE, which wine keeps) for the 64-bit ones. 0x400000 when neither has an MZ
// header.
std::uint64_t findImageBase(const IProcessMemory& memory);

} // namespace og::wowclient
