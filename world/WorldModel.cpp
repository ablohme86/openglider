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
 *  File: WorldModel.cpp
 *
 *  Desc:
 *  Implementation of WorldModel (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/WorldModel.h"
#include "og/world/QuestDatabase.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace og
{

// PlayerKnowledge
Json PlayerKnowledge::toJson() const
{
    Json j = Json::object();
    j["level"] = static_cast<int64_t>(level);
    j["xpPercent"] = xpPercent;
    j["healthPercent"] = healthPercent;
    j["manaPercent"] = manaPercent;
    j["money"] = money;
    j["freeBagSlots"] = static_cast<int64_t>(freeBagSlots);
    j["durabilityPercent"] = durabilityPercent;
    j["class"] = std::string(toString(playerClass));

    if (!race.empty())
    {
        j["race"] = race;
    }

    if (!zone.empty())
    {
        j["zone"] = zone;
    }

    if (!subZone.empty())
    {
        j["subZone"] = subZone;
    }

    j["position"] = Json(Json::Array{position.x, position.y, position.z});

    if (!knownFlightPaths.empty())
    {
        Json fp = Json::array();

        for (int p : knownFlightPaths)
        {
            fp.push_back(static_cast<int64_t>(p));
        }

        j["knownFlightPaths"] = std::move(fp);
    }

    return j;
}

PlayerKnowledge PlayerKnowledge::fromJson(const Json& j)
{
    PlayerKnowledge pk;

    if (j.contains("level"))
    {
        pk.level = static_cast<int>(j["level"].asInt());
    }

    if (j.contains("xpPercent"))
    {
        pk.xpPercent = j["xpPercent"].asFloat();
    }

    if (j.contains("healthPercent"))
    {
        pk.healthPercent = j["healthPercent"].asFloat();
    }

    if (j.contains("manaPercent"))
    {
        pk.manaPercent = j["manaPercent"].asFloat();
    }

    if (j.contains("money"))
    {
        pk.money = j["money"].asInt();
    }

    if (j.contains("freeBagSlots"))
    {
        pk.freeBagSlots = static_cast<int>(j["freeBagSlots"].asInt());
    }

    if (j.contains("durabilityPercent"))
    {
        pk.durabilityPercent = j["durabilityPercent"].asFloat();
    }

    if (j.contains("class"))
    {
        pk.playerClass = parsePlayerClass(j["class"].asString());
    }

    if (j.contains("race"))
    {
        pk.race = j["race"].asString();
    }

    if (j.contains("zone"))
    {
        pk.zone = j["zone"].asString();
    }

    if (j.contains("subZone"))
    {
        pk.subZone = j["subZone"].asString();
    }

    if (j.contains("position") && j["position"].isArray() && j["position"].asArray().size() == 3)
    {
        const auto& arr = j["position"].asArray();
        pk.position = Vec3{arr[0].asFloat(), arr[1].asFloat(), arr[2].asFloat()};
    }

    if (j.contains("knownFlightPaths") && j["knownFlightPaths"].isArray())
    {
        for (const auto& p : j["knownFlightPaths"].asArray())
        {
            pk.knownFlightPaths.insert(static_cast<int>(p.asInt()));
        }
    }

    return pk;
}

// QuestKnowledge
Json QuestKnowledge::toJson() const
{
    Json j = Json::object();
    Json act = Json::array();

    for (const auto& q : activeQuests)
    {
        act.push_back(q.toJson());
    }

    j["activeQuests"] = std::move(act);
    Json ignored = Json::array();
    std::vector<int> ignoredIds(ignoredQuestIds.begin(), ignoredQuestIds.end());
    std::sort(ignoredIds.begin(), ignoredIds.end());
    for (const int id : ignoredIds)
    {
        ignored.push_back(id);
    }
    j["ignoredQuestIds"] = std::move(ignored);

    Json comp = Json::array();

    for (int id : completedQuestIds)
    {
        comp.push_back(static_cast<int64_t>(id));
    }

    j["completedQuestIds"] = std::move(comp);

    Json avail = Json::array();

    for (int id : availableQuestIds)
    {
        avail.push_back(static_cast<int64_t>(id));
    }

    j["availableQuestIds"] = std::move(avail);
    return j;
}

QuestKnowledge QuestKnowledge::fromJson(const Json& j)
{
    QuestKnowledge qk;
    if (j["ignoredQuestIds"].isArray())
    {
        for (const auto& id : j["ignoredQuestIds"].asArray())
        {
            qk.ignoredQuestIds.insert(static_cast<int>(id.asInt()));
        }
    }

    if (j.contains("activeQuests") && j["activeQuests"].isArray())
    {
        for (const auto& q : j["activeQuests"].asArray())
        {
            qk.activeQuests.push_back(ActiveQuestState::fromJson(q));
        }
    }

    if (j.contains("completedQuestIds") && j["completedQuestIds"].isArray())
    {
        for (const auto& id : j["completedQuestIds"].asArray())
        {
            qk.completedQuestIds.insert(static_cast<int>(id.asInt()));
        }
    }

    if (j.contains("availableQuestIds") && j["availableQuestIds"].isArray())
    {
        for (const auto& id : j["availableQuestIds"].asArray())
        {
            qk.availableQuestIds.push_back(static_cast<int>(id.asInt()));
        }
    }

    return qk;
}

// HistoricalKnowledge
void HistoricalKnowledge::recordQuestResult(int questId, bool success, int deaths, Duration duration)
{
    auto& s = questStats[questId];
    ++s.attempts;

    if (success)
    {
        ++s.completions;
    }

    s.deaths += deaths;
    const double minutes = toSeconds(duration) / 60.0;

    if (s.attempts == 1)
    {
        s.avgMinutes = minutes;
    }
    else
    {
        s.avgMinutes = (s.avgMinutes * (s.attempts - 1) + minutes) / s.attempts;
    }

    s.failureRate = s.attempts > 0 ? static_cast<double>(s.attempts - s.completions) / s.attempts : 0.0;
}

const HistoricalQuestStats* HistoricalKnowledge::find(int questId) const
{
    auto it = questStats.find(questId);
    return it != questStats.end() ? &it->second : nullptr;
}

Json HistoricalKnowledge::toJson() const
{
    Json j = Json::object();
    Json qs = Json::object();

    for (const auto& [id, s] : questStats)
    {
        Json sj = Json::object();
        sj["attempts"] = static_cast<int64_t>(s.attempts);
        sj["completions"] = static_cast<int64_t>(s.completions);
        sj["deaths"] = static_cast<int64_t>(s.deaths);
        sj["avgMinutes"] = s.avgMinutes;
        sj["failureRate"] = s.failureRate;
        qs[std::to_string(id)] = std::move(sj);
    }

    j["questStats"] = std::move(qs);
    return j;
}

HistoricalKnowledge HistoricalKnowledge::fromJson(const Json& j)
{
    HistoricalKnowledge hk;

    if (j.contains("questStats") && j["questStats"].isObject())
    {
        for (const auto& [k, sj] : j["questStats"].asObject())
        {
            HistoricalQuestStats s;

            if (sj.contains("attempts"))
            {
                s.attempts = static_cast<int>(sj["attempts"].asInt());
            }

            if (sj.contains("completions"))
            {
                s.completions = static_cast<int>(sj["completions"].asInt());
            }

            if (sj.contains("deaths"))
            {
                s.deaths = static_cast<int>(sj["deaths"].asInt());
            }

            if (sj.contains("avgMinutes"))
            {
                s.avgMinutes = sj["avgMinutes"].asDouble();
            }

            if (sj.contains("failureRate"))
            {
                s.failureRate = sj["failureRate"].asDouble();
            }

            try
            {
                hk.questStats[std::stoi(k)] = s;
            }
            catch (...)
            {
            }
        }
    }

    return hk;
}

// WorldModel
Json WorldModel::toJson() const
{
    Json j = Json::object();
    j["mapId"] = mapId;
    j["zoneId"] = zoneId;
    j["player"] = player.toJson();
    j["quests"] = quests.toJson();
    j["history"] = history.toJson();
    Json results = Json::array();

    for (const auto& e : recentTaskResults)
    {
        Json r = Json::object();
        r["planId"] = e.planId;
        r["stepIndex"] = e.stepIndex;
        r["step"] = e.step.toJson();
        r["status"] = taskStatusToString(e.result.status);
        r["failureReason"] = failureReasonToString(e.result.failureReason);
        r["details"] = e.result.details;
        Json metrics = Json::object();

        for (const auto& [key, value] : e.result.metrics)
        {
            if (std::isfinite(value))
            {
                metrics[key] = value;
            }
        }

        r["metrics"] = std::move(metrics);
        results.push_back(std::move(r));
    }

    j["recentTaskResults"] = std::move(results);
    return j;
}

WorldModel WorldModel::fromJson(const Json& j)
{
    WorldModel wm;
    wm.mapId = static_cast<std::uint32_t>(j["mapId"].asInt());
    wm.zoneId = static_cast<std::uint32_t>(j["zoneId"].asInt());

    if (j.contains("player"))
    {
        wm.player = PlayerKnowledge::fromJson(j["player"]);
    }

    if (j.contains("quests"))
    {
        wm.quests = QuestKnowledge::fromJson(j["quests"]);
    }

    if (j.contains("history"))
    {
        wm.history = HistoricalKnowledge::fromJson(j["history"]);
    }

    if (j["recentTaskResults"].isArray())
    {
        const auto& results = j["recentTaskResults"].asArray();
        const auto start = results.size() > WorldModelTaskResultUpdater::maxRecentResults ? results.size() - WorldModelTaskResultUpdater::maxRecentResults : 0;

        for (std::size_t i = start; i < results.size(); ++i)
        {
            const auto& r = results[i];
            PlanStepResultEvent e;
            e.planId = r["planId"].asString("");
            e.stepIndex = static_cast<std::size_t>(std::max<std::int64_t>(0, r["stepIndex"].asInt()));
            e.step = PlanStep::fromJson(r["step"]);
            e.result.status = taskStatusFromString(r["status"].asString("Failed"));
            e.result.failureReason = failureReasonFromString(r["failureReason"].asString("None"));
            e.result.details = r["details"].asString("");

            if (r["metrics"].isObject())
            {
                for (const auto& [key, value] : r["metrics"].asObject())
                {
                    if (value.isNumber() && std::isfinite(value.asDouble()))
                    {
                        e.result.metrics[key] = value.asDouble();
                    }
                }
            }

            wm.recentTaskResults.push_back(std::move(e));
        }
    }

    return wm;
}

std::string WorldModel::summarizeForLlm() const
{
    Json j = Json::object();
    j["mapId"] = mapId;
    j["zoneId"] = zoneId;

    Json p = Json::object();
    p["level"] = static_cast<int64_t>(player.level);
    p["xpPercent"] = player.xpPercent;
    p["zone"] = player.zone.empty() ? "Unknown" : player.zone;
    p["freeBagSlots"] = static_cast<int64_t>(player.freeBagSlots);
    p["durability"] = player.durabilityPercent;
    j["player"] = std::move(p);

    Json act = Json::array();

    for (const auto& q : quests.activeQuests)
    {
        Json qj = Json::object();
        qj["id"] = static_cast<int64_t>(q.questId);
        qj["isComplete"] = q.isComplete();
        act.push_back(std::move(qj));
    }

    j["activeQuests"] = std::move(act);
    j["recentTaskResults"] = toJson()["recentTaskResults"];
    j["ignoredQuestIds"] = quests.toJson()["ignoredQuestIds"];
    j["history"] = history.toJson();

    return j.dump();
}

// WorldModelBuilder
void WorldModelBuilder::updateFromSnapshot(WorldModel& model, const GameSnapshot& s)
{
    model.mapId = s.mapId.value_or(0);
    model.zoneId = s.zoneId;

    model.player.level = s.player.level;
    model.player.xpPercent = s.player.nextLevelXp > 0 ? (100.0f * static_cast<float>(s.player.xp) / static_cast<float>(s.player.nextLevelXp)) : 0.0f;
    model.player.healthPercent = s.player.healthPercent();
    model.player.manaPercent = s.player.manaPercent();

    if (s.player.freeBagSlots >= 0)
    {
        model.player.freeBagSlots = s.player.freeBagSlots;
    }

    model.player.playerClass = s.player.playerClass;
    model.player.position = s.player.position;
    model.timestamp = s.timestamp;
}

void WorldModelTaskResultUpdater::apply(WorldModel& model, const PlanStepResultEvent& event, const QuestDatabase& quests) const
{
    if (!event.result.isDone())
    {
        return;
    }

    model.recentTaskResults.push_back(event);

    if (model.recentTaskResults.size() > maxRecentResults)
    {
        model.recentTaskResults.erase(model.recentTaskResults.begin(), model.recentTaskResults.end() - maxRecentResults);
    }

    const auto& step = event.step;
    const auto& result = event.result;
    const auto* definition = quests.find(step.questId);
    auto& active = model.quests.activeQuests;
    auto quest = std::find_if(active.begin(), active.end(), [&](const auto& q) { return q.questId == step.questId; });

    if (result.isSuccess() && step.type == PlanStepType::AcceptQuest && definition && !model.quests.completedQuestIds.contains(step.questId))
    {
        if (quest == active.end())
        {
            ActiveQuestState accepted;
            accepted.questId = step.questId;

            for (const auto& objective : definition->objectives)
            {
                accepted.objectives.push_back({objective.targetName, 0, objective.requiredCount});
            }

            active.push_back(std::move(accepted));
        }

        std::erase(model.quests.availableQuestIds, step.questId);
        return;
    }

    if (step.type == PlanStepType::CompleteQuestObjective && quest != active.end() && step.objectiveIndex >= 0 && static_cast<std::size_t>(step.objectiveIndex) < quest->objectives.size())
    {
        auto& objective = quest->objectives[static_cast<std::size_t>(step.objectiveIndex)];
        auto progress = result.metrics.find("currentCount");

        if (progress != result.metrics.end() && std::isfinite(progress->second) && progress->second >= 0 && progress->second <= std::numeric_limits<int>::max())
        {
            objective.current = std::max(objective.current, static_cast<int>(progress->second));
        }

        if (result.isSuccess())
        {
            objective.current = std::max(objective.current, objective.required);
        }

        if (quest->isComplete())
        {
            quest->state = QuestRuntimeState::Complete;
        }
    }

    // Only a confirmed turn-in completes a quest. Objective successes and interruptions
    // are not separate quest completions (or failures) in the scoring history.
    if (result.isSuccess() && step.type == PlanStepType::TurnInQuest && quest != active.end())
    {
        model.quests.completedQuestIds.insert(step.questId);
        active.erase(quest);
        std::erase(model.quests.availableQuestIds, step.questId);
    }

    // Do not apply money/slots/position deltas: the fresh snapshot already includes them.
}

} // namespace og
