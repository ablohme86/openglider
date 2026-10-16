# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(combat
    SOURCES
        script/ScriptParser.cpp
        script/ScriptInterpreter.cpp
        script/SnapshotEnvironment.cpp
        script/ScriptValidator.cpp
        ScriptedCombatClass.cpp
    DEPS
        og_common
        og_state
        og_actions)
