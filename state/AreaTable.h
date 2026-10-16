/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-01
 *  File: AreaTable.h
 *
 *  Desc:
 *  Declarations and interface for AreaTable (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include <cstdint>
#include <unordered_map>

namespace og
{

// Returns the canonical/enclosing parent zone ID (AreaTable ID) for an area or subarea.
// For example, Valley of Trials (363) -> Durotar (14),
// Coldridge Valley (132) -> Dun Morogh (1),
// Northshire Valley (9) -> Elwynn Forest (12).
// Returns the input zoneId if it is already a top-level zone or not recognized as a subarea.
std::uint32_t canonicalZone(std::uint32_t zoneId);

// The quest data's subzone table (Questie's, area -> its zone), looked in
// before the built-in one: that one is a short list, and a subzone missing
// from it left the map unknown - a ghost by Moonbrook (area 919, Westfall)
// on a client whose map global is not known. Safe to call while other
// threads look zones up.
void setSubzoneParents(std::unordered_map<std::uint32_t, std::uint32_t> parents);

} // namespace og
