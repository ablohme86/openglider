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
 *  File: ModernLayout.h
 *
 *  Desc:
 *  Declarations and interface for ModernLayout (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <cstdint>

// Where a 64-bit client (TBC Anniversary 2.5.6) keeps the data the state
// provider reads.
//
// Unlike ClientLayout, addresses in the image are RVAs from the image base,
// pointers are 8 bytes and GUIDs 128 bits. Objects sit in a slot array rather
// than a linked list, and unit fields are inline in the object instead of
// behind a descriptor pointer. The values were found by probing a live client;
// docs/TBC256-Probing.md says how each one was confirmed.
namespace og::wowclient
{

struct FactionTemplate;

struct ModernLayout
{
    // Pointer to an owner containing an inline camera. Matrix rows are
    // forward, left, up; offsets are from the owner, not another pointer.
    std::uint64_t cameraGlobal = 0;
    std::uint32_t cameraPos = 0, cameraMatrix = 0, cameraFov = 0;
    // --- Image (RVAs) ------------------------------------------------------------
    std::uint64_t objectManager = 0;         // -> object manager
    std::uint64_t objectManagerFallback = 0; // -> mirrored/secondary object manager pointer
    std::uint64_t targetGuid = 0;            // GUID128 of the current target
    std::uint64_t playerNameGuid = 0;        // GUID128 of the character; its name follows at +0x10
    // Other players' names: a hash table whose bucket array pointer sits here,
    // with its capacity 8 bytes before. Each node: next node, GUID128, name.
    std::uint64_t playerNameCacheBuckets = 0;
    std::uint32_t nameNodeNext = 0; // node + this -> next node in the bucket, 0 at the end
    std::uint32_t nameNodeGuid = 0; // GUID128
    std::uint32_t nameNodeName = 0; // char[], inline
    // 1.60.1 names every character "First Last": the last name follows the
    // first this far on, in the cache node and after playerNameGuid alike.
    // 0: first names only.
    std::uint32_t lastNameAfterFirst = 0;
    std::uint64_t gameTime = 0;  // u32 ms, the clock aura expirations count in
    std::uint64_t mapId = 0;     // u32
    std::uint64_t zoneId = 0;    // u32
    std::uint64_t realmName = 0; // char[], the realm the character is on
    // u32 the auto-repeat spell (wand Shoot, Auto Shot) while it fires, 0 when off.
    std::uint64_t autoRepeatSpell = 0;
    std::uint64_t actionBars = 0; // 120 slots of {u32 id, u32 flags}, slot 1 first; see modern::ActionFlag*
    // GUID128 of the unit or object under the mouse, zero over nothing: old
    // Glider's "UnderCursor". The mouse is moved until a corpse or a herb is
    // under it, then right-clicked. 0: not known.
    std::uint64_t mouseoverGuid = 0;
    // float x, y, z of the character's corpse while it is dead or a ghost,
    // zero once alive: where the corpse run goes. 0: not known.
    std::uint64_t corpsePosition = 0;

    // --- Object manager ----------------------------------------------------------
    std::uint32_t mgrSlots = 0;     // -> slot array
    std::uint32_t mgrSlotCount = 0; // u32 capacity of the slot array
    std::uint32_t slotSize = 0;
    std::uint32_t slotEntry = 0;       // slot + this -> entry
    std::uint32_t entryObject = 0;     // entry + this -> object
    std::uint32_t entryItemObject = 0; // entry + this -> object, for items and containers
    std::uint32_t maxSlots = 0;        // upper bound on the capacity, against a bad read

    // --- Object header -----------------------------------------------------------
    std::uint32_t objEntry = 0;  // -> entry; a live object points back at its entry
    std::uint32_t objGuid = 0;   // GUID128
    std::uint32_t objTypeId = 0; // u8, see modern::Type*

    // --- Unit (inline in the object) ----------------------------------------------
    std::uint32_t unitPosition = 0; // float x, y, z
    std::uint32_t unitFacing = 0;
    // 1.60.1 keeps position and facing in a movement block the object points
    // to: object + this -> block, and unitPosition/unitFacing count from the
    // block. 0: they are inline in the object.
    std::uint32_t unitMovement = 0;
    std::uint32_t unitCastingSpell = 0;  // u32, 0 when not casting
    std::uint32_t unitNameCache = 0;     // -> creature cache entry
    std::uint32_t nameCacheName = 0;     // entry + this -> char*
    std::uint32_t creatureCacheType = 0; // entry + this -> u8 creature type (1 Beast, 7 Humanoid, 8 Critter)
    bool hasCreatureCacheType = false;

    // Auras: an array of auraSize records, inline while it is small.
    std::uint32_t unitAuraData = 0;  // -> first record
    std::uint32_t unitAuraCount = 0; // u32 high-water mark; empty records have spell id 0
    std::uint32_t auraSize = 0;
    std::uint32_t auraCaster = 0;     // GUID128
    std::uint32_t auraSpellId = 0;    // u32
    std::uint32_t auraFlags = 0;      // u16, see modern::AuraFlag*
    std::uint32_t auraDuration = 0;   // u32 ms, 0 for an aura without one
    std::uint32_t auraExpiration = 0; // u32 ms on the gameTime clock
    std::uint32_t auraStacks = 0;     // u8

    std::uint32_t unitDynamicFlags = 0; // u32, see modern::UnitDynFlag*
    // The lootable flag shows every corpse with loot for us and clears when it
    // is looted, so the bot can go by it (see BotConfig::lootWithoutFlag).
    bool lootFlagTrusted = false;
    std::uint32_t unitPower = 0;     // u32 the class's power: mana, energy
    std::uint32_t unitHealth = 0;    // int64
    std::uint32_t unitMaxHealth = 0; // int64
    std::uint32_t unitTarget = 0;    // GUID128
    // GUID128 of whoever summoned or created the unit: the player's own for
    // its pet (a warlock's demon, a hunter's beast). 0: not known.
    std::uint32_t unitSummonedBy = 0;
    std::uint32_t unitCreatedBy = 0;
    std::uint32_t unitTapper = 0;   // GUID128 of our player while we hold the tag; empty for anyone else's
    std::uint32_t unitNpcFlags = 0; // u32, see modern::NpcFlag*
    // u32 in the object header: the quest marker over the NPC's head, see
    // modern::QuestGiver*. 0: not known.
    std::uint32_t unitQuestGiverStatus = 0;
    // u32 in the object header beside it: what marker the NPC shows. Not 0
    // on a flight master whose flight path the character has not learnt (the
    // green "!": 3 at Karos Razok in The Sepulcher, 0 once learnt), and on a
    // quest giver with its marker too. 0: not known.
    std::uint32_t unitInteractMarker = 0;
    std::uint32_t unitBytes0 = 0;          // race, class
    std::uint32_t unitBytes1 = 0;          // its low byte the stand state: 1-6 sat (0: not known)
    std::uint32_t unitLevel = 0;           // u32
    std::uint32_t unitFactionTemplate = 0; // u32
    std::uint32_t unitFlags = 0;           // u32, same bits as the older clients
    std::uint32_t unitMaxPower = 0;        // u32 max of the class's power

    // --- Active player (inline in the object) --------------------------------------
    std::uint32_t playerInventory = 0;        // GUID128 per inventory slot, slot 0 first
    std::uint32_t playerCoinage = 0;          // u64 copper carried
    std::uint32_t playerAutoAttackTarget = 0; // GUID128 of the unit auto attack swings at; zero when not attacking
    std::uint32_t playerComboPoints = 0;      // u32, the power slot after the class's own
    std::uint32_t playerMaxComboPoints = 0;   // u32, 5 where that slot holds combo points
    std::uint32_t playerXp = 0;               // u32 experience into the level
    std::uint32_t playerNextLevelXp = 0;      // u32 experience the level needs
    // Skill lines: u16 ids, then their ranks playerSkillStride bytes further on
    // and their maximums twice that. A line with maximum 0 is not learned.
    std::uint32_t playerSkillIds = 0;
    std::uint32_t playerSkillStride = 0;
    // The quest log: a u32 quest id at the start of each entry, 0 in an empty one.
    std::uint32_t playerQuestLog = 0;
    std::uint32_t playerQuestLogStride = 0;
    // Within an entry: a u16 counter per objective, the first objective first.
    std::uint32_t playerQuestLogProgress = 0;
    int inventoryBagFirst = 0; // the equipped bags
    int inventoryBagCount = 0;
    int inventoryBackpackFirst = 0;
    int inventoryBackpackExtraCount = 0; // slots after the backpack's own that some characters have (an authenticator's)
    // u8 on the player: the backpack's slots, the extra ones included (16, or
    // 20 with an authenticator). 0 when not known: the extra slots then count
    // only once the backpack is seen using them.
    std::uint32_t playerBackpackSlots = 0;
    int inventoryEquipmentCount = 0; // equipped slots, from slot 0
    int inventoryBackpackCount = 0;

    // --- Item and container --------------------------------------------------------
    std::uint32_t itemEntry = 0;         // u32 item id
    std::uint32_t itemStackCount = 0;    // u32
    std::uint32_t itemContainedIn = 0;   // GUID128 of its bag; the player's own for the backpack and equipment
    std::uint32_t itemDurability = 0;    // u32
    std::uint32_t itemMaxDurability = 0; // u32, 0 for an item without durability
    std::uint32_t containerSlots = 0;    // u32 number of slots in a bag
    // Enchantments on an item: records of {u32 id, u32 expires (Unix seconds), u32
    // charges}, the permanent enchant first, then the temporary one (a lure,
    // a poison). 0 when not known.
    std::uint32_t itemEnchantments = 0;
    std::uint32_t itemEnchantmentSize = 0;

    // --- Game object -------------------------------------------------------------
    std::uint32_t gameObjectPosition = 0;  // float x, y, z
    std::uint32_t gameObjectNameCache = 0; // -> game object cache entry
    std::uint32_t gameObjectCacheName = 0; // entry + this -> char*
    std::uint32_t gameObjectCacheType = 0; // entry + this -> u32 type (0 Door, 3 Chest, 5 Generic...)
    bool hasGameObjectCacheType = false;
    std::uint32_t gameObjectCacheCaption = 0; // entry + this -> char* ("Collecting")
    // Fishing: who cast a bobber, and the byte that turns 1 while a fish bites.
    // 0 until probed (with a fishing probe); the bot cannot fish without the bite.
    std::uint32_t gameObjectCreatedBy = 0; // GUID128
    std::uint32_t gameObjectBobbing = 0;   // u8 state
    // u8, the low byte of GAMEOBJECT_BYTES_1: 1 ready, 0 used. A gathered herb
    // or vein, or an emptied chest, stays in the client for half a minute or so.
    std::uint32_t gameObjectState = 0;
    std::uint32_t gameObjectBobbingValue = 1;   // what that state reads during a bite
    std::uint32_t gameObjectBobbingMask = 0xFF; // the bits of the state that value is compared on
    std::uint32_t unitZoneId = 0;               // u32 zone id inline in unit/player object

    // --- UI frames -----------------------------------------------------------------
    // Every UI object sits in one ring, the modern form of the UIParent->next
    // list old Glider walked: a frame found by name there gives its rectangle on
    // screen and whether it shows, so a button can be clicked with the mouse -
    // Release Spirit, Repair All - without Lua or an addon. 0: not known.
    std::uint64_t uiTop = 0;        // -> the UI's top object (CSimpleTop)
    std::uint32_t uiTopObjects = 0; // top + this: the ring's head, {next, prev}
    std::uint32_t uiObjectLink = 0; // object + this: its node in the ring, {next, prev}
    std::uint32_t uiObjectName = 0; // object + this -> char*, null for an unnamed one
    // object + this: 4 floats bottom, left, top, right on screen once laid out.
    // UIParent's covers the whole window, so the others divide by it.
    std::uint32_t uiObjectRect = 0;
    std::uint32_t uiObjectFlags = 0; // u32
    std::uint32_t uiVisibleMask = 0; // in uiObjectFlags: shown, and every parent shown
    // A frame's regions - its textures and FontStrings - hang off the frame,
    // not in the ring: frame + uiFrameRegions is their list's head, region +
    // uiRegionLink its node. A FontString's text: region + uiFontStringText -> char*.
    std::uint32_t uiFrameRegions = 0;
    std::uint32_t uiRegionLink = 0;
    std::uint32_t uiFontStringText = 0;
    std::uint32_t uiFontStringColor = 0; // packed ARGB
    std::uint32_t uiObjectParent = 0;
    std::uint32_t uiTooltipOwner = 0;
    std::uint64_t uiBagButtonVtable = 0; // RVA of ordinary bag item buttons
    // An action button's Cooldown frame ("ActionButton2Cooldown"): when the
    // cooldown it shows began, on gameTime's clock, and how long it runs, both
    // in ms (u32); both 0 while it shows none. The global cooldown shows too.
    std::uint32_t uiCooldownStart = 0;
    std::uint32_t uiCooldownDuration = 0;
    // An action button table in .data holding entries of {Frame* button, Frame* cooldown, u64 slot}.
    // 0 on clients that discover UI frames through the CSimpleTop ring (1.60.1).
    std::uint64_t uiActionButtonTable = 0;

    // The build's FactionTemplate table, for unit reactions. nullptr: every
    // reaction is unknown.
    const FactionTemplate* (*factionTemplate)(std::uint32_t id) = nullptr;
};

namespace modern
{
inline constexpr std::uint8_t TypeItem = 1;
inline constexpr std::uint8_t TypeContainer = 2;
inline constexpr std::uint8_t TypeUnit = 5;
inline constexpr std::uint8_t TypePlayer = 6;
inline constexpr std::uint8_t TypeActivePlayer = 7;
inline constexpr std::uint8_t TypeGameObject = 8;
inline constexpr std::uint8_t TypeDynamicObject = 9;
inline constexpr std::uint8_t TypeCorpse = 10;

// The corpse's loot is ours to take. Set only once we are out of combat, and
// cleared when it is looted.
inline constexpr std::uint32_t UnitDynFlagLootable = 0x04;
// Tagged by another player, living or dead: its loot and skin are not ours.
inline constexpr std::uint32_t UnitDynFlagTappedByOther = 0x10;

inline constexpr std::uint32_t NpcFlagVendor = 0x80;
inline constexpr std::uint32_t NpcFlagRepair = 0x1000;
inline constexpr std::uint32_t NpcFlagFlightMaster = 0x2000;

// The quest marker's bits, seen on 1.60.1 while quests were taken and handed
// in: 0x15 and 0x16 a yellow !, 0x22 a yellow ?, 0x0D a grey ?, 0 none. The
// low bits vary with the quest; the highest bit set decides.
inline constexpr std::uint32_t QuestGiverIncomplete = 0x08;
inline constexpr std::uint32_t QuestGiverAvailable = 0x10;
inline constexpr std::uint32_t QuestGiverReward = 0x20;

inline constexpr std::uint16_t AuraFlagPositive = 0x02;
inline constexpr std::uint16_t AuraFlagDuration = 0x04;
inline constexpr std::uint16_t AuraFlagNegative = 0x10;

inline constexpr std::uint32_t ActionFlagItem = 0x80000000;
inline constexpr std::uint32_t ActionFlagMacro = 0x40000000; // as in 3.3.5a; not seen on 2.5.6 yet
// An action bar slot's kind is the top byte of its flags: 0 a spell, an item,
// a macro - and others besides (WoW Forever 1.60.1.70450: 0x30, id 267, on a
// hunter's bars), which are none of those.
inline constexpr std::uint32_t ActionFlagTypeMask = 0xFF000000;
} // namespace modern

} // namespace og::wowclient
