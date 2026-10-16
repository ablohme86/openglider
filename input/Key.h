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
 *  File: Key.h
 *
 *  Desc:
 *  Declarations and interface for Key (low-level OS and Wine input
 *  simulation backends).
 *
 *
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace og
{

enum class Key : std::uint16_t
{
    None = 0,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    D0,
    D1,
    D2,
    D3,
    D4,
    D5,
    D6,
    D7,
    D8,
    D9,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Numpad0,
    Numpad1,
    Numpad2,
    Numpad3,
    Numpad4,
    Numpad5,
    Numpad6,
    Numpad7,
    Numpad8,
    Numpad9,
    NumpadAdd,
    NumpadSubtract,
    NumpadMultiply,
    NumpadDivide,
    NumpadDecimal,
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    Up,
    Down,
    Left,
    Right,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Semicolon,
    Apostrophe,
    Comma,
    Period,
    Slash,
    Backslash,
    Grave,
    Shift,
    Control,
    Alt,
    Count_
};

enum Modifier : std::uint8_t
{
    ModNone = 0,
    ModShift = 1 << 0,
    ModCtrl = 1 << 1,
    ModAlt = 1 << 2,
};

std::string_view keyName(Key key);
std::optional<Key> parseKey(std::string_view name);
bool isModifierKey(Key key);

// Windows virtual-key code for a key (also used by the wine input bridge).
std::uint8_t toWindowsVirtualKey(Key key);
bool isExtendedKey(Key key);

// A key plus modifiers, e.g. "SHIFT+1", "CTRL+ALT+F5", "NUMPAD3".
struct KeyChord
{
    Key key = Key::None;
    std::uint8_t modifiers = ModNone;

    bool valid() const
    {
        return key != Key::None;
    }

    std::string toString() const;
    static std::optional<KeyChord> parse(std::string_view text);
    // A binding as the game's GetBindingKey() spells it: "SHIFT-1", "ALT-CTRL-F5",
    // "SHIFT--". Mouse buttons and other keys the bot cannot press give nullopt.
    static std::optional<KeyChord> parseWow(std::string_view text);

    bool operator==(const KeyChord&) const = default;
};

} // namespace og
