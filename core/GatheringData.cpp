/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-12
 *  File: GatheringData.cpp
 *
 *  Desc:
 *  Implementation of GatheringData (core bot engine, behaviors,
 *  context, and objective tracking).
 *
 *
 */
#include "og/core/GatheringData.h"

#include <algorithm>
#include <cctype>
#include <span>
#include <string>
#include <string_view>

namespace og
{
namespace
{

struct NodeRequirement
{
    std::string_view name;
    int skill;
};

// Herb requirements from Classic 1.12 through WotLK 3.3.5a.
// Longer names are listed earlier for substring matching.
constexpr NodeRequirement kHerbs[] = {
    {"frost lotus", 450}, {"icethorn", 435}, {"lichbloom", 425}, {"adder's tongue", 400}, {"deadnettle", 400}, {"frozen herb", 400}, {"talandra's rose", 385}, {"mana thistle", 375}, {"tiger lily", 375}, {"nightmare vine", 365}, {"firethorn", 360}, {"ancient lichen", 340}, {"flame cap", 335}, {"goldclover", 350}, {"netherbloom", 350}, {"netherdust bush", 350}, {"ragveil", 325}, {"terocone", 325}, {"dreaming glory", 315}, {"black lotus", 300}, {"felweed", 300}, {"icecap", 290}, {"plaguebloom", 285}, {"mountain silversage", 280}, {"dreamfoil", 270}, {"golden sansam", 260}, {"gromsblood", 250}, {"ghost mushroom", 245}, {"blindweed", 235}, {"sungrass", 230}, {"arthas' tears", 220}, {"purple lotus", 210}, {"firebloom", 205}, {"wintersbite", 195}, {"khadgar's whisker", 185}, {"goldthorn", 170}, {"fadeleaf", 160}, {"liferoot", 150}, {"kingsblood", 125}, {"grave moss", 120}, {"wild steelbloom", 115}, {"bruiseweed", 100}, {"stranglekelp", 85}, {"briarthorn", 70}, {"mageroyal", 50}, {"earthroot", 15}, {"peacebloom", 1}, {"silverleaf", 1}, {"bloodthistle", 1},
};

// Mineral / ore requirements from Classic 1.12 through WotLK 3.3.5a.
// Longer names are listed earlier for substring matching.
constexpr NodeRequirement kMinerals[] = {
    {"pure saronite deposit", 450}, {"titanium vein", 450}, {"rich saronite deposit", 425}, {"saronite deposit", 400}, {"rich cobalt deposit", 375}, {"khorium vein", 375}, {"nethercite deposit", 375}, {"rich adamantite deposit", 350}, {"cobalt deposit", 350}, {"adamantite deposit", 325}, {"fel iron deposit", 300}, {"rich thorium vein", 275}, {"small thorium vein", 245}, {"thorium vein", 245}, {"truesilver deposit", 230}, {"dark iron deposit", 230}, {"dark iron vein", 230}, {"mithril deposit", 175}, {"gold vein", 155}, {"indurium mineral vein", 150}, {"iron deposit", 125}, {"iron vein", 125}, {"lesser bloodstone deposit", 75}, {"bloodstone deposit", 75}, {"silver vein", 75}, {"incendicite mineral vein", 65}, {"tin vein", 65}, {"copper vein", 1},
};

std::string toLower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());

    for (char c : s)
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return out;
}

int matchRequirement(std::span<const NodeRequirement> table, std::string_view name)
{
    const std::string lower = toLower(name);

    // 1. Exact match
    for (const auto& req : table)
    {
        if (lower == req.name)
        {
            return req.skill;
        }
    }

    // 2. Substring match (e.g. "Ooze Covered Rich Thorium Vein" matches "rich thorium vein")
    for (const auto& req : table)
    {
        if (lower.find(req.name) != std::string::npos)
        {
            return req.skill;
        }
    }

    return 1; // Default to apprentice requirement for unknown nodes of that kind
}

} // namespace

int requiredGatheringSkill(GameObjectKind kind, std::string_view name)
{
    if (kind == GameObjectKind::Herb)
    {
        return matchRequirement(kHerbs, name);
    }

    if (kind == GameObjectKind::Mineral)
    {
        return matchRequirement(kMinerals, name);
    }

    return 0;
}

bool canGatherNode(const GameObjectState& node, const PlayerState& player)
{
    if (player.skills.empty())
    {
        return true;
    }

    const int required = requiredGatheringSkill(node.kind, node.name);

    if (required <= 0)
    {
        return true;
    }

    if (node.kind == GameObjectKind::Herb)
    {
        if (!player.hasSkill(kSkillHerbalism))
        {
            return false;
        }

        return player.herbalismSkill() >= required;
    }

    if (node.kind == GameObjectKind::Mineral)
    {
        if (!player.hasSkill(kSkillMining))
        {
            return false;
        }

        return player.miningSkill() >= required;
    }

    return true;
}

GatherDifficulty gatherDifficulty(const GameObjectState& node, const PlayerState& player)
{
    const int required = requiredGatheringSkill(node.kind, node.name);

    if (player.skills.empty() || required <= 0)
    {
        return GatherDifficulty::Unknown;
    }

    const std::uint32_t skill = node.kind == GameObjectKind::Herb ? kSkillHerbalism : kSkillMining;

    if (!player.hasSkill(skill))
    {
        return GatherDifficulty::Red;
    }

    const int rank = player.skillRank(skill);

    if (rank < required)
    {
        return GatherDifficulty::Red;
    }

    if (rank < required + 25)
    {
        return GatherDifficulty::Orange;
    }

    if (rank < required + 50)
    {
        return GatherDifficulty::Yellow;
    }

    if (rank < required + 100)
    {
        return GatherDifficulty::Green;
    }

    return GatherDifficulty::Gray;
}

int requiredSkinningSkill(int creatureLevel)
{
    if (creatureLevel <= 0)
    {
        return 0;
    }

    if (creatureLevel <= 10)
    {
        return 1;
    }

    if (creatureLevel <= 20)
    {
        return (creatureLevel - 10) * 10;
    }

    // Above 73 (365) the WotLK requirements no longer follow the formula.
    if (creatureLevel > 73)
    {
        return 0;
    }

    return creatureLevel * 5;
}

bool canSkinUnit(const UnitState& unit, const PlayerState& player)
{
    if (player.skills.empty())
    {
        return true;
    }

    if (!player.hasSkill(kSkillSkinning))
    {
        return player.hasSkill(kSkillHerbalism) || player.hasSkill(kSkillMining);
    }

    return player.skinningSkill() >= requiredSkinningSkill(unit.level);
}

} // namespace og
