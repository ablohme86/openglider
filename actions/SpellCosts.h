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
 *  File: SpellCosts.h
 *
 *  Desc:
 *  Declarations and interface for SpellCosts (semantic actions,
 *  keybindings, and cooldown tracking).
 *
 *
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace og
{

// What a spell costs to cast, of one power: 10 rage, 45 energy, 25 mana.
struct SpellCost
{
    // The client's power type: 0 mana, 1 rage, 2 focus, 3 energy, 4 combo points.
    std::uint8_t power = 0;
    int amount = 0; // in the units the bot reads (whole rage, not tenths)
};

// The power costs of the spells, by spell id, from the client's own SpellPower
// table: the rank on the action bar is the cost. Talents and effects that make
// a spell cheaper are not in it, so a spell may be waited for a little longer
// than it needs - never pressed for "Not enough rage".
class SpellCosts
{
public:
    // From the SpellPower DB2 (dbfilesclient/spellpower.db2, FileDataID
    // 982806) of a layout it knows; nullopt, and why, otherwise.
    static std::optional<SpellCosts> fromSpellPower(std::span<const std::uint8_t> db2, std::string& error);

    // The costs of a spell; empty for a spell that costs nothing or is unknown.
    const std::vector<SpellCost>& of(std::uint32_t spellId) const;
    void set(std::uint32_t spellId, std::vector<SpellCost> costs)
    {
        costs_[spellId] = std::move(costs);
    }

    std::size_t size() const
    {
        return costs_.size();
    }

    static constexpr std::uint32_t kSpellPowerFileDataId = 982806;

private:
    std::unordered_map<std::uint32_t, std::vector<SpellCost>> costs_;
};

} // namespace og
