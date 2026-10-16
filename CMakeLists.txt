# SPDX-License-Identifier: LGPL-3.0-or-later
#
# OpenGlider: the bot engine. Built on its own (cmake -S . -B build), it is one
# shared library, libopenglider.so / OpenGlider.dll, that a program built with
# the same headers can run with in place of its own. Added by a program with
# add_subdirectory(og) instead, after it includes cmake/OpenGliderHelpers.cmake
# and cmake/OpenGliderDependencies.cmake, its modules are static libraries
# og_<module> - or, with OG_SHARED_ENGINE, the same shared library.
#
# Module layering (a module may only depend on modules above it):
#
#   common      math, clock, logging, events, json, blacklists
#   state       GameSnapshot, IGameStateProvider, mock provider
#   input       IInputBackend + backends
#   actions     semantic actions -> input
#   navigation  waypoint routes, movement controller, stuck handling
#   targets     target filtering/scoring
#   profiles    profiles: loader, validator, editor
#   combat      combat class interface + OGScript language
#   core        BotEngine, behaviors, objective stack, BotInstance
#   replay      snapshot recording and replay provider
#   sim         simulated world for offline testing
#   wowclient   WoW client memory state provider (3.3.5a, 1.12.1 layouts)
#   abi         whether the engine running fits the program's headers
cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(OpenGlider
        DESCRIPTION "Bot engine for World of Warcraft clients"
        LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    option(OG_WITH_X11 "Build the X11 input backend (Linux)" ON)
    option(OG_WITH_MINIMAP "Read the client's minimap tiles" ON)
    option(OG_WITH_PATHFINDING "Build client-map pathfinding" ON)
    option(OG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
    # On its own, OpenGlider is the shared library and nothing else.
    set(OG_SHARED_ENGINE ON)

    # The sources include "og/<module>/...": og/ is this checkout, whatever
    # its directory is called.
    set(OG_INCLUDE_ROOT "${CMAKE_BINARY_DIR}/include")
    file(MAKE_DIRECTORY "${OG_INCLUDE_ROOT}")
    file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}" "${OG_INCLUDE_ROOT}/og" SYMBOLIC RESULT og_link_result)

    if(NOT og_link_result STREQUAL "0")
        message(FATAL_ERROR "cannot link ${OG_INCLUDE_ROOT}/og to the sources (${og_link_result}); on Windows, turn on Developer Mode")
    endif()

    include(cmake/OpenGliderHelpers.cmake)
    include(cmake/OpenGliderDependencies.cmake)
endif()

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
# Last: it sees every module's types.
add_subdirectory(abi)

# The one shared library the modules make with OG_SHARED_ENGINE (always, on
# its own): libopenglider.so / OpenGlider.dll.
if(OG_SHARED_ENGINE)
    get_property(og_engine_modules GLOBAL PROPERTY OG_ENGINE_MODULES)
    add_library(openglider SHARED)
    target_link_libraries(openglider PUBLIC ${og_engine_modules})
    set_target_properties(openglider PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

    if(WIN32)
        set_target_properties(openglider PROPERTIES OUTPUT_NAME OpenGlider)
    endif()
endif()
