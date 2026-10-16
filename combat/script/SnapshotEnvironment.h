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
 *  File: SnapshotEnvironment.h
 *
 *  Desc:
 *  Declarations and interface for SnapshotEnvironment (combat
 *  execution, rotation scripting, and class behaviors).
 *
 *
 */
#pragma once

#include "og/actions/IActionService.h"
#include "og/combat/ICombatClass.h"
#include "og/combat/script/ScriptInterpreter.h"
#include "og/state/GameSnapshot.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace og::script
{

using SettingsMap = std::map<std::string, Value, std::less<>>;

// Exposes the GameSnapshot to OGScript:
//
//   player.health  player.mana  player.rage  player.hasBuff("Battle Shout") ...
//   player.itemCount("Conjured Water")  player.hasSpell("Slice and Dice")
//   target.health  target.distance  target.hasDebuff("Rend")  target.comboPoints ...
//   pet.exists  pet.health
//   adds  attackers  combatTime  ready("Warrior.Charge")  enemiesNear(10)
//
// plus every value from the class' settings block.
class SnapshotEnvironment final : public IEnvironment
{
public:
    // spellLookup: the live game's spell table, for player.hasSpell("name").
    SnapshotEnvironment(const GameSnapshot& snapshot, const IActionService* actions, const SettingsMap* settings, double combatSeconds, SpellLookup spellLookup = {}, const std::set<std::uint32_t>* instantSpells = nullptr);

    std::optional<Value> name(std::string_view name) override;
    std::optional<Value> member(const ObjectRef& object, std::string_view member) override;
    Value call(const FunctionRef& fn, const std::vector<Value>& args) override;
    bool exists(const ObjectRef& object) override;

    // Reference tables used by validation and documentation.
    static const std::vector<std::string_view>& objectNames();
    static const std::vector<std::string_view>& globalValues();
    static const std::vector<std::string_view>& globalFunctions();
    static const std::vector<std::string_view>& unitMembers();
    static const std::vector<std::string_view>& playerMembers();
    // Only on target: what the character has built up on it.
    static const std::vector<std::string_view>& targetMembers();
    static const std::vector<std::string_view>& unitMethods();
    static const std::vector<std::string_view>& playerMethods();
    // Named values scripts compare with: Stance.Battle, Form.Cat.
    static const std::vector<std::string_view>& constantGroups();
    static std::vector<std::string_view> constantsOf(std::string_view group);

private:
    const UnitState* resolve(const ObjectRef& object) const;
    std::optional<Value> unitMember(const UnitState* unit, std::string_view m) const;
    std::optional<Value> playerMember(std::string_view m) const;
    bool knowsSpell(std::string_view nameOrId) const;

    const GameSnapshot& snapshot_;
    const IActionService* actions_;
    const SettingsMap* settings_;
    double combatSeconds_;
    SpellLookup spellLookup_;
    const std::set<std::uint32_t>* instantSpells_ = nullptr;
};

} // namespace og::script
