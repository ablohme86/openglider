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
 *  File: EngineAbi.h
 *
 *  Desc:
 *  Whether the engine a program runs with (a shared libopenglider /
 *  OpenGlider.dll, perhaps one a user built) fits the headers the
 *  program was built with.
 *
 *
 */
#pragma once

#include "og/core/BotConfig.h"
#include "og/core/BotContext.h"
#include "og/core/BotEngine.h"
#include "og/core/BotInstance.h"
#include "og/profiles/GliderProfile.h"
#include "og/profiles/ProfileEditor.h"
#include "og/state/GameSnapshot.h"
#include "og/world/QuestSource.h"
#include "og/wowclient/ClientLayout.h"
#include "og/wowclient/ModernLayout.h"
#include "og/wowclient/WowMemoryStateProvider.h"

#include <cstdint>
#include <initializer_list>
#include <string>

namespace og
{

// Bumped by hand on a change programs must be built again for that the sizes
// below do not show: a virtual function added, a field moved.
inline constexpr std::uint32_t kEngineAbiVersion = 1;

// The size and alignment of each type a program shares with the engine, as
// the headers at hand make them. A field added to one changes it.
template <typename... Types>
constexpr std::uint64_t layoutFingerprint()
{
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a

    for (const std::uint64_t value : {std::uint64_t{kEngineAbiVersion}, (std::uint64_t{sizeof(Types)} << 8 | std::uint64_t{alignof(Types)})...})
    {
        for (int i = 0; i < 8; ++i)
        {
            hash ^= (value >> (i * 8)) & 0xFF;
            hash *= 1099511628211ull;
        }
    }

    return hash;
}

constexpr std::uint64_t engineAbiFingerprint()
{
    return layoutFingerprint<GameSnapshot, PlayerState, UnitState, GameObjectState, BotConfig, BotContext, BotEngine, BotInstance, InstanceStatus, GliderProfile, ProfileEditor,
                             QuestSource, wowclient::ClientLayout, wowclient::ModernLayout, wowclient::WowMemoryStateProvider>();
}

// What the engine running says of itself.
struct EngineInfo
{
    std::uint32_t abiVersion = 0;
    std::uint64_t abiFingerprint = 0;
    std::string buildId;  // the build it came out of; empty for one built on its own
    std::string location; // the file it was loaded from (the program itself, built statically)
};

EngineInfo engineInfo();

// How the engine running fits a program built with these headers and, when
// it has one, this build id.
struct EngineCheck
{
    bool compatible = true; // false: the program must not run with it
    bool custom = false;    // compatible, but not the build the program came with
    std::string message;
};

inline EngineCheck checkEngine(std::string_view programBuildId)
{
    // At compile time, from the program's headers (an inline function called
    // at run time may be the engine's own copy).
    constexpr std::uint64_t expected = engineAbiFingerprint();
    const EngineInfo info = engineInfo();
    EngineCheck check;

    if (info.abiVersion != kEngineAbiVersion || info.abiFingerprint != expected)
    {
        check.compatible = false;
        check.message = "the OpenGlider engine in " + info.location + " was built from other OpenGlider headers than this program (ABI " +
                        std::to_string(info.abiVersion) + "/" + std::to_string(info.abiFingerprint) + ", this program needs " + std::to_string(kEngineAbiVersion) + "/" +
                        std::to_string(expected) + "). Build it again from the OpenGlider sources this version names, or remove it.";
        return check;
    }

    check.custom = !programBuildId.empty() && info.buildId != programBuildId;

    if (check.custom)
    {
        check.message = "running a custom OpenGlider engine: " + info.location;
    }

    return check;
}

} // namespace og
