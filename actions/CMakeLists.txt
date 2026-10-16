# SPDX-License-Identifier: LGPL-3.0-or-later
# Keep the shipped action metadata available without an external config file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/data/actions.yaml")
file(READ "${PROJECT_SOURCE_DIR}/data/actions.yaml" OG_DEFAULT_ACTIONS)
configure_file(DefaultActions.h.in "${CMAKE_CURRENT_BINARY_DIR}/DefaultActions.h" @ONLY)

og_add_module(actions
    SOURCES
        ActionBinding.cpp
        ActionCooldownTracker.cpp
        ActionService.cpp
        ActionConfig.cpp
        SpellCosts.cpp
    DEPS
        og_common
        og_input
    PRIVATE_DEPS
        ${OG_YAML_TARGET})

target_include_directories(og_actions PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
