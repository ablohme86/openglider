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
#include "og/state/UnitState.h"

#include <cstdint>
#include <string>

namespace og::wowclient
{

// What a game object is to the bot, from its GAMEOBJECT_TYPE and its name:
// chests, herbs and mining veins share one type and differ only by name.
GameObjectKind classifyGameObject(std::uint8_t type, const std::string& name);
// A transport - a lift's car - is kept in sight at any distance: riding one,
// the client gives the character's place on it, not in the world, and the
// car fell out of the objects in sight of that, the bot never knowing it
// had arrived.
bool keptAtAnyDistance(const GameObjectState& object);

} // namespace og::wowclient
