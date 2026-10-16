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
 *  File: HybridStrategicPlanner.cpp
 *
 *  Desc:
 *  Implementation of HybridStrategicPlanner (strategic and tactical AI
 *  planning systems).
 *
 *
 */
#include "og/ai/HybridStrategicPlanner.h"

#include <algorithm>
#include <cmath>

namespace og
{

std::string HybridStrategicPlanner::buildPrompt(const WorldModel& world, const BotGoal& goal, const std::vector<CandidatePlan>& candidates) const
{
    Json j = Json::parse(world.summarizeForLlm());
    j["goal"] = goal.toJson();

    Json p = Json::object();
    p["level"] = static_cast<int64_t>(world.player.level);
    p["xpPercent"] = world.player.xpPercent;
    p["zone"] = world.player.zone.empty() ? "Unknown" : world.player.zone;
    p["freeBagSlots"] = static_cast<int64_t>(world.player.freeBagSlots);
    p["durability"] = world.player.durabilityPercent;
    j["player"] = std::move(p);

    Json candArr = Json::array();

    for (const auto& c : candidates)
    {
        Json cj = Json::object();
        cj["id"] = c.id;
        cj["description"] = c.description;
        cj["score"] = c.score;
        candArr.push_back(std::move(cj));
    }

    j["candidates"] = std::move(candArr);

    return j.dump();
}

Plan HybridStrategicPlanner::createPlan(const WorldModel& world, const BotGoal& goal)
{
    lastUsedFallback_ = false;
    lastLlmReason_.clear();
    lastFallbackReason_.clear();

    auto allCandidates = candidateGen_.generateCandidates(world, questDb_, travelGraph_, goal);

    if (allCandidates.empty())
    {
        lastUsedFallback_ = true;
        lastFallbackReason_ = "No candidates available";
        return fallbackPlanner_.createPlan(world, goal);
    }

    if (!llmClient_)
    {
        lastUsedFallback_ = true;
        lastFallbackReason_ = "No AI client configured";
        return fallbackPlanner_.createPlan(world, goal);
    }

    // Pick top 5 candidates for LLM consideration
    const std::size_t topN = std::min<std::size_t>(5, allCandidates.size());
    std::vector<CandidatePlan> topCandidates(allCandidates.begin(), allCandidates.begin() + topN);

    // Build LLM request
    LlmRequest req;
    req.systemPrompt = "You are an expert autonomous WoW planning agent. "
                       "Select the single best candidate from the supplied list. "
                       "Return ONLY a valid JSON object matching this schema: "
                       "{\"selectedCandidateId\": \"<id>\", \"reason\": \"<brief reason>\", \"confidence\": <0.0-1.0>}";
    req.userPrompt = buildPrompt(world, goal, topCandidates);

    try
    {
        LlmResponse resp = llmClient_->complete(req);

        if (!resp.success || resp.content.empty())
        {
            lastUsedFallback_ = true;
            lastFallbackReason_ = resp.errorMessage.empty() ? "AI returned no content" : resp.errorMessage;
            return fallbackPlanner_.createPlan(world, goal);
        }

        Json parsed = Json::parse(resp.content);

        if (!parsed.isObject() || parsed.size() != 3 || !parsed["selectedCandidateId"].isString() || !parsed["reason"].isString() || !parsed["confidence"].isNumber())
        {
            lastUsedFallback_ = true;
            lastFallbackReason_ = "AI response did not match the planning schema";
            return fallbackPlanner_.createPlan(world, goal);
        }

        const std::string selectedId = parsed["selectedCandidateId"].asString();
        const std::string reason = parsed.contains("reason") ? parsed["reason"].asString() : "";
        const double confidence = parsed["confidence"].asDouble();

        // Confidence threshold guardrail (Section 58: if confidence < 0.55, fallback)
        if (!std::isfinite(confidence) || confidence < 0.55 || confidence > 1.0)
        {
            lastUsedFallback_ = true;
            lastFallbackReason_ = "AI confidence was invalid or below 0.55";
            return fallbackPlanner_.createPlan(world, goal);
        }

        // Locate candidate plan
        for (const auto& c : topCandidates)
        {
            if (c.id == selectedId)
            {
                Plan chosen = c.plan;
                chosen.candidateId = selectedId;
                chosen.reason = reason;
                chosen.confidence = confidence;
                lastLlmReason_ = reason;

                // Validate before acceptance (Section 11)
                auto valRes = validator_.validate(chosen);

                if (valRes.valid)
                {
                    return chosen;
                }

                break;
            }
        }
    }
    catch (...)
    {
        // Parse error or invalid JSON -> safe fallback
        lastFallbackReason_ = "AI request failed or returned malformed JSON";
    }

    lastUsedFallback_ = true;

    if (lastFallbackReason_.empty())
    {
        lastFallbackReason_ = "AI selected an unknown or invalid candidate";
    }

    return fallbackPlanner_.createPlan(world, goal);
}

} // namespace og
