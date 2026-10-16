# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(wowclient
    SOURCES
        ProcessMemory.cpp
        ClientLayouts.cpp
        LayoutJson.cpp
        ClientVersion.cpp
        ChatLog.cpp
        QuestCounts.cpp
        FactionTemplates256.cpp
        GameObjectKind.cpp
        ZoneMaps.cpp
        Layout335a.cpp
        Layout1121.cpp
        BobberProbe.cpp
        ModernClientReader.cpp
        WowBindings.cpp
        SpellNames.cpp
        SpellTooltip.cpp
        ItemNames.cpp
        LuaExecutor.cpp
        WowMemoryStateProvider.cpp
    DEPS
        og_common
        og_state
    PRIVATE_DEPS
        ${OG_YAML_TARGET})

# The 64-bit client layouts are not part of OpenGlider: a build that has them
# adds their sources to og_wowclient and defines OG_EMBED_MODERN_LAYOUTS;
# otherwise they are registered at run time (registerLayout).
