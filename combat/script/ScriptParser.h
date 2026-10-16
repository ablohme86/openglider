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
 *  File: ScriptParser.h
 *
 *  Desc:
 *  Declarations and interface for ScriptParser (combat execution,
 *  rotation scripting, and class behaviors).
 *
 *
 */
#pragma once

#include "og/combat/script/ScriptAst.h"

#include <optional>
#include <string_view>

namespace og::script
{

// OGScript - the OpenGlider combat class language.
//
//   # comment (also //)
//   class "Warrior"
//
//   settings {
//       combatRange = 5
//       restHealth = 55
//   }
//
//   combat {
//       Player.HealthPotion  if player.health < 20
//       Warrior.Rend         if not target.hasDebuff("Rend") and player.rage >= 10
//       Warrior.HeroicStrike if player.rage >= 40
//       Warrior.AutoAttack
//       #off Warrior.Charge  if target.distance > 8
//   }
//
// "#off " in front of a rule switches it off for now: it is a comment to the
// parser, and the class editor shows it as a rule with its box unticked.
//
// Each block is a priority list: every tick the first rule whose action is
// ready and whose condition is true is executed.
struct ParseResult
{
    std::optional<Program> program;
    std::vector<Diagnostic> errors;
    bool ok() const
    {
        return program.has_value() && errors.empty();
    }
};

ParseResult parse(std::string_view source);

} // namespace og::script
