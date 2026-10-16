/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-21
 *  File: ZoneMaps.h
 *
 *  Desc:
 *  Declarations and interface for ZoneMaps (WoW client memory reading,
 *  process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <cstdint>
#include <optional>

namespace og::wowclient
{

// The map (continent) an outdoor zone or city lies on, from AreaTable: 0
// Eastern Kingdoms, 1 Kalimdor, 530 Outland and the TBC starting isles.
// nullopt for subzones, instances and anything else not listed.
std::optional<std::uint32_t> mapForZone(std::uint32_t zoneId);

} // namespace og::wowclient
