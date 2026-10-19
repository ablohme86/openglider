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
 *  File: ICombatClass.h
 *
 *  Desc:
 *  Declarations and interface for ICombatClass (combat execution,
 *  rotation scripting, and class behaviors).
 *
 *
 */
#pragma once

#include "og/actions/IActionService.h"
#include "og/common/Log.h"
#include "og/state/GameSnapshot.h"

#include <cstdint>
#include <functional>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

struct CombatClassSettings
{
    float combatRange = 4.5f;     // stay within this distance while fighting
    float restHealth = 60.0f;     // start resting below this health %
    float restMana = 40.0f;       // start resting below this mana % (if usesMana)
    float restDoneHealth = 95.0f; // stop resting at/above
    float restDoneMana = 95.0f;
    bool usesMana = false;
};

// Spell ids for a spell name ("FrostArmor", "Frost Armor"); empty when the
// name is unknown. Used to skip rules for spells the character has not learned.
using SpellLookup = std::function<std::vector<std::uint32_t>(std::string_view spellName)>;

// A step a script asks for: player.WalkBackward(10) and the like. The combat
// class only asks; the fight (CombatBehavior) takes the step.
struct ScriptMove
{
    enum class Way
    {
        Forward,
        Backward,
        Left,
        Right
    };
    Way way = Way::Backward;
    float yards = 0;
    std::string rule; // as written, for the log
};

// The script's moves by name ("player.WalkBackward"), or nullopt.
inline std::optional<ScriptMove::Way> scriptMoveWay(std::string_view action)
{
    if (action == "player.WalkForward")
    {
        return ScriptMove::Way::Forward;
    }

    if (action == "player.WalkBackward")
    {
        return ScriptMove::Way::Backward;
    }

    if (action == "player.StrafeLeft")
    {
        return ScriptMove::Way::Left;
    }

    if (action == "player.StrafeRight")
    {
        return ScriptMove::Way::Right;
    }

    return std::nullopt;
}

// The most a script may ask to be walked in one step.
inline constexpr float kScriptMoveLongest = 40.0f;

struct CombatContext
{
    const GameSnapshot& snapshot;
    IActionService& actions;
    Logger& log;
    double combatSeconds = 0.0;               // time since the current fight started
    const SpellLookup* spellLookup = nullptr; // live game only
    // Spells a mob "casts" without a cast time (Greater Fleshripper's 12166):
    // target.isCasting leaves them out, there being nothing to interrupt.
    const std::set<std::uint32_t>* instantSpells = nullptr;
    // Where a rule's step is put for the fight to take; null where nothing moves.
    std::optional<ScriptMove>* moveOrder = nullptr;
};

// A combat class decides which semantic action to use in each phase. It never
// selects targets, moves, or reads the game directly - it only sees the
// snapshot in the context and emits actions.
//
// Every on* method returns true if an action was executed this tick.
class ICombatClass
{
public:
    virtual ~ICombatClass() = default;

    virtual std::string name() const = 0;
    virtual const CombatClassSettings& settings() const = 0;
    // Reach of the combat rotation on this character, capped by its configured
    // fighting distance. Pull-only spells must not make a melee rotation wait.
    virtual float combatRange(CombatContext& ctx)
    {
        (void)ctx;
        return settings().combatRange;
    }

    // How far the pull rules reach, from their spells' ranges: the pull
    // approach walks this close before it tries them.
    virtual float pullReach(CombatContext& ctx)
    {
        (void)ctx;
        return 5.0f;
    }

    // True when the rotation itself goes for a melee swing (an auto attack
    // rule), so the bot walks to the target instead of waiting for it to come.
    // Auto attack switched on by some other ability does not count.
    virtual bool wantsMeleeSwing(CombatContext& ctx)
    {
        (void)ctx;
        return false;
    }

    // Emergency (priority 1000): e.g. health potion at low health.
    // The actions a block's rules name ("pull": Warrior.Charge, ...), in order.
    virtual std::vector<std::string> blockActions(std::string_view) const
    {
        return {};
    }

    virtual bool wantsEmergency(CombatContext& ctx) = 0;
    virtual bool onEmergency(CombatContext& ctx) = 0;

    // Party system, a healer: the emergency rules whose action `allowed`
    // lets through (the spells that can be cast on others), with the context's
    // `player` being the party member to heal. True for one that would fire;
    // on... casts it.
    virtual bool wantsPartyHeal(CombatContext&, const std::function<bool(std::string_view)>&)
    {
        return false;
    }

    virtual bool onPartyHeal(CombatContext&, const std::function<bool(std::string_view)>&)
    {
        return false;
    }

    // True when a pull rule would fire right now - the approach can stop as
    // soon as the spell that will open the fight actually reaches.
    virtual bool wantsPull(CombatContext& ctx) = 0;
    virtual bool onPull(CombatContext& ctx) = 0;
    virtual void resetPull()
    {
    }

    // True when a combat rule would fire right now - asked with the mob moved,
    // to find where the rotation reaches when it fires nowhere (a hunter at 6
    // yards: Raptor Strike reaches 5, Auto Shot starts at 8).
    virtual bool wantsCombat(CombatContext& ctx)
    {
        (void)ctx;
        return false;
    }

    virtual bool onCombat(CombatContext& ctx) = 0;
    virtual bool onRest(CombatContext& ctx) = 0;

    // Out-of-combat buffs.
    virtual bool wantsBuff(CombatContext& ctx) = 0;
    virtual bool onBuff(CombatContext& ctx) = 0;
    virtual bool wantsInstantBuff(CombatContext& ctx)
    {
        (void)ctx;
        return false;
    }

    virtual bool onInstantBuff(CombatContext& ctx)
    {
        return onBuff(ctx);
    }

    virtual bool wantsStationaryBuff(CombatContext& ctx)
    {
        return wantsBuff(ctx) && !wantsInstantBuff(ctx);
    }
};

} // namespace og
