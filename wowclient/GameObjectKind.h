/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-14
 *  File: GameObjectKind.h
 *
 *  Desc:
 *  Declarations and interface for GameObjectKind (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/state/Types.h"

#include <cstdint>
#include <string>

namespace og::wowclient
{

// What a game object is to the bot, from its GAMEOBJECT_TYPE and its name:
// chests, herbs and mining veins share one type and differ only by name.
GameObjectKind classifyGameObject(std::uint8_t type, const std::string& name);

} // namespace og::wowclient
