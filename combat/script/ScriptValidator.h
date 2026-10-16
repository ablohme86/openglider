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
 *  File: ScriptValidator.h
 *
 *  Desc:
 *  Declarations and interface for ScriptValidator (combat execution,
 *  rotation scripting, and class behaviors).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"
#include "og/combat/script/ScriptAst.h"

#include <set>
#include <string>
#include <vector>

namespace og::script
{

inline const std::set<std::string>& knownBlocks()
{
    static const std::set<std::string> blocks{"emergency", "pull", "combat", "rest", "buffs"};
    return blocks;
}

// Static checks: unknown names/members/functions, argument counts, unbound
// actions (if `actions` is given), unknown blocks.
std::vector<Diagnostic> validate(const Program& program, const ActionMap* actions);

} // namespace og::script
