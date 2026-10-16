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
 *  File: SnapshotJson.h
 *
 *  Desc:
 *  Declarations and interface for SnapshotJson (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include "og/common/Json.h"
#include "og/state/GameSnapshot.h"

namespace og
{

// JSON (de)serialization of snapshots. Used by replay files (.glr), the daemon
// API and tests. GUIDs are written as hex strings to keep 64-bit precision.
Json toJson(const Vec3& v);
Vec3 vec3FromJson(const Json& j);

Json toJson(const Aura& a);
Json toJson(const UnitState& u);
Json toJson(const PlayerState& p);
Json toJson(const GameObjectState& o);
Json toJson(const GameSnapshot& s);

Aura auraFromJson(const Json& j);
UnitState unitFromJson(const Json& j);
PlayerState playerFromJson(const Json& j);
GameObjectState objectFromJson(const Json& j);
GameSnapshot snapshotFromJson(const Json& j);

Json guidToJson(Guid guid);
Guid guidFromJson(const Json& j);

} // namespace og
