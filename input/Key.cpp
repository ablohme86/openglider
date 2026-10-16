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
 *  File: Key.cpp
 *
 *  Desc:
 *  Implementation of Key (low-level OS and Wine input simulation
 *  backends).
 *
 *
 */
#include "og/input/Key.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace og
{

namespace
{

struct KeyInfo
{
    Key key;
    std::string_view name;
    std::uint8_t vk;
};

constexpr std::array kKeys = {
    KeyInfo{Key::A, "A", 0x41}, KeyInfo{Key::B, "B", 0x42}, KeyInfo{Key::C, "C", 0x43}, KeyInfo{Key::D, "D", 0x44}, KeyInfo{Key::E, "E", 0x45}, KeyInfo{Key::F, "F", 0x46}, KeyInfo{Key::G, "G", 0x47}, KeyInfo{Key::H, "H", 0x48}, KeyInfo{Key::I, "I", 0x49}, KeyInfo{Key::J, "J", 0x4A}, KeyInfo{Key::K, "K", 0x4B}, KeyInfo{Key::L, "L", 0x4C}, KeyInfo{Key::M, "M", 0x4D}, KeyInfo{Key::N, "N", 0x4E}, KeyInfo{Key::O, "O", 0x4F}, KeyInfo{Key::P, "P", 0x50}, KeyInfo{Key::Q, "Q", 0x51}, KeyInfo{Key::R, "R", 0x52}, KeyInfo{Key::S, "S", 0x53}, KeyInfo{Key::T, "T", 0x54}, KeyInfo{Key::U, "U", 0x55}, KeyInfo{Key::V, "V", 0x56}, KeyInfo{Key::W, "W", 0x57}, KeyInfo{Key::X, "X", 0x58}, KeyInfo{Key::Y, "Y", 0x59}, KeyInfo{Key::Z, "Z", 0x5A}, KeyInfo{Key::D0, "0", 0x30}, KeyInfo{Key::D1, "1", 0x31}, KeyInfo{Key::D2, "2", 0x32}, KeyInfo{Key::D3, "3", 0x33}, KeyInfo{Key::D4, "4", 0x34}, KeyInfo{Key::D5, "5", 0x35}, KeyInfo{Key::D6, "6", 0x36}, KeyInfo{Key::D7, "7", 0x37}, KeyInfo{Key::D8, "8", 0x38}, KeyInfo{Key::D9, "9", 0x39}, KeyInfo{Key::F1, "F1", 0x70}, KeyInfo{Key::F2, "F2", 0x71}, KeyInfo{Key::F3, "F3", 0x72}, KeyInfo{Key::F4, "F4", 0x73}, KeyInfo{Key::F5, "F5", 0x74}, KeyInfo{Key::F6, "F6", 0x75}, KeyInfo{Key::F7, "F7", 0x76}, KeyInfo{Key::F8, "F8", 0x77}, KeyInfo{Key::F9, "F9", 0x78}, KeyInfo{Key::F10, "F10", 0x79}, KeyInfo{Key::F11, "F11", 0x7A}, KeyInfo{Key::F12, "F12", 0x7B}, KeyInfo{Key::Numpad0, "NUMPAD0", 0x60}, KeyInfo{Key::Numpad1, "NUMPAD1", 0x61}, KeyInfo{Key::Numpad2, "NUMPAD2", 0x62}, KeyInfo{Key::Numpad3, "NUMPAD3", 0x63}, KeyInfo{Key::Numpad4, "NUMPAD4", 0x64}, KeyInfo{Key::Numpad5, "NUMPAD5", 0x65}, KeyInfo{Key::Numpad6, "NUMPAD6", 0x66}, KeyInfo{Key::Numpad7, "NUMPAD7", 0x67}, KeyInfo{Key::Numpad8, "NUMPAD8", 0x68}, KeyInfo{Key::Numpad9, "NUMPAD9", 0x69}, KeyInfo{Key::NumpadAdd, "NUMPADPLUS", 0x6B}, KeyInfo{Key::NumpadSubtract, "NUMPADMINUS", 0x6D}, KeyInfo{Key::NumpadMultiply, "NUMPADMULTIPLY", 0x6A}, KeyInfo{Key::NumpadDivide, "NUMPADDIVIDE", 0x6F}, KeyInfo{Key::NumpadDecimal, "NUMPADDECIMAL", 0x6E}, KeyInfo{Key::Space, "SPACE", 0x20}, KeyInfo{Key::Enter, "ENTER", 0x0D}, KeyInfo{Key::Escape, "ESCAPE", 0x1B}, KeyInfo{Key::Tab, "TAB", 0x09}, KeyInfo{Key::Backspace, "BACKSPACE", 0x08}, KeyInfo{Key::Up, "UP", 0x26}, KeyInfo{Key::Down, "DOWN", 0x28}, KeyInfo{Key::Left, "LEFT", 0x25}, KeyInfo{Key::Right, "RIGHT", 0x27}, KeyInfo{Key::Insert, "INSERT", 0x2D}, KeyInfo{Key::Delete, "DELETE", 0x2E}, KeyInfo{Key::Home, "HOME", 0x24}, KeyInfo{Key::End, "END", 0x23}, KeyInfo{Key::PageUp, "PAGEUP", 0x21}, KeyInfo{Key::PageDown, "PAGEDOWN", 0x22}, KeyInfo{Key::Minus, "-", 0xBD}, KeyInfo{Key::Equals, "=", 0xBB}, KeyInfo{Key::LeftBracket, "[", 0xDB}, KeyInfo{Key::RightBracket, "]", 0xDD}, KeyInfo{Key::Semicolon, ";", 0xBA}, KeyInfo{Key::Apostrophe, "'", 0xDE}, KeyInfo{Key::Comma, ",", 0xBC}, KeyInfo{Key::Period, ".", 0xBE}, KeyInfo{Key::Slash, "/", 0xBF}, KeyInfo{Key::Backslash, "\\", 0xDC}, KeyInfo{Key::Grave, "`", 0xC0}, KeyInfo{Key::Shift, "SHIFT", 0x10}, KeyInfo{Key::Control, "CTRL", 0x11}, KeyInfo{Key::Alt, "ALT", 0x12},
};

struct Alias
{
    std::string_view alias;
    Key key;
};

constexpr std::array kAliases = {
    Alias{"ESC", Key::Escape}, Alias{"RETURN", Key::Enter}, Alias{"CONTROL", Key::Control}, Alias{"MINUS", Key::Minus}, Alias{"EQUALS", Key::Equals}, Alias{"PLUS", Key::Equals}, Alias{"COMMA", Key::Comma}, Alias{"PERIOD", Key::Period}, Alias{"SLASH", Key::Slash}, Alias{"BACKSLASH", Key::Backslash}, Alias{"SEMICOLON", Key::Semicolon}, Alias{"PGUP", Key::PageUp}, Alias{"PGDN", Key::PageDown}, Alias{"DEL", Key::Delete}, Alias{"INS", Key::Insert}, Alias{"UPARROW", Key::Up}, Alias{"DOWNARROW", Key::Down}, Alias{"LEFTARROW", Key::Left}, Alias{"RIGHTARROW", Key::Right},
};

std::string upper(std::string_view s)
{
    std::string out;
    out.reserve(s.size());

    for (char c : s)
    {
        if (c == ' ' || c == '\t')
        {
            continue;
        }

        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    return out;
}

const KeyInfo* info(Key key)
{
    for (const auto& k : kKeys)
    {
        if (k.key == key)
        {
            return &k;
        }
    }

    return nullptr;
}

} // namespace

std::string_view keyName(Key key)
{
    const KeyInfo* k = info(key);
    return k ? k->name : std::string_view("NONE");
}

std::optional<Key> parseKey(std::string_view name)
{
    const std::string u = upper(name);

    if (u.empty())
    {
        return std::nullopt;
    }

    for (const auto& k : kKeys)
    {
        if (k.name == u)
        {
            return k.key;
        }
    }

    for (const auto& a : kAliases)
    {
        if (a.alias == u)
        {
            return a.key;
        }
    }

    return std::nullopt;
}

bool isModifierKey(Key key)
{
    return key == Key::Shift || key == Key::Control || key == Key::Alt;
}

std::uint8_t toWindowsVirtualKey(Key key)
{
    const KeyInfo* k = info(key);
    return k ? k->vk : 0;
}

bool isExtendedKey(Key key)
{
    switch (key)
    {
    case Key::Up:
    case Key::Down:
    case Key::Left:
    case Key::Right:
    case Key::Insert:
    case Key::Delete:
    case Key::Home:
    case Key::End:
    case Key::PageUp:
    case Key::PageDown:
    case Key::NumpadDivide:
        return true;
    default:
        return false;
    }
}

std::string KeyChord::toString() const
{
    std::string out;

    if (modifiers & ModCtrl)
    {
        out += "CTRL+";
    }

    if (modifiers & ModAlt)
    {
        out += "ALT+";
    }

    if (modifiers & ModShift)
    {
        out += "SHIFT+";
    }

    out += keyName(key);
    return out;
}

std::optional<KeyChord> KeyChord::parse(std::string_view text)
{
    const std::string u = upper(text);

    if (u.empty())
    {
        return std::nullopt;
    }

    KeyChord chord;
    std::size_t start = 0;

    while (true)
    {
        // A '+' that is the last character is the key itself ("SHIFT++" is unusual,
        // but "+" alone maps to the equals/plus key via alias "PLUS").
        std::size_t plus = u.find('+', start);

        if (plus == std::string::npos || plus == u.size() - 1)
        {
            const std::string_view keyPart = std::string_view(u).substr(start);
            auto k = parseKey(keyPart == "+" ? std::string_view("PLUS") : keyPart);

            if (!k || isModifierKey(*k))
            {
                return std::nullopt;
            }

            chord.key = *k;
            return chord;
        }

        const std::string_view part = std::string_view(u).substr(start, plus - start);

        if (part == "SHIFT")
        {
            chord.modifiers |= ModShift;
        }
        else if (part == "CTRL" || part == "CONTROL")
        {
            chord.modifiers |= ModCtrl;
        }
        else if (part == "ALT")
        {
            chord.modifiers |= ModAlt;
        }
        else
        {
            return std::nullopt;
        }

        start = plus + 1;
    }
}

std::optional<KeyChord> KeyChord::parseWow(std::string_view text)
{
    static constexpr std::array<std::pair<std::string_view, std::uint8_t>, 3> kPrefixes{{{"ALT-", ModAlt}, {"CTRL-", ModCtrl}, {"SHIFT-", ModShift}}};
    KeyChord chord;

    for (bool stripped = true; stripped;)
    {
        stripped = false;

        for (const auto& [prefix, modifier] : kPrefixes)
        {
            // "SHIFT--" is SHIFT plus the minus key, so a prefix never takes the last character.
            if (text.size() > prefix.size() && text.starts_with(prefix))
            {
                chord.modifiers |= modifier;
                text.remove_prefix(prefix.size());
                stripped = true;
            }
        }
    }

    const auto key = parseKey(text);

    if (!key || isModifierKey(*key))
    {
        return std::nullopt;
    }

    chord.key = *key;
    return chord;
}

} // namespace og
