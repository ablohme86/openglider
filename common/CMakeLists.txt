# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(common
    SOURCES
        Log.cpp
        Json.cpp
        FileUtil.cpp
        Db2.cpp
        Sqlite.cpp
    DEPS
        Threads::Threads
        SQLite3::SQLite3)

# Release packages use "data"; development builds also search the source tree.
set(OG_DATA_DIR "${PROJECT_SOURCE_DIR}/data" CACHE STRING "Default data directory")
target_compile_definitions(og_common PRIVATE OG_DATA_DIR="${OG_DATA_DIR}")

# The application OpenGlider is built into, which names its config and runtime
# directories; a parent project sets it before add_subdirectory(og).
if(NOT DEFINED OG_APP_NAME)
    set(OG_APP_NAME "OpenGlider")
endif()
target_compile_definitions(og_common PRIVATE OG_APP_NAME="${OG_APP_NAME}")
