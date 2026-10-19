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
 *  File: IActionService.h
 *
 *  Desc:
 *  Declarations and interface for IActionService (semantic game
 *  actions, action configuration, and execution).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"
#include "og/actions/SpellCosts.h"

#include <functional>
#include "og/common/Clock.h"
#include "og/input/IInputBackend.h"

#include <optional>
#include <string>
#include <string_view>

namespace og
{

enum class ActionResult
{
    Executed,
    NotBound,
    OnCooldown,
    InputUnavailable
};

std::string_view toString(ActionResult r);

// Combat, navigation and behaviors express *what* they want to do. The action
// service decides *how* (which key, which backend).
//
//   actions.execute("Warrior.Rend");       // not: input.press(Key::D2)
class IActionService
{
public:
    virtual ~IActionService() = default;

    // `reason` (optional) is logged with the action: why it was chosen.
    virtual ActionResult execute(std::string_view action, std::string_view reason = {}) = 0;

    // Continuous actions (movement): key held until released.
    virtual void hold(std::string_view action) = 0;
    virtual void release(std::string_view action) = 0;
    virtual bool isHeld(std::string_view action) const = 0;
    virtual void releaseAll() = 0;

    // A deliberate keyboard pause while a typed chat command settles.
    virtual bool inputLocked() const
    {
        return false;
    }

    virtual bool hasBinding(std::string_view action) const = 0;
    // What the action's spell costs (SpellCosts), where it is known.
    virtual std::vector<SpellCost> costOf(std::string_view) const
    {
        return {};
    }

    // Can be cast on others (friendly in actions.yaml): a heal, a shield.
    virtual bool isFriendly(std::string_view) const
    {
        return false;
    }
    // Types a line into the chat and sends it (Enter, the text, Enter), as a
    // player typing a slash command would. False where the input cannot type.
    virtual bool chat(std::string_view line) = 0;
    // Runs Lua the way a player would type it: /script, split over several
    // lines and put back together in the client when it is longer than one
    // chat line takes. False where it cannot be typed.
    virtual bool chatScript(std::string_view lua) = 0;
    // Types what `action` needs in the client before its key works (see
    // ActionService::setPreparation), where that is due. True when a line was
    // typed: the key is then pressed a moment later. execute() does it too;
    // called ahead where the press itself must not wait (a bite).
    virtual bool prepare(std::string_view action)
    {
        (void)action;
        return false;
    }
    // Bound and not on (action or global) cooldown.
    virtual bool isReady(std::string_view action) const = 0;
    // Bound and off its own cooldown; ignores the shared global cooldown.
    virtual bool isReadyIgnoringGlobalCooldown(std::string_view action) const = 0;
    // A spell was pressed less than a global cooldown ago.
    virtual bool onGlobalCooldown() const = 0;
    // Forgets an action's cooldown so it can be issued again right away. For
    // callers that can tell the previous press had no effect (an item used
    // without gaining its buff, say).
    virtual void clearCooldown(std::string_view action) = 0;
    // Treats an action as on cooldown for `duration`, for callers that can tell
    // pressing it would only be refused (a buff a stronger one blocks, say).
    // clearCooldown() lets it go early.
    virtual void holdBack(std::string_view action, Duration duration) = 0;
    // Starts the global cooldown without pressing a spell that has one, for a
    // press the client gives one to that actions.yaml does not (stopping the
    // wand). Returns its length.
    virtual Duration startGlobalCooldown() = 0;
    // The client answered the last spell press with "Spell is not ready yet",
    // so it did nothing. For up to a second after the first refusal the spell
    // and the global cooldown count as ready again, so the rotation keeps
    // pressing it instead of waiting out a cooldown that never started.
    virtual void spellNotReady() = 0;
    // The client answered the last spell press with "Out of range", "Target
    // too close" or "Target not in line of sight". Nothing started - no cast, no GCD - but pressing it
    // again from here would fail the same way, so the spell is held back
    // briefly: the rotation moves on and the pull approach closes in.
    virtual void spellOutOfReach() = 0;

    virtual std::optional<std::string> lastAction() const = 0;

    // Spell cast-time tracking:
    virtual Duration castTime(std::string_view action) const = 0;
    // Distance band the action works at; an unbound or unranged action returns
    // an empty band, which covers every distance.
    virtual ActionRange range(std::string_view action) const = 0;
    // The item the action's bar slot holds, or 0 when it is not an item slot.
    virtual std::uint32_t itemId(std::string_view action) const
    {
        (void)action;
        return 0;
    }

    // For an item action on several bar slots: switch to the slot whose item
    // `count` says the bags hold most of. Returns that count (0: none left
    // anywhere, or not an item action).
    virtual int chooseItemSlot(std::string_view action, const std::function<int(std::uint32_t)>& count)
    {
        (void)action;
        (void)count;
        return 0;
    }

    virtual bool isCasting() const = 0;
    virtual Duration castingTimeRemaining() const = 0;
    virtual std::optional<std::string> castingAction() const = 0;
    virtual void cancelCasting() = 0;

    // Mouse operations for UI and world-space targeting:
    virtual void mouseMove(int x, int y)
    {
        (void)x;
        (void)y;
    }

    virtual void click(MouseButton button = MouseButton::Left)
    {
        (void)button;
    }

    virtual void mouseDown(MouseButton button = MouseButton::Left)
    {
        (void)button;
    }

    virtual void mouseUp(MouseButton button = MouseButton::Left)
    {
        (void)button;
    }

    virtual void mouseScroll(int delta)
    {
        (void)delta;
    }

    // False for a moment after a chat line, when mouse input is held back.
    virtual bool mouseFree() const
    {
        return true;
    }

    // The game window's client area in pixels, where the input can tell.
    virtual std::optional<std::pair<int, int>> windowSize()
    {
        return std::nullopt;
    }
};

} // namespace og
