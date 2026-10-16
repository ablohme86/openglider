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
 *  File: IGameStateProvider.h
 *
 *  Desc:
 *  Declarations and interface for IGameStateProvider (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include "og/common/Camera.h"
#include "og/state/GameSnapshot.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

// A named UI frame as the client lays it out: where it is in the game window,
// as fractions of the window from the top left, and whether it shows.
struct UiFrame
{
    bool visible = false;
    float left = 0, top = 0, right = 0, bottom = 0;

    float centerX() const
    {
        return (left + right) / 2;
    }

    float centerY() const
    {
        return (top + bottom) / 2;
    }
};

// A chat line the client received: its channel ("Party", "Whisper"), who said
// it, and what.
struct ChatMessage
{
    std::string channel;
    std::string sender;
    std::string text;
};

// The cooldown an action bar slot's button shows.
struct ActionCooldown
{
    Duration remaining{0};
    Duration duration{0};
    // Not the action's own: the global cooldown, or the gap between a wand's
    // shots, which the client shows on every button alike.
    bool shared = false;
};

// An opaque UI identity, valid only while this bag button still exists.
struct UiBagSlot
{
    std::uint64_t token = 0;
    UiFrame frame;
};

struct UiItemTooltip
{
    std::uint64_t owner = 0;
    std::string name;
    int quality = -1;
    std::vector<std::string> lines;
    // Each line's colour, 0xRRGGBB, where the client tells: a red line is
    // something the character cannot use (the armour, the weapon, the level).
    std::vector<std::uint32_t> lineColors;
    // Copper it sells for; -1 when the tooltip shows no price.
    int sellPrice = -1;

    bool hasRedLine() const
    {
        for (const auto c : lineColors)
        {
            if (((c >> 16) & 0xff) >= 0xe0 && ((c >> 8) & 0xff) < 0x50 && (c & 0xff) < 0x50)
            {
                return true;
            }
        }

        return false;
    }
};

// Slots past this stand for the stance bar's buttons, 1001 its first: a
// stance changed with a click on it.
inline constexpr int kStanceBarSlots = 1000;
// Pet buttons use a separate range so they never become UseAction slots.
inline constexpr int kPetBarSlots = 2000;

// The button the default UI shows an action bar slot on, which a click on
// uses the slot: the main bar's first page, the four extra bars and the stance
// bar. Empty for the slots it has no fixed button for (the main bar's other pages).
inline std::string actionButtonFrame(int slot)
{
    if (slot > kPetBarSlots && slot <= kPetBarSlots + 10)
    {
        return "PetActionButton" + std::to_string(slot - kPetBarSlots);
    }

    if (slot > kStanceBarSlots && slot <= kStanceBarSlots + 10)
    {
        return "StanceButton" + std::to_string(slot - kStanceBarSlots);
    }

    struct Bar
    {
        int first;
        const char* name;
    };
    static constexpr Bar bars[] = {{1, "ActionButton"}, {25, "MultiBarRightButton"}, {37, "MultiBarLeftButton"}, {49, "MultiBarBottomRightButton"}, {61, "MultiBarBottomLeftButton"}};

    for (const Bar& bar : bars)
    {
        if (slot >= bar.first && slot < bar.first + 12)
        {
            return bar.name + std::to_string(slot - bar.first + 1);
        }
    }

    return {};
}

// Source of game state. Core never knows which implementation is in use:
// MockStateProvider, ReplayStateProvider, SimStateProvider, WowMemoryStateProvider, ...
class IGameStateProvider
{
public:
    virtual ~IGameStateProvider() = default;

    virtual std::string name() const = 0;

    // Returns the current world state, or nullopt when no state is available
    // (not attached, loading screen, character select, end of replay...).
    virtual std::optional<GameSnapshot> capture() = 0;

    // A UI frame by its global name ("StaticPopup1Button1"), read from the
    // client's memory: what lets a button be clicked without Lua or an addon.
    // nullopt where the client's UI cannot be read, or no frame has the name.
    virtual std::optional<UiFrame> uiFrame(std::string_view name)
    {
        (void)name;
        return std::nullopt;
    }

    // The GUID of the unit or object under the mouse, 0 over nothing;
    // nullopt where the client does not tell.
    virtual std::optional<Guid> mouseoverGuid()
    {
        return std::nullopt;
    }

    // Whether uiFrame and uiText can read this client's UI at all.
    virtual bool supportsUi() const
    {
        return false;
    }

    virtual bool supportsUiBagSlots() const
    {
        return false;
    }

    virtual std::vector<UiBagSlot> uiBagSlots()
    {
        return {};
    }

    virtual std::optional<UiItemTooltip> uiItemTooltip()
    {
        return std::nullopt;
    }

    // What the character wears where the item under the mouse would go: the
    // comparison tooltips beside its tooltip (two for a ring, a trinket, a
    // one-hand weapon with a second). None: the slot is empty, or the client
    // does not compare (its alwaysCompareItems is off).
    virtual std::vector<UiItemTooltip> uiComparisonTooltips()
    {
        return {};
    }

    // The text of a frame's FontString, both by global name
    // ("StaticPopup1", "StaticPopup1Text"): what a dialog says, or a button.
    virtual std::optional<std::string> uiText(std::string_view frame, std::string_view fontString)
    {
        (void)frame;
        (void)fontString;
        return std::nullopt;
    }

    // What the button showing an action bar slot tells of its cooldown, read
    // from the client instead of counted from the bot's own key presses.
    // nullopt where it cannot be read (no button shown for the slot, or a
    // client whose UI is not readable): the bot's own count then stands.
    // The chat lines received since the last call, oldest first (the Party
    // system's messages among them); none where the chat cannot be read.
    virtual std::vector<ChatMessage> takeChat()
    {
        return {};
    }

    virtual std::optional<ActionCooldown> actionCooldown(int slot)
    {
        (void)slot;
        return std::nullopt;
    }

    // Bounds of a named region (such as a section heading) inside a frame.
    virtual std::optional<UiFrame> uiRegion(std::string_view frame, std::string_view region)
    {
        (void)frame;
        (void)region;
        return std::nullopt;
    }

    // A shown frame inside `under` (by global name) that shows `text` - the
    // unnamed buttons of a list, such as the quests in a gossip window.
    virtual std::optional<UiFrame> uiFrameWithText(std::string_view under, std::string_view text)
    {
        (void)under;
        (void)text;
        return std::nullopt;
    }

    // Every shown frame inside `under` that shows `text` - or any text at all,
    // where `text` is empty: a quest's title stands in a greeting window twice
    // when the quest taken and the one offered next share their name.
    virtual std::vector<UiFrame> uiFramesWithText(std::string_view under, std::string_view text)
    {
        if (const auto one = text.empty() ? std::nullopt : uiFrameWithText(under, text))
        {
            return {*one};
        }

        return {};
    }

    // A client whose target cannot be set, and whose soft target does not take
    // corpses (the read-only 64-bit ones): a corpse is targeted by name
    // through the chat before Interact, and only once the client shows it.
    virtual bool targetsByName() const
    {
        return false;
    }

    // How far Tab (Target.Next) reaches: about 20 yards on the old clients;
    // the 64-bit ones (targetNearestDistance, 41 by default) much farther.
    virtual float tabTargetReach() const
    {
        return 20.0f;
    }

    // Directly sets target in the game client (if supported by provider).
    virtual bool setTarget(Guid guid)
    {
        (void)guid;
        return false;
    }

    // Projects a 3D world position to 2D screen coordinates using the game's active camera.
    virtual std::optional<ScreenPoint> worldToScreen(const Vec3& worldPos)
    {
        (void)worldPos;
        return std::nullopt;
    }

    // Returns the active 3D camera if available.
    virtual std::optional<Camera> camera() const
    {
        return std::nullopt;
    }

    // Loots a corpse. This is not the same client command as talking to a
    // living NPC: a vendor ignores a loot request.
    virtual bool interactUnit(Guid guid, const Vec3& position)
    {
        (void)guid;
        (void)position;
        return false;
    }

    // Skin through the client's right-click when available, otherwise use the interact binding.
    virtual bool skinUnit(Guid guid)
    {
        (void)guid;
        return false;
    }

    // Talks to a living NPC (vendor, quest giver, flight master), which is what
    // opens its window.
    virtual bool interactNpc(Guid guid, const Vec3& position)
    {
        (void)guid;
        (void)position;
        return false;
    }

    // Clears a pending interact command. Click-to-move commands persist in the
    // client, so one left standing keeps steering the character back to its
    // target and fights any movement the bot does with the keyboard.
    virtual void cancelInteraction()
    {
    }

    // Running Lua in the client. Needed for anything the bot cannot express as
    // a keypress or a memory write - selling to a vendor, for one.
    virtual bool supportsLua() const
    {
        return false;
    }

    virtual bool executeLua(std::string_view script)
    {
        (void)script;
        return false;
    }

    // False while the client still has a queued script to run.
    virtual bool luaIdle() const
    {
        return true;
    }

    // The client speaks Lua 5.0 and the 1.x API, which scripts have to be written for.
    virtual bool vanillaLua() const
    {
        return false;
    }

    // Game objects need a different client command from corpse looting.
    virtual bool supportsObjectInteraction() const
    {
        return false;
    }

    virtual bool interactObject(Guid guid, const Vec3& position)
    {
        (void)guid;
        (void)position;
        return false;
    }

    virtual void cancelObjectInteraction()
    {
    }
};

} // namespace og
