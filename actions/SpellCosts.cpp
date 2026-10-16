/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-04
 *  File: SpellCosts.cpp
 *
 *  Desc:
 *  Implementation of SpellCosts (semantic actions, keybindings, and
 *  cooldown tracking).
 *
 *
 */
#include "og/actions/SpellCosts.h"

#include "og/common/Db2.h"

#include <algorithm>
#include <format>

namespace og
{

namespace
{

// Where the columns are, by the table's layout (WoWDBDefs, SpellPower.dbd).
struct Columns
{
    std::uint32_t layout;
    std::size_t orderIndex;
    std::size_t manaCost;
    std::size_t powerType;
};

// 1.60.1 (WoW Forever): ID, OrderIndex, ManaCost, ManaCostPerLevel,
// ManaPerSecond, PowerDisplayID, AltPowerBarID, PowerCostPct, PowerCostMaxPct,
// OptionalCostPct, PowerPctPerSecond, PowerType, RequiredAuraSpellID,
// OptionalCost; SpellID as the relation.
constexpr Columns kLayouts[] = {
    {0x61AD223F, 1, 2, 11},
};

constexpr std::uint8_t kRage = 1;

} // namespace

std::optional<SpellCosts> SpellCosts::fromSpellPower(std::span<const std::uint8_t> db2, std::string& error)
{
    const auto table = Db2Table::parse(db2, error);

    if (!table)
    {
        return std::nullopt;
    }

    const auto columns = std::find_if(std::begin(kLayouts), std::end(kLayouts), [&](const Columns& c) { return c.layout == table->layoutHash(); });

    if (columns == std::end(kLayouts))
    {
        error = std::format("SpellPower layout {:08X} is not one the bot knows", table->layoutHash());
        return std::nullopt;
    }

    // The costs of a spell in the order the client lists them (OrderIndex).
    std::unordered_map<std::uint32_t, std::vector<std::pair<std::uint32_t, SpellCost>>> ordered;

    for (const auto& row : table->rows())
    {
        if (!row.relation || row.fields.size() <= std::max({columns->orderIndex, columns->manaCost, columns->powerType}))
        {
            continue;
        }

        const auto amount = static_cast<std::int32_t>(row.fields[columns->manaCost]);

        if (amount <= 0)
        {
            continue;
        }

        SpellCost cost;
        cost.power = static_cast<std::uint8_t>(row.fields[columns->powerType]);
        // Rage is kept in tenths: 100 is 10 rage.
        cost.amount = cost.power == kRage ? amount / 10 : amount;
        ordered[*row.relation].emplace_back(row.fields[columns->orderIndex], cost);
    }

    SpellCosts out;

    for (auto& [spell, costs] : ordered)
    {
        std::sort(costs.begin(), costs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<SpellCost> list;

        for (const auto& [order, cost] : costs)
        {
            list.push_back(cost);
        }

        out.costs_[spell] = std::move(list);
    }

    return out;
}

const std::vector<SpellCost>& SpellCosts::of(std::uint32_t spellId) const
{
    static const std::vector<SpellCost> kNone;
    const auto it = costs_.find(spellId);
    return it == costs_.end() ? kNone : it->second;
}

} // namespace og
