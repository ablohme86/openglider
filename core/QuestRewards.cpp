/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-30
 *  File: QuestRewards.cpp
 *
 *  Desc:
 *  Implementation of QuestRewards (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/QuestRewards.h"

#include <algorithm>
#include <tuple>
#include <cctype>
#include <string_view>

namespace og
{

bool isGear(const UiItemTooltip& item)
{
    static constexpr std::string_view kSlots[] = {"Head", "Neck", "Shoulder", "Back", "Chest", "Shirt", "Tabard", "Wrist", "Hands", "Waist", "Legs", "Feet", "Finger", "Trinket", "Main Hand", "Off Hand", "One-Hand", "Two-Hand", "Ranged", "Held In Off-hand", "Thrown", "Relic"};
    return std::any_of(item.lines.begin(), item.lines.end(), [](const std::string& line) { return std::find(std::begin(kSlots), std::end(kSlots), line) != std::end(kSlots); });
}

namespace
{

enum class Armour
{
    None, // not armour: a weapon, a ring, a cloak
    Cloth,
    Leather,
    Mail,
    Plate
};

Armour armourOf(const UiItemTooltip& item)
{
    // A cloak says Cloth but is for everyone.
    if (std::any_of(item.lines.begin(), item.lines.end(), [](const std::string& line) { return line == "Back"; }))
    {
        return Armour::None;
    }

    for (const auto& line : item.lines)
    {
        if (line == "Cloth")
        {
            return Armour::Cloth;
        }

        if (line == "Leather")
        {
            return Armour::Leather;
        }

        if (line == "Mail")
        {
            return Armour::Mail;
        }

        if (line == "Plate")
        {
            return Armour::Plate;
        }
    }

    return Armour::None;
}

// The heaviest armour the class wears at its level (classic: mail and plate
// from 40 for the classes that learn them).
Armour bestArmour(PlayerClass c, int level)
{
    switch (c)
    {
    case PlayerClass::Warrior:
    case PlayerClass::Paladin:
        return level >= 40 ? Armour::Plate : Armour::Mail;
    case PlayerClass::Hunter:
    case PlayerClass::Shaman:
        return level >= 40 ? Armour::Mail : Armour::Leather;
    case PlayerClass::Rogue:
    case PlayerClass::Druid:
        return Armour::Leather;
    case PlayerClass::DeathKnight:
        return Armour::Plate;
    case PlayerClass::Priest:
    case PlayerClass::Mage:
    case PlayerClass::Warlock:
        return Armour::Cloth;
    default:
        return Armour::None;
    }
}

// The class's stats, the main one counting twice: "+3 Agility" is 6 for a
// hunter, "+2 Stamina" 2.
int statScore(const UiItemTooltip& item, PlayerClass c)
{
    std::string_view main;

    switch (c)
    {
    case PlayerClass::Warrior:
    case PlayerClass::Paladin:
    case PlayerClass::DeathKnight:
        main = "Strength";
        break;
    case PlayerClass::Hunter:
    case PlayerClass::Rogue:
    case PlayerClass::Druid:
        main = "Agility";
        break;
    case PlayerClass::Shaman:
    case PlayerClass::Priest:
    case PlayerClass::Mage:
    case PlayerClass::Warlock:
        main = "Intellect";
        break;
    default:
        return 0;
    }

    int score = 0;

    for (const auto& line : item.lines)
    {
        int value = 0;
        std::size_t at = 0;

        if (line.size() < 3 || line[0] != '+')
        {
            continue;
        }

        for (at = 1; at < line.size() && std::isdigit(static_cast<unsigned char>(line[at])); ++at)
        {
            value = value * 10 + (line[at] - '0');
        }

        const std::string_view stat = std::string_view(line).substr(std::min(line.size(), at + 1));

        if (stat == main)
        {
            score += 2 * value;
        }
        else if (stat == "Stamina")
        {
            score += value;
        }
    }

    return score;
}

// The armour a piece gives ("42 Armor"), or a weapon's damage per second in
// tenths ("(4.6 damage per second)": 46). The lines of a comparison
// tooltip's stat changes are not the item's own.
int protectionOrDamage(const UiItemTooltip& item)
{
    int out = 0;

    for (const auto& line : item.lines)
    {
        if (line.starts_with("If you replace this item"))
        {
            break;
        }

        std::size_t at = 0;
        int whole = 0;

        if (line.starts_with("("))
        {
            at = 1;
        }

        const std::size_t first = at;

        while (at < line.size() && std::isdigit(static_cast<unsigned char>(line[at])))
        {
            whole = whole * 10 + (line[at++] - '0');
        }

        if (at == first)
        {
            continue;
        }

        const std::string_view rest = std::string_view(line).substr(at);

        if (first == 0 && rest == " Armor")
        {
            out += whole;
        }
        else if (first == 1)
        {
            int tenth = 0;
            std::size_t after = at;

            if (after + 1 < line.size() && line[after] == '.' && std::isdigit(static_cast<unsigned char>(line[after + 1])))
            {
                tenth = line[after + 1] - '0';
                after += 2;

                while (after < line.size() && std::isdigit(static_cast<unsigned char>(line[after])))
                {
                    ++after;
                }
            }

            if (std::string_view(line).substr(after).starts_with(" damage per second)"))
            {
                out += whole * 10 + tenth;
            }
        }
    }

    return out;
}

// The tooltip without what a comparison tooltip adds after its own lines.
UiItemTooltip ownLines(const UiItemTooltip& item)
{
    UiItemTooltip out = item;
    const auto cut = std::find_if(out.lines.begin(), out.lines.end(), [](const std::string& line) { return line.starts_with("If you replace this item"); });
    out.lines.erase(cut, out.lines.end());
    return out;
}

} // namespace

bool bindsWhenEquipped(const UiItemTooltip& item)
{
    const auto has = [&](std::string_view text) { return std::any_of(item.lines.begin(), item.lines.end(), [&](const std::string& line) { return line == text; }); };
    return has("Binds when equipped") && !has("Soulbound");
}

std::tuple<int, int, int, int> gearRank(const UiItemTooltip& item, PlayerClass playerClass, int level)
{
    const UiItemTooltip own = ownLines(item);
    const Armour wears = bestArmour(playerClass, level);
    const Armour kind = armourOf(own);
    const int fit = wears == Armour::None || kind == Armour::None || kind == wears ? 1 : 0;
    return std::tuple{fit, statScore(own, playerClass), protectionOrDamage(own), own.sellPrice};
}

std::optional<std::size_t> chooseQuestReward(const std::vector<std::optional<UiItemTooltip>>& rewards, QuestConfig::Reward policy, PlayerClass playerClass, int level)
{
    const auto best = [&](auto&& fits) -> std::optional<std::size_t>
    {
        std::optional<std::size_t> out;

        for (std::size_t i = 0; i < rewards.size(); ++i)
        {
            if (!rewards[i] || !fits(*rewards[i]))
            {
                continue;
            }

            if (!out || rewards[i]->sellPrice > rewards[*out]->sellPrice)
            {
                out = i;
            }
        }

        return out;
    };

    if (policy == QuestConfig::Reward::GearForCharacter)
    {
        const Armour wears = bestArmour(playerClass, level);
        std::optional<std::size_t> out;
        const auto rank = [&](const UiItemTooltip& t)
        {
            const Armour kind = armourOf(t);
            // Armour of a lighter kind than the class wears comes after.
            const int fit = wears == Armour::None || kind == Armour::None || kind == wears ? 1 : 0;
            return std::tuple{fit, statScore(t, playerClass), t.sellPrice};
        };

        for (std::size_t i = 0; i < rewards.size(); ++i)
        {
            if (rewards[i] && isGear(*rewards[i]) && !rewards[i]->hasRedLine() && (!out || rank(*rewards[i]) > rank(*rewards[*out])))
            {
                out = i;
            }
        }

        if (out)
        {
            return out;
        }
    }

    return best([](const UiItemTooltip&) { return true; });
}

} // namespace og
