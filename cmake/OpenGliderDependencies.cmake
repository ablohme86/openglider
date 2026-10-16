# SPDX-License-Identifier: LGPL-3.0-or-later
#
# The engine's third-party dependencies. System packages are preferred;
# FetchContent is the fallback so it also builds on Windows without a package
# manager. Included by OpenGlider's own build, or by a program's before
# add_subdirectory(og).
include(FetchContent)

# CMake 4 dropped support for projects that ask for compatibility with CMake
# older than 3.5, and CMake 3.31+ warns for projects asking for CMake < 3.10,
# which some of the fetched dependencies below still do (casclib, yaml-cpp).
# Subdirectories inherit this, so it relaxes them without pinning any of them
# to an unreleased commit.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

find_package(Threads REQUIRED)

# --- SQLite (data/world.db: quests, NPCs, zones, spell names) ---------------
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    message(STATUS "SQLite3 not found on system, fetching")
    enable_language(C) # the amalgamation is the one C source here
    FetchContent_Declare(sqlite3_amalgamation
        URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
        URL_HASH SHA3_256=f131b68e6ba5fb891cc13ebb5ff9555054c77294cb92d8d1268bad5dba4fa2a1)
    FetchContent_MakeAvailable(sqlite3_amalgamation)
    add_library(og_sqlite3 STATIC ${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c)
    target_include_directories(og_sqlite3 SYSTEM PUBLIC ${sqlite3_amalgamation_SOURCE_DIR})
    target_compile_definitions(og_sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION)
    target_link_libraries(og_sqlite3 PRIVATE Threads::Threads)
    add_library(SQLite3::SQLite3 ALIAS og_sqlite3)
    add_library(SQLite::SQLite3 ALIAS og_sqlite3)
else()
    # CMake < 4.3 (such as Ubuntu LTS runners with CMake 3.x) provides SQLite::SQLite3
    # rather than SQLite3::SQLite3. Ensure SQLite3::SQLite3 target always exists.
    if(NOT TARGET SQLite3::SQLite3)
        if(TARGET SQLite::SQLite3)
            add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
        elseif(SQLite3_LIBRARIES)
            add_library(SQLite3::SQLite3 UNKNOWN IMPORTED)
            set_target_properties(SQLite3::SQLite3 PROPERTIES
                IMPORTED_LOCATION "${SQLite3_LIBRARIES}"
                INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}")
        endif()
    endif()
endif()

# --- yaml-cpp (actions.yaml, config.yaml, task scripts) ---------------------
find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
    message(STATUS "yaml-cpp not found on system, fetching")
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG 0.8.0)
    FetchContent_MakeAvailable(yaml-cpp)
    # yaml-cpp 0.8.0 uses uint16_t without including <cstdint>; newer
    # standard libraries no longer provide it through unrelated headers.
    if(MSVC)
        target_compile_options(yaml-cpp PRIVATE /FIcstdint)
    else()
        target_compile_options(yaml-cpp PRIVATE -include cstdint)
    endif()
endif()
if(TARGET yaml-cpp::yaml-cpp)
    set(OG_YAML_TARGET yaml-cpp::yaml-cpp)
else()
    set(OG_YAML_TARGET yaml-cpp)
endif()

# --- StormLib (reads the client's MPQ archives for minimap tiles) -------------
# Optional: without it the minimap background is simply unavailable and
# everything else builds as before.
set(OG_HAVE_STORMLIB OFF)
if(OG_WITH_MINIMAP)
    find_package(StormLib CONFIG QUIET)
    if(NOT StormLib_FOUND)
        message(STATUS "StormLib not found on system, fetching")
        set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(STORM_UNICODE OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(stormlib
            GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib.git
            GIT_TAG v9.25)
        FetchContent_MakeAvailable(stormlib)
    endif()
    if(TARGET storm)
        set(OG_STORM_TARGET storm)
        set(OG_HAVE_STORMLIB ON)
    elseif(TARGET StormLib::storm)
        set(OG_STORM_TARGET StormLib::storm)
        set(OG_HAVE_STORMLIB ON)
    else()
        message(STATUS "StormLib unavailable - minimap background disabled")
    endif()
endif()

# --- CascLib (reads the CASC storage of the 64-bit clients for minimap tiles) ---
# Optional like StormLib: without it only the MPQ clients get a minimap.
set(OG_HAVE_CASCLIB OFF)
if(OG_WITH_MINIMAP)
    find_package(CascLib CONFIG QUIET)
    if(NOT CascLib_FOUND)
        message(STATUS "CascLib not found on system, fetching")
        set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
        set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
        set(CASC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(CASC_UNICODE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(casclib
            GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
            GIT_TAG 2a280f5a231966dc5d1b534978dd9f9f04a374cd)
        FetchContent_MakeAvailable(casclib)
        # CascLib and StormLib share internal stream type names but have
        # incompatible layouts. Keep their C++ type identities distinct when
        # both static libraries participate in release link-time optimisation.
        # These types are internal; the public CascLib API uses opaque handles.
        target_compile_definitions(casc_static PRIVATE
            TBaseProviderData=OgCascBaseProviderData
            TFileStream=OgCascFileStream
            TBlockStream=OgCascBlockStream
            TEncryptedStream=OgCascEncryptedStream)
    endif()
    if(TARGET casc_static)
        set(OG_CASC_TARGET casc_static)
        set(OG_HAVE_CASCLIB ON)
    elseif(TARGET CascLib::casc_static)
        set(OG_CASC_TARGET CascLib::casc_static)
        set(OG_HAVE_CASCLIB ON)
    elseif(TARGET CascLib::casc)
        set(OG_CASC_TARGET CascLib::casc)
        set(OG_HAVE_CASCLIB ON)
    else()
        message(STATUS "CascLib unavailable - no minimap for the 64-bit clients")
    endif()
endif()

# --- Recast/Detour (client-map pathfinding) --------------------------------------
if(OG_WITH_PATHFINDING)
    set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
    set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
    set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(recastnavigation
        GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
        GIT_TAG v1.6.0)
    FetchContent_MakeAvailable(recastnavigation)
endif()

# --- X11 (optional background input backend on Linux) -------------------------
set(OG_HAVE_X11 OFF)
if(OG_WITH_X11 AND UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND)
        set(OG_HAVE_X11 ON)
    else()
        message(STATUS "X11 not found - X11InputBackend disabled")
    endif()
endif()
