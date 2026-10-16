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
 *  File: VendorScript.h
 *
 *  Desc:
 *  Declarations and interface for VendorScript (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/core/BotConfig.h"

#include <string>
#include <string_view>

namespace og
{

struct UiItemTooltip;
// Tooltip-only clients keep all crafting reagents: their exact subclass isn't shown.
bool shouldSellTooltip(const VendorConfig& config, const UiItemTooltip& item);

// The categories "Sell Cooking-stuff", "Sell Skinning Stuff" and "Sell
// Herbalism Stuff" sell, told by name - the tooltip has no item class.
// `reagent`: the item is a trade good (a tooltip's "Crafting Reagent").
enum class SellCategory
{
    None,
    Cooking,
    Skinning,
    Herbalism
};
SellCategory sellCategory(std::string_view name, bool reagent);
bool sellsByCategory(const VendorConfig& config, std::string_view name, bool reagent);

// Item categories the bot harvests itself. Selling what a profession just
// gathered defeats the point of running it, so these survive the sale whatever
// the quality rules say - including "sell every grey", which otherwise ignores
// the never-sell list (Ruined Leather Scraps are grey).
//
// Skinning tools and leather are always protected, even with skinning disabled.
// Matched on the item's class and subclass from GetItemInfo rather than a list
// of names, so it covers every herb, ore and hide in the game. Like the
// never-sell list, it assumes an English client.
struct KeepGathered
{
    bool herbs = false;  // Herbalism
    bool ore = false;    // Mining
    bool leather = true; // Always enforced by buildSellScript, including when false is passed.
    bool none() const
    {
        return !herbs && !ore && !leather;
    }
};

// Which client's Lua the script is written for.
enum class LuaApi
{
    Wrath,   // 3.3.5a: GetItemInfo takes a link and reports item level and required level
    Vanilla, // 1.12.1: Lua 5.0, GetItemInfo takes an item id and has no item level
    Modern,  // 1.15 / 2.5.6: as Wrath, with the bag calls under C_Container
};

// Builds the Lua the bot runs at an open merchant window to sell what the
// user's rules allow. Selling is done in the client rather than in C++ because
// only the client knows an item's quality and name without a DBC reader.
//
// The script is deliberately one line and free of newlines: it travels through
// a fixed-size buffer in the client's memory (see LuaExecutor).
//
// 1.x has no Herb, Metal & Stone or Leather subclasses - they are all plain
// Trade Goods - so there every Trade Goods item is kept.
std::string buildSellScript(const VendorConfig& config, KeepGathered keep = {}, LuaApi api = LuaApi::Wrath);

// The repair-all call, or empty when repairing is off.
std::string buildRepairScript(const VendorConfig& config);

// Escapes a string for embedding in Lua single quotes.
std::string luaQuote(const std::string& value);

} // namespace og
