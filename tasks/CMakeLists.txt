# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(tasks
    SOURCES
        TaskTypes.cpp
        PlanStep.cpp
        Plan.cpp
        PlanValidator.cpp
        TaskNodes.cpp
        PlanExecutor.cpp
    DEPS
        og_common
        og_state
        og_navigation
        og_core)
