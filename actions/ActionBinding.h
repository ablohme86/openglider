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
 *  File: ActionBinding.h
 *
 *  Desc:
 *  Declarations and interface for ActionBinding (semantic game actions,
 *  action configuration, and execution).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/input/Key.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

// Maps one semantic action ("Warrior.Rend") to the key chord that performs it
// in the game, plus timing rules.
// How far a target may be for an action to work at all. `max` of 0 means the
// action has no range configured and is never gated on distance; `min` is for
// the abilities a target can be too close for, like Charge.
struct ActionRange
{
    float min = 0.0f;
    float max = 0.0f;
    bool limited() const
    {
        return max > 0.0f;
    }

    bool covers(float distance) const
    {
        return !limited() || (distance >= min && distance <= max);
    }
};

struct ActionBinding
{
    std::string name;
    KeyChord chord;
    Duration cooldown{0}; // action-specific minimum time between executions
    bool onGcd = false;   // triggers and is blocked by the global cooldown (spells)
    Duration castTime{0}; // time required to cast; movement must stop while casting
    ActionRange range{};  // yards; the rule engine skips a rule whose target is outside it
    // Action bar slot (1-120) the action sits in, when detected from the client.
    // A binding with a slot but no key is pressed with UseAction() through Lua.
    int actionSlot = 0;
    // The item an item slot holds (Player.Eat, Player.Drink); 0 for anything else.
    std::uint32_t itemId = 0;
    // Other bar slots holding something for the same action (another food,
    // say): the one with the most left in the bags is pressed.
    struct ItemSlot
    {
        KeyChord chord;
        int actionSlot = 0;
        std::uint32_t itemId = 0;
    };
    std::vector<ItemSlot> itemAlternatives;
    // Explicit slash command for an action without a key or bar slot (e.g. /petattack).
    std::string chatCommand;
    // The spell (its rank) on the action bar slot, where detection found one:
    // what it costs comes from the client's SpellPower table (SpellCosts).
    std::uint32_t spellId = 0;
    // A spell that can be cast on others (a heal, a shield): a party healer's
    // emergency rules use it on the party members too.
    bool friendly = false;
};

// Well-known semantic actions used by the engine itself. Combat classes and
// profiles may use any other names, as long as they are bound in actions.yaml.
namespace actions
{
inline constexpr std::string_view MoveForward = "Move.Forward";
inline constexpr std::string_view MoveBackward = "Move.Backward";
inline constexpr std::string_view TurnLeft = "Move.TurnLeft";
inline constexpr std::string_view TurnRight = "Move.TurnRight";
inline constexpr std::string_view StrafeLeft = "Move.StrafeLeft";
inline constexpr std::string_view StrafeRight = "Move.StrafeRight";
inline constexpr std::string_view Jump = "Move.Jump";
// In the water: up (the client's Jump) and down (its Sit/Move Down), held.
inline constexpr std::string_view SwimUp = "Move.SwimUp";
inline constexpr std::string_view SwimDown = "Move.SwimDown";

inline constexpr std::string_view TargetNext = "Target.Next";
inline constexpr std::string_view TargetClear = "Target.Clear";

inline constexpr std::string_view Interact = "Player.Interact";
inline constexpr std::string_view ReleaseSpirit = "Player.ReleaseSpirit";
inline constexpr std::string_view RetrieveCorpse = "Player.RetrieveCorpse";
inline constexpr std::string_view Sit = "Player.Sit";
inline constexpr std::string_view Eat = "Player.Eat";
inline constexpr std::string_view Drink = "Player.Drink";
inline constexpr std::string_view CameraResetView = "Camera.ResetView";
// The Fishing spell, as action bar detection names it, and putting a lure on the pole.
inline constexpr std::string_view Fish = "Fishing";
// The key the player named for Fishing (the Fishing window); wins over Fish.
inline constexpr std::string_view FishCast = "Fishing.Cast";
inline constexpr std::string_view FishingLure = "Fishing.Lure";
} // namespace actions

class ActionMap
{
public:
    Duration globalCooldown{1500};

    void add(ActionBinding binding);
    bool remove(std::string_view name);
    const ActionBinding* find(std::string_view name) const;
    ActionBinding* find(std::string_view name);
    bool contains(std::string_view name) const
    {
        return find(name) != nullptr;
    }

    std::size_t size() const
    {
        return bindings_.size();
    }

    const std::map<std::string, ActionBinding, std::less<>>& bindings() const
    {
        return bindings_;
    }

    // First action bound to this chord, if any. If preferredPrefix is given,
    // matches starting with it (or standard "Move."/"Target."/"Player.") take priority.
    std::optional<std::string> actionForChord(const KeyChord& chord, std::string_view preferredPrefix = "") const;
    std::vector<std::string> actionsForKey(Key key) const;

    // How a spell behaves - cooldown, global cooldown, cast time, range - for
    // actions with no key of their own: actions.yaml lists them so that the
    // key found on the action bar gets them. Not bound, so not pressable.
    void addInfo(ActionBinding info);
    // The binding's own settings, else the spell's info; nullptr for neither.
    const ActionBinding* settingsFor(std::string_view name) const;
    // Castable on others (friendly in actions.yaml), by the binding or the
    // spell's info - an action found on the action bar has only the latter.
    bool isFriendly(std::string_view name) const;
    const std::map<std::string, ActionBinding, std::less<>>& info() const
    {
        return info_;
    }

private:
    std::map<std::string, ActionBinding, std::less<>> bindings_;
    std::map<std::string, ActionBinding, std::less<>> info_;
};

} // namespace og
