# SPDX-License-Identifier: LGPL-3.0-or-later
set(OG_INPUT_SOURCES
    Key.cpp
    IInputBackend.cpp
    VirtualInputBackend.cpp
    InputBackendFactory.cpp)

if(WIN32)
    list(APPEND OG_INPUT_SOURCES Win32InputBackend.cpp)
else()
    list(APPEND OG_INPUT_SOURCES WineBridgeInputBackend.cpp UInputBackend.cpp)
endif()

if(OG_HAVE_X11)
    list(APPEND OG_INPUT_SOURCES X11InputBackend.cpp)
endif()

og_add_module(input
    SOURCES ${OG_INPUT_SOURCES}
    DEPS og_common)

if(WIN32)
    target_link_libraries(og_input PRIVATE user32)
endif()

if(OG_HAVE_X11)
    target_compile_definitions(og_input PUBLIC OG_HAVE_X11=1)
    target_link_libraries(og_input PRIVATE X11::X11)
endif()
