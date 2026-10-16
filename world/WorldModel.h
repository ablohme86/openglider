/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: WorldModel.h
 *
 *  Desc:
 *  Declarations and interface for WorldModel (world model, travel
 *  graph, zone bounds, and quest databases).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Json.h"
#include "og/common/Vec3.h"
#include "og/state/GameSnapshot.h"
#include "og/state/Types.h"
#include "og/tasks/TaskEvents.h"
#include "og/world/QuestScorer.h"
#include "og/world/QuestTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace og
{

struct PlayerKnowledge
{
    int level = 1;
    float xpPercent = 0.0f;
    float healthPercent = 100.0f;
    float manaPercent = 100.0f;
    std::int64_t money = 0;
    int freeBagSlots = 16;
    float durabilityPercent = 100.0f;
    PlayerClass playerClass = PlayerClass::Unknown;
    std::string race;
    std::string zone;
    std::string subZone;
    Vec3 position{};
    std::unordered_set<int> knownFlightPaths;
    std::unordered_map<std::string, int> itemCounts;

    Json toJson() const;
    static PlayerKnowledge fromJson(const Json& j);
};

struct QuestKnowledge
{
    std::vector<ActiveQuestState> activeQuests;
    std::unordered_set<int> completedQuestIds;
    std::vector<int> availableQuestIds;
    std::unordered_set<int> ignoredQuestIds;

    Json toJson() const;
    static QuestKnowledge fromJson(const Json& j);
};

struct HistoricalKnowledge
{
    std::unordered_map<int, HistoricalQuestStats> questStats;

    void recordQuestResult(int questId, bool success, int deaths, Duration duration);
    const HistoricalQuestStats* find(int questId) const;

    Json toJson() const;
    static HistoricalKnowledge fromJson(const Json& j);
};

struct WorldModel
{
    PlayerKnowledge player;
    QuestKnowledge quests;
    HistoricalKnowledge history;
    // Bounded semantic feedback; snapshots remain authoritative for player state.
    std::vector<PlanStepResultEvent> recentTaskResults;
    std::uint32_t mapId = 0;
    std::uint32_t zoneId = 0;
    TimePoint timestamp{};

    Json toJson() const;
    static WorldModel fromJson(const Json& j);

    // Creates the compact JSON planning payload for LLM prompts (Section 26)
    std::string summarizeForLlm() const;
};

class WorldModelBuilder
{
public:
    WorldModelBuilder() = default;

    void updateFromSnapshot(WorldModel& model, const GameSnapshot& s);
};

class QuestDatabase;

class WorldModelTaskResultUpdater
{
public:
    static constexpr std::size_t maxRecentResults = 20;
    void apply(WorldModel& model, const PlanStepResultEvent& event, const QuestDatabase& quests) const;
};

} // namespace og
