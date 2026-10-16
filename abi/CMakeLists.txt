# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(abi
    SOURCES
        EngineAbi.cpp
    DEPS
        og_core
        og_world
        og_wowclient)

# Which build of the engine this is: the one a program was built with, or
# another put in its place (OG_ENGINE_BUILD_ID, set by the program's build).
if(OG_ENGINE_BUILD_ID)
    target_compile_definitions(og_abi PRIVATE OG_ENGINE_BUILD_ID="${OG_ENGINE_BUILD_ID}")
endif()

if(UNIX)
    target_link_libraries(og_abi PRIVATE ${CMAKE_DL_LIBS})
endif()
