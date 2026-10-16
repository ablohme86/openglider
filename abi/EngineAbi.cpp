/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-16
 *  File: EngineAbi.cpp
 *
 *  Desc:
 *  What the engine says of itself: the ABI it was built with, its
 *  build, and the file it was loaded from.
 *
 *
 */
#include "og/abi/EngineAbi.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace og
{

namespace
{

std::string ownLocation()
{
#ifdef _WIN32
    HMODULE module = nullptr;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&ownLocation), &module))
    {
        char path[MAX_PATH] = {};

        if (GetModuleFileNameA(module, path, MAX_PATH) > 0)
        {
            return path;
        }
    }
#else
    Dl_info info{};

    if (dladdr(reinterpret_cast<void*>(&ownLocation), &info) && info.dli_fname)
    {
        return info.dli_fname;
    }
#endif
    return "?";
}

} // namespace

EngineInfo engineInfo()
{
    EngineInfo info;
    info.abiVersion = kEngineAbiVersion;
    // Worked out here, at compile time, with the headers this was built from:
    // a call at run time could go to the program's copy of the function.
    constexpr std::uint64_t fingerprint = engineAbiFingerprint();
    info.abiFingerprint = fingerprint;
#ifdef OG_ENGINE_BUILD_ID
    info.buildId = OG_ENGINE_BUILD_ID;
#endif
    info.location = ownLocation();
    return info;
}

} // namespace og
