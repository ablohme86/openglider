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
 *  File: ModernClientReader.h
 *
 *  Desc:
 *  Declarations and interface for ModernClientReader (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/wowclient/QuestCounts.h"

#include "og/common/Clock.h"
#include "og/state/IGameStateProvider.h"
#include "og/state/GameSnapshot.h"
#include "og/wowclient/ModernLayout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace og::wowclient
{

class IProcessMemory;
class SpellNames;

// A GUID as the 64-bit clients store it: the low half first.
struct Guid128
{
    std::uint64_t low = 0;
    std::uint64_t high = 0;

    bool operator==(const Guid128&) const = default;
};

// The engine's GUIDs are 64 bits. Folding keeps them stable between snapshots
// and unique in practice; 0 stays 0.
Guid foldGuid(const Guid128& guid);

// Whether a text the UI shows has `wanted` in it, as a quest title is looked
// for in an NPC's list. Questie spells some titles otherwise than the client
// ("Your Place In The World" for "Your Place in the World"), so case does not
// count, nor does a typographic apostrophe against a straight one.
bool uiTextContains(std::string_view shown, std::string_view wanted);
// The copper a tooltip's "Sell Price: 1|A:coin-silver:...|a 20|A:coin-copper:...|a"
// line names, as 1.60.1 shows it; -1 without one.
int sellPriceInLines(const std::vector<std::string>& lines);

// One filled action bar slot as the client stores it.
struct ModernActionSlot
{
    int slot = 0; // 1-120
    std::uint32_t id = 0;
    bool item = false;
    bool macro = false;
};

// Builds GameSnapshots from a 64-bit client laid out by a ModernLayout.
// It only reads: nothing here writes to the client's memory.
class ModernClientReader
{
public:
    ModernClientReader(const IProcessMemory& memory, const ModernLayout& layout, std::uint64_t imageBase, const SpellNames& spellNames);

    std::uint64_t objectManager() const;
    // In the world once the active player's object exists.
    bool inWorld() const;
    // nullopt outside the world (login, character select, loading screen).
    std::optional<GameSnapshot> capture(TimePoint now, float visibilityRange);

    std::vector<ModernActionSlot> actionBars() const;
    // Where the object with this (folded) GUID lives, and its full GUID.
    std::optional<std::pair<std::uint64_t, Guid128>> findObject(Guid guid) const;
    std::string realmName() const;
    // A UI frame by name, where the layout knows the client's UI (uiTop).
    std::optional<UiFrame> uiFrame(std::string_view name) const;
    std::optional<std::string> uiText(std::string_view frame, std::string_view fontString) const;
    // The minimap's zone text: the subzone the character stands in
    // ("Northshire Valley"), or the zone where it is in none. Its font string
    // sits in a frame without a global name, so it is looked for once and
    // read where it was found from then on.
    std::optional<std::string> minimapZoneText() const;
    std::optional<UiFrame> uiRegion(std::string_view frame, std::string_view region) const;
    // A Cooldown frame's start (gameTime's clock) and duration, in ms; both 0
    // while it shows none. nullopt where the frame is not found.
    struct UiCooldown
    {
        std::uint32_t start = 0;
        std::uint32_t duration = 0;
    };
    std::optional<UiCooldown> uiCooldown(std::string_view frame) const;
    // The client's clock (ms) the cooldowns start on; 0 when not known.
    std::uint32_t gameTime() const;
    std::optional<UiFrame> uiFrameWithText(std::string_view under, std::string_view text) const;
    // All of them; with an empty `text`, every frame showing any text.
    std::vector<UiFrame> uiFramesWithText(std::string_view under, std::string_view text) const;
    bool supportsUiBagSlots() const;
    std::vector<UiBagSlot> uiBagSlots() const;
    std::optional<UiItemTooltip> uiItemTooltip() const;
    // The comparison tooltips beside it (ShoppingTooltip1, 2): what the
    // character wears in the slot, shown with the client's alwaysCompareItems.
    std::vector<UiItemTooltip> uiComparisonTooltips() const;
    // A tooltip frame's lines (GameTooltip, ShoppingTooltip1); `owned`: only
    // while it is shown for something (a bag slot).
    std::optional<UiItemTooltip> readTooltip(std::string_view frame, bool owned) const;
    // The (folded) GUID under the mouse, 0 over nothing; nullopt where not known.
    std::optional<Guid> mouseoverGuid() const;
    // Every item object in memory, for probing: where it is, its id and stack.
    struct ItemObject
    {
        std::uint64_t base = 0;
        std::uint32_t entry = 0;
        std::uint32_t stack = 0;
    };
    std::vector<ItemObject> itemObjects() const;
    // Every font string with text in a shown frame, for probing: the frame's
    // name (or its nearest named parent's, with a ~), the region's name, the text.
    struct UiTextEntry
    {
        std::string frame;
        std::string region;
        std::string text;
    };
    std::vector<UiTextEntry> uiVisibleTexts() const;
    // The lines players said that the chat window shows now, as the client
    // keeps them - links and colours and all - in no order: the shown font
    // strings with a player's link. Empty where the UI cannot be read.
    std::vector<std::string> chatFrameLines() const;
    // The text of every shown font string (a frame shown; the string itself
    // may be one the frame keeps hidden), with where it is, in no order.
    std::vector<PlacedText> shownTexts() const;

private:
    // The map last known, while the same world stays loaded.
    std::optional<std::uint32_t> lastMap_;
    std::uint64_t lastMapManager_ = 0;
    // Where the character last stood in the world: on a lift's car the client
    // gives its place on the car, and the car nearest this is the one it is on.
    std::optional<Vec3> lastInWorld_;
    // The character's heading then, in the world, and how far the car is
    // turned: its heading on the car is the car's, and the turn is what the
    // heading changed by as it stepped on.
    float lastHeadingInWorld_ = 0.0f;
    std::optional<float> carTurn_;

    struct Object
    {
        std::uint64_t base = 0;
        std::uint8_t type = 0;
        Guid128 guid128;
        Guid guid = 0;
    };

    std::vector<Object> objects() const;
    bool readUnit(const Object& obj, std::uint32_t clock, UnitState& u) const;
    void readPlayer(const Object& obj, PlayerState& p) const;
    void readSkills(const Object& obj, PlayerState& p) const;
    void readQuestLog(const Object& obj, PlayerState& p) const;
    // Backpack and bag contents, and the free slots in them; equipment does not count.
    void readBags(const std::vector<Object>& all, const Object& self, PlayerState& p) const;
    void readAuras(std::uint64_t base, std::uint32_t clock, std::vector<Aura>& auras) const;
    bool readGameObject(const Object& obj, GameObjectState& g) const;
    // Names are cached per GUID once the client has them.
    std::string cachedName(Guid guid, std::uint64_t base, std::uint32_t cacheOffset, std::uint32_t nameOffset) const;
    Guid128 readGuid128(std::uint64_t address) const;

    const IProcessMemory& memory_;
    const ModernLayout& L_;
    std::uint64_t imageBase_;
    const SpellNames& spellNames_;
    // Unit data is read in two blocks: the object header and the inline fields.
    std::uint32_t headerSize_ = 0;
    std::uint32_t fieldsBegin_ = 0;
    std::uint32_t fieldsSize_ = 0;
    bool dynamicFlagsInHeader_ = false;
    mutable std::unordered_map<Guid, std::string> names_;
    // Frames by name, and UIParent, found once and checked by their name on
    // each use: a frame lives as long as the UI (until /reload or logout).
    std::uint64_t findUiObject(std::string_view name) const;
    // Where a UI object sits, as fractions of the window, and whether it shows.
    std::optional<UiFrame> frameOf(std::uint64_t object) const;
    bool uiObjectNamed(std::uint64_t object, std::string_view name) const;
    void loadActionButtonTable() const;
    mutable bool actionButtonTableLoaded_ = false;
    mutable std::unordered_map<std::string, std::uint64_t> uiObjects_;
    mutable std::uint64_t minimapZoneRegion_ = 0;

    // Other players' names by GUID, walked out of the client's name cache at
    // most every two seconds, and only while a player nearby has none.
    void refreshPlayerNames(TimePoint now);
    std::unordered_map<Guid, std::string> playerNames_;
    TimePoint namesRefreshed_{};
};

} // namespace og::wowclient
