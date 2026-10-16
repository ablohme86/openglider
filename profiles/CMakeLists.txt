# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(profiles
    SOURCES
        GlobalObstacles.cpp
        ProfileLoader.cpp
        ProfileRecorder.cpp
    DEPS
        og_common
        og_navigation)
