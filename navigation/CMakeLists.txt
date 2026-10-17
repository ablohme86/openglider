# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(navigation
    SOURCES
        WaypointRoute.cpp
        RouteCorrector.cpp
        ObstacleMemory.cpp
        Roaming.cpp
        MovementController.cpp
        StuckDetector.cpp
        StuckRecovery.cpp
        Navigator.cpp
        Lifts.cpp
        Avoidance.cpp
        ClientGeometry.cpp
    DEPS
        og_common
        og_state
        og_actions)
if(OG_WITH_PATHFINDING)
    target_sources(og_navigation PRIVATE ClientNavigationProvider.cpp ClientMapSource.cpp)
    target_link_libraries(og_navigation PRIVATE Recast Detour)
    target_compile_definitions(og_navigation PUBLIC OG_HAS_CLIENT_PATHFINDING=1)
    if(OG_HAVE_CASCLIB)
        target_link_libraries(og_navigation PRIVATE og_minimap)
        target_compile_definitions(og_navigation PRIVATE OG_CLIENT_MAP_CASC=1)
    endif()
endif()
