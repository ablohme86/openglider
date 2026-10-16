/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-22
 *  File: SpecialBags.h
 *
 *  Desc:
 *  Declarations and interface for SpecialBags (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include <cstdint>

namespace og
{

// Whether a bag only holds one kind of item: herb, soul, enchanting, mining,
// gem and engineering bags, quivers and ammo pouches. Their empty slots take
// no loot, so they do not count as free bag space. The client keeps a bag's
// kind in its item template, not in memory we read, so this goes by item id.
bool isSpecialBag(std::uint32_t itemId);

} // namespace og
