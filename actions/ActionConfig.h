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
 *  File: ActionConfig.h
 *
 *  Desc:
 *  Declarations and interface for ActionConfig (semantic game actions,
 *  action configuration, and execution).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"

#include <filesystem>
#include <string>
#include <vector>

namespace og
{

struct ActionConfigResult
{
    ActionMap map;
    std::vector<std::string> errors;
    bool ok() const
    {
        return errors.empty();
    }
};

// Parses actions.yaml:
//
//   globalCooldown: 1.5          # seconds
//   actions:
//     Warrior.Rend:
//       key: "2"
//       gcd: true                # on the global cooldown (spells/abilities)
//     Player.Eat:
//       key: "SHIFT+1"
//       cooldown: 20
//     Move.Forward: "W"          # short form: key only
ActionConfigResult parseActionConfig(const std::string& yamlText);
// Built-in spell settings and pet commands; no user-maintained keys.
ActionConfigResult defaultActionConfig();
ActionConfigResult loadActionConfig(const std::filesystem::path& path);

} // namespace og
