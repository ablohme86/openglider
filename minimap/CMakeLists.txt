# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(minimap
    SOURCES
        Blp.cpp
        MinimapTiles.cpp
    DEPS
        og_common)

# Each storage format comes with its library: MPQ for 1.12.1 and 3.3.5a, CASC
# for the 64-bit clients. OG_MINIMAP_MPQ / OG_MINIMAP_CASC say which were built.
if(OG_HAVE_STORMLIB)
    target_sources(og_minimap PRIVATE MpqMinimap.cpp)
    target_link_libraries(og_minimap PRIVATE ${OG_STORM_TARGET})
    target_compile_definitions(og_minimap PUBLIC OG_MINIMAP_MPQ=1)
endif()
if(OG_HAVE_CASCLIB)
    target_sources(og_minimap PRIVATE CascMinimap.cpp)
    target_link_libraries(og_minimap PRIVATE ${OG_CASC_TARGET})
    target_compile_definitions(og_minimap PUBLIC OG_MINIMAP_CASC=1)
endif()
