# SPDX-License-Identifier: LGPL-3.0-or-later
#
# OpenGlider's build helpers, for its own build and for a program that builds
# it as a subdirectory (which includes this file before add_subdirectory).

# OG_INCLUDE_ROOT: where "og/<module>/Header.h" is found from, the directory
# that holds og/. Set before this file is included: a parent project that has
# OpenGlider at src/og sets src/; OpenGlider's own build links og/ to itself.

# Common compile settings shared by every target.
function(og_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(OG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-missing-field-initializers)
        if(OG_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# og_add_module(<name> SOURCES ... [DEPS ...] [PRIVATE_DEPS ...])
#
# Creates library og_<name> of the engine (OpenGlider) rooted at src/ so
# headers are included as "og/<module>/Header.h". Dependencies express the
# layering of the engine: a module can only see the modules it explicitly
# lists. A static library; with OG_SHARED_ENGINE, an object library that goes
# into the one shared libopenglider. Programs link og_engine, never a module.
function(og_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
    set(target og_${name})

    if(OG_SHARED_ENGINE)
        add_library(${target} OBJECT ${ARG_SOURCES})
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET default VISIBILITY_INLINES_HIDDEN OFF)
    else()
        add_library(${target} STATIC ${ARG_SOURCES})
    endif()

    set_property(GLOBAL APPEND PROPERTY OG_ENGINE_MODULES ${target})
    add_library(AllGlider::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC ${OG_INCLUDE_ROOT})
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(ARG_DEPS)
        target_link_libraries(${target} PUBLIC ${ARG_DEPS})
    endif()
    if(ARG_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()
    og_set_warnings(${target})
endfunction()
