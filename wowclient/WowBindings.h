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
 *  File: WowBindings.h
 *
 *  Desc:
 *  Declarations and interface for WowBindings (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/input/Key.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace og::wowclient
{

// The key bindings a WoW client keeps in its WTF folder.
//
// For clients whose action bars cannot be scanned through Lua, this is how the
// bot learns which key presses which slot. bindings-cache.wtf only lists what
// differs from the defaults, so the defaults of the action buttons come first.
class WowBindings
{
public:
    WowBindings();

    // Applies "bind <KEY> <COMMAND>" lines; a later binding of a key replaces
    // the earlier one, as in the client.
    void apply(std::string_view bindingsFile);
    // Reads and applies a file; false if it cannot be read.
    bool applyFile(const std::filesystem::path& path);

    std::optional<KeyChord> keyFor(std::string_view command) const;

    // The command of the button showing an action bar slot (1-120), or empty for
    // a slot no bar shows on its own (the pages behind the main bar).
    static std::string commandForSlot(int slot);
    // Slots whose button depends on the page the main bar shows: 1-12 and the
    // stance and form pages, 73-120.
    static bool onSwappingPage(int slot)
    {
        return slot <= 12 || slot > 72;
    }

private:
    std::map<std::string, std::string, std::less<>> commandByKey_; // "SHIFT-1" -> "MULTIACTIONBAR1BUTTON1"
};

// The bindings-cache.wtf files for a character, in the order they apply: the
// account's, then the character's own when it uses character bindings.
// `installDirectory` is the one holding WTF. With no realm, or one that has no
// such character, the character is looked for under every realm of the account.
std::vector<std::filesystem::path> bindingFilesFor(const std::filesystem::path& installDirectory, std::string_view realm, std::string_view character);

// The bindings addon (addons/BarKeys), for clients the bot cannot run Lua in.
// While the bot has claimed them it gives every action bar slot a key of its
// own, so a slot the player gave no key can be pressed too, and Interact With
// Target one that also takes the nearest corpse in front. They are override
// bindings: releasing them, a /reload or logging out puts back the player's
// own. The table must match MODIFIERS, KEYS, CLAIM_KEY, RELEASE_KEY and
// INTERACT_KEY in the addon, which says nothing and names nothing of the bot.
namespace bindingsaddon
{
inline constexpr std::string_view Name = "BarKeys";
inline constexpr int Slots = 120;

std::span<const std::string_view> modifiers();
std::span<const std::string_view> keys();
// Slot i + 1 is modifiers()[i / keys().size()] + keys()[i % keys().size()].
std::optional<KeyChord> keyForSlot(int slot);
// The key of the button that casts shapeshiftSpells()[index]: the keys after the slots'.
std::optional<KeyChord> keyForShapeshift(std::size_t index);
KeyChord claimKey();
KeyChord releaseKey();
// Interact With Target (INTERACTTARGET): F11, the key actions.yaml gives
// Player.Interact. From login on, claimed or not, while the player's own
// bindings leave F11 free; the addon also turns SoftTargetInteract to 3 then.
KeyChord interactKey();
// Release Spirit (RepopMe) and Retrieve Corpse, which need Lua on the older clients.
KeyChord releaseSpiritKey();
KeyChord retrieveCorpseKey();
// Installed in the client, and not turned off for the character.
bool usable(const std::filesystem::path& installDirectory, std::string_view realm, std::string_view character);
} // namespace bindingsaddon

} // namespace og::wowclient
