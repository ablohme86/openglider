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
 *  File: IInputBackend.h
 *
 *  Desc:
 *  Declarations and interface for IInputBackend (low-level OS and Wine
 *  input simulation backends).
 *
 *
 */
#pragma once

#include "og/input/Key.h"

#include <optional>
#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace og
{

enum class MouseButton : std::uint8_t
{
    Left,
    Right,
    Middle
};

// Low-level input sink. Implementations deliver input to the game window,
// ideally without requiring it to be in the foreground:
//
//   VirtualInputBackend    records input (tests, simulation)
//   Win32InputBackend      PostMessage to the WoW window (Windows)
//   WineBridgeInputBackend forwards to og-inputbridge.exe running inside the
//                          wine prefix, which PostMessages to WoW (Linux)
//   X11InputBackend        XSendEvent to the wine X11 window (experimental)
//
// The public API is non-virtual so that held-key tracking is always correct;
// backends implement the do* hooks.
class IInputBackend
{
public:
    virtual ~IInputBackend() = default;

    virtual std::string name() const = 0;
    virtual bool isConnected() const
    {
        return true;
    }

    void keyDown(Key key);
    void keyUp(Key key);
    // Modifiers down, key down, key up, modifiers up.
    void press(const KeyChord& chord);
    void press(Key key)
    {
        press(KeyChord{key, ModNone});
    }

    void mouseMove(int x, int y)
    {
        doMouseMove(x, y);
    }

    void click(MouseButton button)
    {
        doClick(button);
    }

    void mouseDown(MouseButton button)
    {
        doMouseDown(button);
    }

    void mouseUp(MouseButton button)
    {
        doMouseUp(button);
    }

    void mouseScroll(int delta)
    {
        doMouseScroll(delta);
    }

    // Characters straight into the game's focused edit box (the chat), apart
    // from the keyboard layout and modifiers. False where the backend cannot.
    bool typeText(std::string_view text)
    {
        return doTypeText(text);
    }

    virtual bool canTypeText() const
    {
        return false;
    }

    // The game window's client area in pixels, which mouse coordinates count
    // in; nullopt where the backend cannot tell.
    virtual std::optional<std::pair<int, int>> windowSize()
    {
        return std::nullopt;
    }

    bool isHeld(Key key) const
    {
        return held_.contains(key);
    }

    const std::set<Key>& heldKeys() const
    {
        return held_;
    }

    void releaseAll();

    // Before the chat is opened: every key let go of, and quiet for a while.
    // A held key repeats (the input bridge's typematic, the X server's
    // autorepeat), and the client turns a key-down into its character only as
    // it gets to the message: a repeat of W sent just before the release
    // became a "w" typed into the chat the Enter after it had opened, and a
    // "/script" line went out as "w/script ..." in /say.
    void settleBeforeTyping();
    // How long no key may have gone down or up before the chat is opened.
    virtual std::chrono::milliseconds typingSettle() const
    {
        return std::chrono::milliseconds(300);
    }

protected:
    virtual void doKeyDown(Key key) = 0;
    virtual void doKeyUp(Key key) = 0;
    virtual void doMouseMove(int x, int y) = 0;
    virtual void doClick(MouseButton button) = 0;
    virtual void doMouseDown(MouseButton button)
    {
        doClick(button);
    }

    virtual void doMouseUp(MouseButton button)
    {
        (void)button;
    }

    virtual void doMouseScroll(int delta)
    {
        (void)delta;
    }

    virtual bool doTypeText(std::string_view text)
    {
        (void)text;
        return false;
    }

private:
    std::set<Key> held_;
    std::chrono::steady_clock::time_point lastKeyEvent_{};
};

} // namespace og
