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
 *  File: CandidateGenerator.cpp
 *
 *  Desc:
 *  Implementation of CandidateGenerator (strategic and tactical AI
 *  planning systems).
 *
 *
 */
#include "og/ai/CandidateGenerator.h"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace og
{

std::vector<CandidatePlan> CandidateGenerator::generateCandidates(const WorldModel& world, const QuestDatabase& questDb, const TravelGraph& travelGraph, const BotGoal& goal) const
{
    std::vector<CandidatePlan> candidates;

    // 1. Vendor & Repair Candidate
    const bool bagsTight = world.player.freeBagSlots <= 4;
    const bool durabilityLow = world.player.durabilityPercent <= 35.0f;

    if (bagsTight || durabilityLow)
    {
        CandidatePlan cp;
        cp.id = "candidate_vendor_repair";
        cp.description = "Visit nearby vendor to sell items and repair equipment";
        cp.score = 95.0;

        if (world.player.freeBagSlots <= 1)
        {
            cp.score += 15.0;
        }

        if (world.player.durabilityPercent <= 20.0f)
        {
            cp.score += 20.0;
        }

        Plan plan;
        plan.planId = cp.id;
        plan.goal = "Vendor and repair";
        plan.reason = std::format("Bags free: {}, Durability: {:.0f}%", world.player.freeBagSlots, world.player.durabilityPercent);

        const TravelNode* vendorNode = travelGraph.findClosestNode(world.player.position, world.player.zone);

        if (vendorNode)
        {
            PlanStep sGo;
            sGo.type = PlanStepType::GoTo;
            sGo.locationName = vendorNode->name;
            sGo.position = vendorNode->position;
            plan.steps.push_back(sGo);
        }

        if (durabilityLow)
        {
            PlanStep sRep;
            sRep.type = PlanStepType::Repair;
            plan.steps.push_back(sRep);
        }

        if (bagsTight)
        {
            PlanStep sVend;
            sVend.type = PlanStepType::Vendor;
            plan.steps.push_back(sVend);
        }

        cp.plan = std::move(plan);
        candidates.push_back(std::move(cp));
    }

    // 2. Turn in completed quests
    std::vector<int> readyTurnInIds;

    for (const auto& aq : world.quests.activeQuests)
    {
        if (aq.isComplete() && !world.quests.ignoredQuestIds.contains(aq.questId))
        {
            readyTurnInIds.push_back(aq.questId);
        }
    }

    if (!readyTurnInIds.empty())
    {
        CandidatePlan cp;
        cp.id = "candidate_turn_in";
        cp.description = std::format("Turn in {} completed quest(s)", readyTurnInIds.size());
        cp.score = 90.0 + static_cast<double>(readyTurnInIds.size()) * 5.0;

        Plan plan;
        plan.planId = cp.id;
        plan.goal = "Turn in quests";
        plan.reason = "One or more active quests are complete";

        for (int qid : readyTurnInIds)
        {
            const auto* q = questDb.find(qid);

            if (q)
            {
                PlanStep sGo;
                sGo.type = PlanStepType::GoTo;
                sGo.locationName = q->turnInNpcName.empty() ? "Quest Turn-in" : q->turnInNpcName;
                sGo.position = q->endLocation;
                plan.steps.push_back(sGo);

                PlanStep sTurn;
                sTurn.type = PlanStepType::TurnInQuest;
                sTurn.questId = qid;
                sTurn.targetName = q->turnInNpcName;
                plan.steps.push_back(sTurn);
            }
        }

        if (!plan.steps.empty())
        {
            cp.plan = std::move(plan);
            candidates.push_back(std::move(cp));
        }
    }

    // 3. Active Incomplete Quests (Clustered)
    std::vector<const QuestDefinition*> activeDefs;

    for (const auto& aq : world.quests.activeQuests)
    {
        if (!aq.isComplete() && !world.quests.ignoredQuestIds.contains(aq.questId))
        {
            const auto* q = questDb.find(aq.questId);

            if (q)
            {
                activeDefs.push_back(q);
            }
        }
    }

    if (!activeDefs.empty())
    {
        auto clusters = clusterer_.clusterQuests(activeDefs);

        for (const auto& cluster : clusters)
        {
            CandidatePlan cp;
            cp.id = cluster.clusterId;
            cp.description = std::format("Complete {}", cluster.name);
            cp.score = scorer_.scoreCluster(cluster, questDb, world.player.position, world.player.level);

            Plan plan;
            plan.planId = cluster.clusterId;
            plan.goal = cluster.name;
            plan.reason = std::format("Active quest cluster with {} quest(s)", cluster.questIds.size());

            // Go to cluster center
            PlanStep sGo;
            sGo.type = PlanStepType::GoTo;
            sGo.position = cluster.centerPosition;
            sGo.locationName = cluster.name;
            plan.steps.push_back(sGo);

            // Execute objectives
            for (int qid : cluster.questIds)
            {
                const auto* q = questDb.find(qid);

                if (q)
                {
                    for (int oIdx = 0; oIdx < static_cast<int>(q->objectives.size()); ++oIdx)
                    {
                        PlanStep sObj;
                        sObj.type = PlanStepType::CompleteQuestObjective;
                        sObj.questId = qid;
                        sObj.objectiveIndex = oIdx;
                        sObj.targetName = q->objectives[oIdx].targetName;
                        sObj.requiredCount = q->objectives[oIdx].requiredCount;

                        if (!q->objectives[oIdx].knownLocations.empty())
                        {
                            sObj.position = q->objectives[oIdx].knownLocations.front();
                        }

                        plan.steps.push_back(sObj);
                    }
                }
            }

            cp.plan = std::move(plan);
            candidates.push_back(std::move(cp));
        }
    }

    // 4. Available New Quests
    std::unordered_set<int> activeIds;
    activeIds.insert(world.quests.ignoredQuestIds.begin(), world.quests.ignoredQuestIds.end());

    for (const auto& aq : world.quests.activeQuests)
    {
        activeIds.insert(aq.questId);
    }

    auto avail = questDb.findAvailable(world.player.level, world.quests.completedQuestIds, activeIds, world.player.zone);

    if (!avail.empty())
    {
        for (const auto* q : avail)
        {
            CandidatePlan cp;
            cp.id = std::format("candidate_accept_{}", q->id);
            cp.description = std::format("Accept quest: {}", q->name);
            cp.score = scorer_.scoreQuest(*q, world.player.position, world.player.level) - 10.0; // slightly lower than active

            Plan plan;
            plan.planId = cp.id;
            plan.goal = std::format("Accept {}", q->name);
            plan.reason = "New quest available in zone";

            PlanStep sGo;
            sGo.type = PlanStepType::GoTo;
            sGo.locationName = q->questGiverName;
            sGo.position = q->startLocation;
            plan.steps.push_back(sGo);

            PlanStep sAcc;
            sAcc.type = PlanStepType::AcceptQuest;
            sAcc.questId = q->id;
            sAcc.targetName = q->questGiverName;
            plan.steps.push_back(sAcc);

            cp.plan = std::move(plan);
            candidates.push_back(std::move(cp));
        }
    }

    // 5. Grinding Fallback Candidate (Always present)
    {
        CandidatePlan cp;
        cp.id = "candidate_grind";
        cp.description = "Grind nearby mobs for XP and loot";
        cp.score = 45.0; // fallback baseline

        Plan plan;
        plan.planId = cp.id;
        plan.goal = "Grind for experience";
        plan.reason = "Autonomous grinding fallback";

        PlanStep sGrind;
        sGrind.type = PlanStepType::Grind;
        sGrind.targetLevel = goal.targetLevel > 0 ? std::min(goal.targetLevel, world.player.level + 1) : world.player.level + 1;
        sGrind.timeout = Duration(1800000); // 30 minutes
        plan.steps.push_back(sGrind);

        cp.plan = std::move(plan);
        candidates.push_back(std::move(cp));
    }

    // Sort by score descending
    std::sort(candidates.begin(), candidates.end(), [](const CandidatePlan& a, const CandidatePlan& b) { return a.score > b.score; });

    return candidates;
}

} // namespace og
