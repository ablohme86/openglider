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
 *  File: ItemNames.h
 *
 *  Desc:
 *  Declarations and interface for ItemNames (WoW client memory reading,
 *  process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <cstdint>
#include <string>

namespace og::wowclient
{

// Item names for bag contents ("player.itemCount('Conjured Water')"). The
// client only exposes item ids without reading its item cache, so a small
// built-in table covers the items scripts ask about; any other item can be
// checked by id.
const std::string* itemName(std::uint32_t itemId);
bool isWaterItem(std::uint32_t itemId);
bool isFoodItem(std::uint32_t itemId);

} // namespace og::wowclient
