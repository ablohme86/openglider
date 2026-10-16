# SPDX-License-Identifier: LGPL-3.0-or-later
#
# OpenGlider: the bot engine, as static libraries og_<module>. Added from the
# parent's src/CMakeLists.txt (add_subdirectory(og)), which provides
# og_add_module and the dependencies (yaml-cpp, Recast/Detour, SQLite,
# StormLib/CascLib, OpenSSL).
#
# Module layering (a module may only depend on modules above it):
#
#   common      math, clock, logging, events, json, blacklists
#   state       GameSnapshot, IGameStateProvider, mock provider
#   input       IInputBackend + backends
#   actions     semantic actions -> input
#   navigation  waypoint routes, movement controller, stuck handling
#   targets     target filtering/scoring
#   profiles    Glider profiles, loader/validator/recorder
#   combat      combat class interface + OGScript language
#   core        BotEngine, behaviors, objective stack, BotInstance
#   replay      snapshot recording and replay provider
#   sim         simulated world for offline testing
#   wowclient   WoW client memory state provider (3.3.5a, 1.12.1 layouts)
add_subdirectory(common)
add_subdirectory(state)
add_subdirectory(input)
add_subdirectory(actions)
add_subdirectory(navigation)
add_subdirectory(targets)
add_subdirectory(profiles)
add_subdirectory(combat)
add_subdirectory(core)
add_subdirectory(tasks)
add_subdirectory(world)
add_subdirectory(ai)
add_subdirectory(replay)
add_subdirectory(sim)
add_subdirectory(wowclient)
if(OG_HAVE_STORMLIB OR OG_HAVE_CASCLIB)
    add_subdirectory(minimap)
endif()
