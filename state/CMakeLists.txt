# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(state
    SOURCES
        Types.cpp
        AreaTable.cpp
        GameSnapshot.cpp
        SnapshotJson.cpp
        SpecialBags.cpp
        Shapeshift.cpp
    DEPS
        og_common)
