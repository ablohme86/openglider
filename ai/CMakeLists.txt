# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(ai
    SOURCES
        PlannerTypes.cpp
        CandidateGenerator.cpp
        RuleBasedStrategicPlanner.cpp
        HybridStrategicPlanner.cpp
        TacticalPlanner.cpp
        PlanningController.cpp
        PlanningBehavior.cpp
    DEPS
        og_common
        og_state
        og_tasks
        og_world)
