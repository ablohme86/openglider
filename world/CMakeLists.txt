# SPDX-License-Identifier: LGPL-3.0-or-later
og_add_module(world
    SOURCES
        QuestTypes.cpp
        QuestDatabase.cpp
        QuestGivers.cpp
        ZoneBounds.cpp
        QuestSource.cpp
        TravelGraph.cpp
        TaxiNetwork.cpp
        GameNames.cpp
        QuestClusterer.cpp
        QuestScorer.cpp
        WorldModel.cpp
    DEPS
        og_common
        og_state
        og_tasks)
