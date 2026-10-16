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
 *  File: VendorScript.cpp
 *
 *  Desc:
 *  Implementation of VendorScript (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/VendorScript.h"
#include "og/state/IGameStateProvider.h"

#include <vector>
#include <algorithm>
#include <iterator>

namespace og
{

namespace
{

// {['A']=1,['B']=1}
std::string luaSet(const std::vector<std::string>& names)
{
    std::string out = "{";
    bool first = true;

    for (const auto& name : names)
    {
        if (name.empty())
        {
            continue;
        }

        if (!first)
        {
            out += ',';
        }

        out += "[" + luaQuote(name) + "]=1";
        first = false;
    }

    return out + "}";
}

// The herbs of the classic world: their names are all there is to tell them by.
constexpr std::string_view kHerbs[] = {"Peacebloom", "Silverleaf", "Earthroot", "Mageroyal", "Briarthorn", "Swiftthistle", "Stranglekelp", "Bruiseweed", "Wild Steelbloom", "Grave Moss", "Kingsblood", "Liferoot", "Fadeleaf", "Goldthorn", "Khadgar's Whisker", "Wintersbite", "Firebloom", "Purple Lotus", "Wildvine", "Arthas' Tears", "Sungrass", "Blindweed", "Ghost Mushroom", "Gromsblood", "Golden Sansam", "Dreamfoil", "Mountain Silversage", "Plaguebloom", "Icecap", "Black Lotus", "Felweed", "Dreaming Glory", "Ragveil", "Terocone", "Ancient Lichen", "Netherbloom", "Nightmare Vine", "Mana Thistle", "Flame Cap", "Goldclover", "Tiger Lily", "Talandra's Rose", "Adder's Tongue", "Deadnettle", "Lichbloom", "Icethorn", "Frost Lotus", "Fel Lotus"};
// What cooking takes from a kill or the water.
constexpr std::string_view kCookingEnds[] = {" Meat", " Flank", " Rib", " Ribs", " Liver", " Egg", " Flesh", " Fin", " Leg", " Intestines", " Snout", " Haunch", " Tongue", " Wing", " Claw"};

bool endsWith(std::string_view s, std::string_view end)
{
    return s.size() >= end.size() && s.substr(s.size() - end.size()) == end;
}

} // namespace

SellCategory sellCategory(std::string_view name, bool reagent)
{
    if (name.empty() || !reagent)
    {
        return SellCategory::None;
    }

    if (std::find(std::begin(kHerbs), std::end(kHerbs), name) != std::end(kHerbs))
    {
        return SellCategory::Herbalism;
    }

    if (name.find("Leather") != std::string_view::npos || endsWith(name, " Hide") || endsWith(name, " Scale") || name.find("Dragonscale") != std::string_view::npos)
    {
        return SellCategory::Skinning;
    }

    if (name.starts_with("Raw "))
    {
        return SellCategory::Cooking;
    }

    for (const auto end : kCookingEnds)
    {
        if (endsWith(name, end))
        {
            return SellCategory::Cooking;
        }
    }

    return SellCategory::None;
}

bool sellsByCategory(const VendorConfig& config, std::string_view name, bool reagent)
{
    switch (sellCategory(name, reagent))
    {
    case SellCategory::Cooking:
        return config.sellCooking;
    case SellCategory::Skinning:
        return config.sellSkinning;
    case SellCategory::Herbalism:
        return config.sellHerbalism;
    case SellCategory::None:
        break;
    }

    return false;
}

bool shouldSellTooltip(const VendorConfig& config, const UiItemTooltip& item)
{
    if (item.name.empty() || item.quality < 0)
    {
        return false;
    }

    const auto hasLine = [&](std::string_view text) { return std::any_of(item.lines.begin(), item.lines.end(), [&](const std::string& line) { return line == text; }); };

    if (hasLine("No sell price") || hasLine("Quest Item"))
    {
        return false;
    }

    // A whole category the user sells: meat, leather, herbs.
    if (sellsByCategory(config, item.name, hasLine("Crafting Reagent")) && std::find(config.neverSell.begin(), config.neverSell.end(), item.name) == config.neverSell.end() && std::any_of(item.lines.begin(), item.lines.end(), [](const std::string& line) { return line.starts_with("Sell Price:"); }))
    {
        return true;
    }

    // A tooltip doesn't report the item class/subclass. Preserve reagents rather
    // than risk selling gathered leather, ore or herbs, even for an explicit sale.
    if (hasLine("Crafting Reagent") || hasLine("No sell price") || hasLine("Quest Item") || hasLine("Fishing Pole") || item.name == "Skinning Knife" || item.name == "Finkle's Skinner" || item.name == "Zulian Slicer" || item.name == "Gnomish Army Knife" || item.name == "Ruined Leather Scraps")
    {
        return false;
    }

    if (std::none_of(item.lines.begin(), item.lines.end(), [](const std::string& line) { return line.starts_with("Sell Price:"); }))
    {
        return false;
    }

    if (config.sellAllPoor && item.quality == 0)
    {
        return true;
    }

    if (std::find(config.neverSell.begin(), config.neverSell.end(), item.name) != config.neverSell.end())
    {
        return false;
    }

    return std::find(config.alwaysSell.begin(), config.alwaysSell.end(), item.name) != config.alwaysSell.end() || (item.quality == 0 && config.sellPoor) || (item.quality == 1 && config.sellCommon) || (item.quality == 2 && config.sellUncommon);
}

std::string luaQuote(const std::string& value)
{
    std::string out = "'";

    for (const char c : value)
    {
        if (c == '\'' || c == '\\')
        {
            out += '\\'; // "Arthas' Tears"
        }

        out += c;
    }

    return out + "'";
}

std::string buildSellScript(const VendorConfig& config, KeepGathered keep, LuaApi api)
{
    if (config.sellsNothing())
    {
        return {};
    }

    keep.leather = true; // permanent protection, even when skinning is disabled

    // What the enabled professions bring in. 'Leather' is an armour subclass as
    // well, so the item class has to match too or every leather vest would be
    // kept. Skinning tools and materials outrank every user sale rule.
    std::string keepLocals;
    std::string keepClause;
    std::string infoReturns = "local n,_,q=";
    const bool vanilla = api == LuaApi::Vanilla;

    if (vanilla)
    {
        keepClause = "c~='Trade Goods' and ";
        infoReturns = "local n,_,q,_,c=";
    }
    else
    {
        std::vector<std::string> subclasses;

        // A category sold is not kept, profession or not.
        if (keep.herbs && !config.sellHerbalism)
        {
            subclasses.emplace_back("Herb");
        }

        if (keep.ore)
        {
            subclasses.emplace_back("Metal & Stone");
        }

        if (keep.leather && !config.sellSkinning)
        {
            subclasses.emplace_back("Leather");
        }

        keepLocals = "local S=" + luaSet(subclasses) + ";";
        keepClause = "not (c=='Trade Goods' and S[sc]) and ";
        infoReturns = "local n,_,q,_,_,c,sc=";
    }

    // Whole categories sold (Sell Cooking-stuff, Skinning Stuff, Herbalism
    // Stuff). Where the subclass is told: Meat, Leather, Herb. 1.x has none,
    // so there by name, as on the tooltip clients - kept short, its script
    // space is small.
    std::string category = "false";

    if (config.sellsCategory())
    {
        std::vector<std::string> parts;

        if (!vanilla)
        {
            std::vector<std::string> sub;

            if (config.sellCooking)
            {
                sub.emplace_back("Meat"), sub.emplace_back("Cooking");
            }

            if (config.sellSkinning)
            {
                sub.emplace_back("Leather");
            }

            if (config.sellHerbalism)
            {
                sub.emplace_back("Herb");
            }

            keepLocals += "local Y=" + luaSet(sub) + ";";
            parts.emplace_back("(c=='Trade Goods' and Y[sc])");
        }
        else
        {
            if (config.sellSkinning)
            {
                parts.emplace_back("(c=='Trade Goods' and (string.find(n,'Leather') or string.find(n,' Hide$') or "
                                   "string.find(n,' Scale$') or string.find(n,'Dragonscale')))");
            }

            if (config.sellHerbalism)
            {
                std::vector<std::string> herbs(std::begin(kHerbs), std::begin(kHerbs) + 30);
                keepLocals += "local H=" + luaSet(herbs) + ";";
                parts.emplace_back("(c=='Trade Goods' and H[n])");
            }

            if (config.sellCooking)
            {
                parts.emplace_back("(c=='Trade Goods' and (string.find(n,'^Raw ') or string.find(n,' Meat$') or "
                                   "string.find(n,' Flank$') or string.find(n,' Egg$') or string.find(n,' Liver$')))");
            }
        }

        if (!vanilla && config.sellCooking)
        {
            parts.emplace_back("(c=='Trade Goods' and string.find(n,'^Raw '))");
        }

        category = "(";

        for (std::size_t j = 0; j < parts.size(); ++j)
        {
            category += (j ? " or " : "") + parts[j];
        }

        category += ")";
    }

    // Qualities to sell, as a table the loop can index by the item's quality.
    std::string qualities = "{";
    bool first = true;
    const std::pair<bool, int> byQuality[] = {{config.sellPoor, 0}, {config.sellCommon, 1}, {config.sellUncommon, 2}};

    for (const auto& [on, quality] : byQuality)
    {
        if (!on)
        {
            continue;
        }

        if (!first)
        {
            qualities += ',';
        }

        qualities += "[" + std::to_string(quality) + "]=1";
        first = false;
    }

    qualities += "}";

    // The 64-bit clients keep the bag calls under C_Container.
    const std::string bagCalls = api == LuaApi::Modern ? "local C=C_Container or {};local N,L,U=C.GetContainerNumSlots or GetContainerNumSlots,"
                                                         "C.GetContainerItemLink or GetContainerItemLink,C.UseContainerItem or UseContainerItem;"
                                                       : "local N,L,U=GetContainerNumSlots,GetContainerItemLink,UseContainerItem;";

    // 1.x's GetItemInfo does not take a link, only the id inside it.
    const std::string lookup = "local _,_,i=string.find(l,'item:(%d+)') i=tonumber(i or 0) " + infoReturns + (vanilla ? "GetItemInfo(i)" : "GetItemInfo(l)");

    // G: sell every grey, keep-list and all. K: never sell, wins over the rest.
    // A: sell whatever the quality. Q: sell by quality. A missing name means
    // GetItemInfo has not cached the item yet, so it is left alone rather than
    // guessed at.
    return "local G=" + std::string(config.sellAllPoor ? "true" : "false") + ";local K=" + luaSet(config.neverSell) + ";local A=" + luaSet(config.alwaysSell) + ";local P={[7005]=1,[12709]=1,[19901]=1,[40772]=1,[2934]=1};" + keepLocals + "local Q=" + qualities + ";" + bagCalls + "for b=0,4 do for s=1,N(b) do local l=L(b,s) if l then " + lookup + " if i>0 and n and c and " + (vanilla ? "" : "sc and ") + "not P[i] and ((not K[n] and " + category + ") or (" + keepClause + "((G and q==0) or (not K[n] and (A[n] or Q[q]))))) then U(b,s) end end end end";
}

std::string buildRepairScript(const VendorConfig& config)
{
    if (!config.repair)
    {
        return {};
    }

    return "if CanMerchantRepair() then RepairAllItems() end";
}

} // namespace og
