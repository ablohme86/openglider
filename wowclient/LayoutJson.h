/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-15
 *  File: LayoutJson.h
 *
 *  Desc:
 *  Declarations and interface for LayoutJson (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/common/Json.h"
#include "og/wowclient/ModernLayout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A 64-bit client's layout as the license server hands it out, and as
// a layout-json command writes it for the server:
//
//   { "name": "TBC Anniversary", "clientVersion": "2.5.6", "build": 69795,
//     "readOnly": true, "factionTable": "2.5.6",
//     "modern": { "objectManager": "0x1234560", ..., "inventoryBagFirst": 30 } }
//
// The name is the product, the build what a client is matched by, and the
// optional clientVersion what WoW.exe reports for it, to check against.
//
// Offsets are hex strings or plain numbers. When Blizzard patches a client,
// probe the new build, write its layout out and load it into the server: the
// bot picks it up at the next validation, no new release needed.
namespace og::wowclient
{

struct ClientLayout;
struct FactionTemplate;

using FactionTable = const FactionTemplate* (*)(std::uint32_t id);

struct LayoutDocument
{
    std::string name;          // "WoW Forever"
    std::string clientVersion; // "1.60.1"; may be empty
    std::uint32_t build = 0;
    // The built-in FactionTemplate table the build reads reactions from. A
    // patch rarely changes factions, so a new build can name the old table.
    std::string factionTable;
    ModernLayout modern;
};

// nullopt, with the reason in `error`, when the document is not a layout: no
// name or build, a required offset missing, or a value that is not a number.
std::optional<LayoutDocument> layoutFromJson(const Json& json, std::string* error = nullptr);
Json layoutToJson(const LayoutDocument& document);
// The document for a compiled-in 64-bit layout; nullopt for a 32-bit one.
std::optional<LayoutDocument> documentFor(const ClientLayout& layout);

// A built-in FactionTemplate table by name ("2.5.6"); nullptr when there is none.
FactionTable factionTableByName(std::string_view name);

// The product a 64-bit client version belongs to ("WoW Forever" for 1.60.x),
// for a layout found on a new build; empty for one not known.
std::string productName(int major, int minor);
// What a layout is saved as: "layout-wowforever.json".
std::string layoutFileName(const LayoutDocument& document);

} // namespace og::wowclient
