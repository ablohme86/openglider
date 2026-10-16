/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-14
 *  File: WowBindings.cpp
 *
 *  Desc:
 *  Implementation of WowBindings (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/WowBindings.h"

#include "og/state/Shapeshift.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>

namespace og::wowclient
{

namespace
{

std::string upper(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// The client names punctuation keys by their character; KeyChord knows them by name.
std::string keyNameForChord(std::string key)
{
    if (key.empty())
    {
        return key;
    }

    const char last = key.back();
    const bool alone = key.size() == 1 || key[key.size() - 2] == '-';

    if (alone && (last == '+' || last == '-' || last == '='))
    {
        key.pop_back();
        key += last == '+' ? "PLUS" : last == '-' ? "MINUS" : "EQUALS";
    }

    return key;
}

} // namespace

WowBindings::WowBindings()
{
    // The client's defaults for the main bar: 1-9, 0, minus, equals.
    constexpr const char* kKeys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="};

    for (int i = 0; i < 12; ++i)
    {
        commandByKey_[kKeys[i]] = std::format("ACTIONBUTTON{}", i + 1);
    }

    // And the stance bar's: CTRL-F1 to CTRL-F10.
    for (int i = 1; i <= 10; ++i)
    {
        commandByKey_[std::format("CTRL-F{}", i)] = std::format("SHAPESHIFTBUTTON{}", i);
    }

    // Moving, jumping, swimming down (Sit/Move Down) and Tab targeting, as the
    // client has them out of the box.
    for (const auto& [key, command] : {std::pair{"W", "MOVEFORWARD"}, {"S", "MOVEBACKWARD"}, {"A", "TURNLEFT"}, {"D", "TURNRIGHT"}, {"Q", "STRAFELEFT"}, {"E", "STRAFERIGHT"}, {"SPACE", "JUMP"}, {"X", "SITORSTAND"}, {"TAB", "TARGETNEARESTENEMY"}, {"ESCAPE", "TOGGLEGAMEMENU"}})
    {
        commandByKey_[key] = command;
    }
}

void WowBindings::apply(std::string_view file)
{
    std::istringstream in{std::string(file)};
    std::string line;

    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        std::istringstream words(line);
        std::string verb, key, command;

        if (!(words >> verb >> key >> command) || verb != "bind")
        {
            continue;
        }

        commandByKey_[upper(key)] = upper(command); // "NONE" unbinds the key
    }
}

bool WowBindings::applyFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);

    if (!in)
    {
        return false;
    }

    std::ostringstream text;
    text << in.rdbuf();
    apply(text.str());
    return true;
}

std::optional<KeyChord> WowBindings::keyFor(std::string_view command) const
{
    const std::string wanted = upper(command);
    // A command can sit on two keys (W and UP for moving forward): a letter,
    // digit or plain key first, then chords, and arrows, the numpad and mouse
    // buttons last - the bridge and a human alike use the first.
    std::optional<KeyChord> best;
    int bestRank = 99;

    for (const auto& [key, bound] : commandByKey_)
    {
        if (bound != wanted)
        {
            continue;
        }

        auto chord = KeyChord::parseWow(keyNameForChord(key));

        if (!chord)
        {
            continue;
        }

        const bool awkward = key == "UP" || key == "DOWN" || key == "LEFT" || key == "RIGHT" || key.starts_with("NUMPAD") || key.starts_with("BUTTON") || key.starts_with("MOUSE");
        const int rank = awkward ? 2 : key.find('-') != std::string::npos ? 1 : 0;

        if (rank < bestRank)
        {
            best = chord;
            bestRank = rank;
        }
    }

    return best;
}

std::string WowBindings::commandForSlot(int slot)
{
    if (slot < 1 || slot > 72)
    {
        return {};
    }

    const int button = (slot - 1) % 12 + 1;

    switch ((slot - 1) / 12)
    {
    case 0:
        return std::format("ACTIONBUTTON{}", button);
    case 2:
        return std::format("MULTIACTIONBAR3BUTTON{}", button); // right
    case 3:
        return std::format("MULTIACTIONBAR4BUTTON{}", button); // right, second
    case 4:
        return std::format("MULTIACTIONBAR2BUTTON{}", button); // bottom right
    case 5:
        return std::format("MULTIACTIONBAR1BUTTON{}", button); // bottom left
    default:
        return {};
    }
}

namespace
{

struct CharacterFolders
{
    std::filesystem::path account;
    std::vector<std::filesystem::path> characters;
};

// The first account holding the character, and its folders there: the named
// realm's, or - with no realm, or one without the character - every realm's.
CharacterFolders findCharacter(const std::filesystem::path& installDirectory, std::string_view realm, std::string_view character)
{
    CharacterFolders found;

    if (character.empty())
    {
        return found;
    }

    std::error_code ec;

    for (const auto& account : std::filesystem::directory_iterator(installDirectory / "WTF" / "Account", ec))
    {
        // The realm name the 2.5.6 client keeps in memory is cleared once it is in
        // the world, so without it the character is looked for under every realm.
        const auto named = account.path() / std::string(realm) / std::string(character);

        if (!realm.empty() && std::filesystem::is_directory(named, ec))
        {
            found.characters.push_back(named);
        }
        else
        {
            for (const auto& realmDir : std::filesystem::directory_iterator(account.path(), ec))
            {
                const auto dir = realmDir.path() / std::string(character);

                if (realmDir.is_directory(ec) && std::filesystem::is_directory(dir, ec))
                {
                    found.characters.push_back(dir);
                }
            }
        }

        if (!found.characters.empty())
        {
            found.account = account.path();
            break;
        }
    }

    return found;
}

} // namespace

std::vector<std::filesystem::path> bindingFilesFor(const std::filesystem::path& installDirectory, std::string_view realm, std::string_view character)
{
    std::vector<std::filesystem::path> out;
    const CharacterFolders found = findCharacter(installDirectory, realm, character);

    if (found.characters.empty())
    {
        return out;
    }

    std::vector<std::filesystem::path> files{found.account / "bindings-cache.wtf"};

    // The same name on two realms: only the account's bindings are certain.
    if (found.characters.size() == 1)
    {
        files.push_back(found.characters.front() / "bindings-cache.wtf");
    }

    std::error_code ec;

    for (const auto& file : files)
    {
        if (std::filesystem::is_regular_file(file, ec))
        {
            out.push_back(file);
        }
    }

    return out;
}

namespace bindingsaddon
{

namespace
{

constexpr std::string_view kModifiers[] = {"CTRL-SHIFT-", "CTRL-"};
constexpr std::string_view kKeys[] = {
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "INSERT", "DELETE", "HOME", "END", "NUMPAD0", "NUMPAD1", "NUMPAD2", "NUMPAD3", "NUMPAD4", "NUMPAD5", "NUMPAD6", "NUMPAD7", "NUMPAD8", "NUMPAD9", "NUMPADPLUS", "NUMPADMINUS", "NUMPADMULTIPLY", "NUMPADDIVIDE", "NUMPADDECIMAL",
};

} // namespace

std::span<const std::string_view> modifiers()
{
    return kModifiers;
}

std::span<const std::string_view> keys()
{
    return kKeys;
}

namespace
{
std::optional<KeyChord> keyAt(std::size_t index)
{
    const std::size_t modifier = index / std::size(kKeys);

    if (modifier >= std::size(kModifiers))
    {
        return std::nullopt;
    }

    return KeyChord::parseWow(std::string(kModifiers[modifier]) + std::string(kKeys[index % std::size(kKeys)]));
}

} // namespace

std::optional<KeyChord> keyForSlot(int slot)
{
    if (slot < 1 || slot > Slots)
    {
        return std::nullopt;
    }

    return keyAt(static_cast<std::size_t>(slot - 1));
}

std::optional<KeyChord> keyForShapeshift(std::size_t index)
{
    if (index >= shapeshiftSpells().size())
    {
        return std::nullopt;
    }

    return keyAt(static_cast<std::size_t>(Slots) + index);
}

KeyChord claimKey()
{
    return *KeyChord::parseWow("CTRL-SHIFT-PAGEUP");
}

KeyChord releaseKey()
{
    return *KeyChord::parseWow("CTRL-SHIFT-PAGEDOWN");
}

KeyChord interactKey()
{
    return KeyChord{Key::F11};
} // Player.Interact in actions.yaml
KeyChord releaseSpiritKey()
{
    return *KeyChord::parseWow("CTRL-SHIFT-BACKSPACE");
}

KeyChord retrieveCorpseKey()
{
    return *KeyChord::parseWow("CTRL-SHIFT-ENTER");
}

bool usable(const std::filesystem::path& installDirectory, std::string_view realm, std::string_view character)
{
    const std::string name(Name);
    std::error_code ec;

    if (!std::filesystem::is_regular_file(installDirectory / "Interface" / "AddOns" / name / (name + ".toc"), ec))
    {
        return false;
    }

    // AddOns.txt holds what the character turned on and off; one it does not list yet loads.
    for (const auto& folder : findCharacter(installDirectory, realm, character).characters)
    {
        std::ifstream in(folder / "AddOns.txt", std::ios::binary);
        std::string line;

        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (line == name + ": disabled")
            {
                return false;
            }
        }
    }

    return true;
}

} // namespace bindingsaddon

} // namespace og::wowclient
