/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-13
 *  File: SpellTooltip.h
 *
 *  Desc:
 *  Declarations and interface for SpellTooltip (WoW client memory reading,
 *  process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"
#include "og/common/Clock.h"

#include <optional>
#include <span>
#include <string>

namespace og::wowclient
{

// What a spell's tooltip says of casting it, as the client shows it to the
// player: the cast time with talents counted in (Improved Corruption's
// "Instant"), which no table of the client holds. An English client's
// wording; a line worded otherwise leaves its value unknown.
struct SpellTooltipData
{
    // "10 sec cooldown", "2 min cooldown"
    std::optional<Duration> cooldown;
    // "2 sec cast", "Instant"; unknown for a channelled spell, whose line
    // gives no time.
    std::optional<Duration> castTime;
    // "30 yd range", "8-30 yd range", "Melee Range".
    std::optional<ActionRange> range;
};

SpellTooltipData parseSpellTooltip(std::span<const std::string> lines);

} // namespace og::wowclient
