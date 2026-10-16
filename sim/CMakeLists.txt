# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(sim
    SOURCES
        SimWorld.cpp
    DEPS
        og_common
        og_state
        og_input
        og_actions
        og_profiles)
