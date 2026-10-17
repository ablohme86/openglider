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
 *  File: ClientLayout.h
 *
 *  Desc:
 *  Declarations and interface for ClientLayout (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

// Where a WoW client build keeps the data the state provider reads.
//
// Each supported build has one ClientLayout (Layout335a.cpp, Layout1121.cpp).
// The provider code is shared; only these numbers differ. An address or offset
// of 0 means "not known for this client": the provider then either uses a
// fallback (documented per field) or leaves that part of the snapshot empty.
//
// Descriptor offsets are byte offsets from an object's descriptor pointer
// (UpdateFields index * 4).
//
// The 64-bit clients (2.5.6 and later) are laid out too differently for these
// fields: they leave them 0 and describe themselves with a ModernLayout.
namespace og::wowclient
{

struct ModernLayout;

// How the aura list of a unit is stored.
enum class AuraStorage
{
    ObjectTable, // 3.x: count + table of aura records in the unit object (with caster GUID)
    Descriptors, // 1.x: UNIT_FIELD_AURA spell id array + UNIT_FIELD_AURAAPPLICATIONS in the descriptors
};

struct ClientLayout
{
    std::string_view name;   // "3.3.5a", or the product: "WoW Forever"
    std::uint32_t build = 0; // 12340
    // The version WoW.exe reports for this build ("1.60.1"), where the name is
    // a product rather than a version; empty for the free clients.
    std::string_view clientVersion;
    int powerSlots = 7; // UNIT_FIELD_POWER1..n

    // A 64-bit client: snapshots come from this layout instead of the fields below.
    const ModernLayout* modern = nullptr;
    // Never write to this client's memory. That rules out Lua, click-to-move,
    // target selection and the client hooks, whatever the fields below say.
    bool readOnly = false;

    // --- Static addresses ------------------------------------------------------
    // Object manager: read a pointer at objectManager; when curMgrOffset is set,
    // that is a connection object and the manager is at connection + curMgrOffset.
    std::uint32_t objectManager = 0;
    std::uint32_t curMgrOffset = 0;
    std::uint32_t isInGame = 0;             // byte, 1 in world. 0: in world when the local player object exists
    std::uint32_t playerName = 0;           // char[]
    std::uint32_t targetGuid = 0;           // uint64. 0: use the player's UNIT_FIELD_TARGET
    std::uint32_t mouseoverGuid = 0;        // uint64 (not used yet)
    std::uint32_t comboPoints = 0;          // byte
    std::uint32_t corpsePosition = 0;       // float x,y,z. 0: use the player's corpse object (descCorpse*)
    std::uint32_t zoneId = 0;               // uint32
    std::uint32_t actionBarFirstSlot = 0;   // 120 x uint32 spell ids
    std::uint32_t spellBook = 0;            // uint32 spell ids, 0-terminated
    std::uint32_t factionTemplateStore = 0; // s_factionTemplateStore DBC store pointer
    std::uint32_t autoRepeatSpell = 0;      // uint32 spell id being auto-repeated (wand Shoot, Auto Shot)

    // Chat: a ring of chatMessageCount entries, chatMessageStride bytes apart.
    // chatPosition changes whenever a line is added; each entry holds the
    // client's formatted line ("Type: [..], Channel: [..], Player Name: [..],
    // ..., Text: [..]") at chatTextOffset. 0: chat is not read.
    std::uint32_t chatBuffer = 0;
    std::uint32_t chatPosition = 0;
    std::uint32_t chatMessageStride = 0;
    std::uint32_t chatTextOffset = 0;
    int chatMessageCount = 0;
    std::uint32_t cvarList = 0; // -> head pointer to CVar linked list

    // Camera, for projecting world positions to the screen (not used yet).
    std::uint32_t cameraManager = 0; // -> camera manager
    std::uint32_t cameraOffset = 0;  // camera manager + this -> active camera
    std::uint32_t cameraPos = 0, cameraMatrix = 0, cameraFov = 0, cameraAspect = 0;

    // --- Lua execution -----------------------------------------------------------
    // Running a Lua string needs the client to *call* something for us. The
    // executor swaps one entry of the D3D device's vtable for a stub written
    // into a code cave; the render thread then runs the stub every frame.
    // All four must be non-zero for Lua to be available on a build.
    //
    //   luaDoString: FrameScript::Execute(const char* script, const char* name, uintptr_t state), cdecl
    //   d3dDevicePointer / d3dDeviceOffset: -> IDirect3DDevice9, whose first field is the vtable
    //   d3dEndSceneIndex: vtable slot to swap (EndScene is 42 in the D3D9 vtable)
    //   codeCave / codeCaveSize: unused, writable client memory for the stub and buffers
    std::uint32_t luaDoString = 0;
    bool luaFastcall = false;   // 1.x: FrameScript::Execute takes the script in ecx and the name in edx
    bool vanillaLuaApi = false; // Lua 5.0 and the 1.x API: GetItemInfo takes an item id, no item level
    std::uint32_t d3dDevicePointer = 0;
    std::uint32_t d3dDeviceOffset = 0;
    int d3dEndSceneIndex = 42;
    std::uint32_t codeCave = 0;
    std::uint32_t codeCaveSize = 0;
    // Where the stub keeps its request flag, the queued call and the script.
    // 0: the start of the code cave, which then has to be writable by the
    // client as well as executable. A build whose padding is read-only where
    // it is executable (1.12.1) puts these in a writable section's padding.
    std::uint32_t dataCave = 0;
    std::uint32_t dataCaveSize = 0;

    // Client functions the same stub calls, as thiscall with one argument.
    // Only the D3D device and the code cave are needed for these, not Lua.
    //   onRightClickUnit: CGUnit_C::OnRightClick(int autoLoot), what a right-click
    //   on a unit does - on a corpse, it loots it
    std::uint32_t onRightClickUnit = 0;

    // Click-to-move (unit looting and game object interaction write to the game).
    std::uint32_t ctmBase = 0;
    std::uint32_t ctmDistance = 0, ctmAction = 0, ctmGuid = 0, ctmX = 0, ctmY = 0, ctmZ = 0;
    std::uint32_t ctmActivatePointer = 0, ctmActivateOffset = 0;

    // --- Object manager and object structs ---------------------------------------
    std::uint32_t mgrFirstObject = 0;
    std::uint32_t mgrLocalGuid = 0;
    std::uint32_t objNext = 0;
    std::uint32_t objDescriptors = 0;
    std::uint32_t objType = 0;
    std::uint32_t objGuid = 0;

    std::uint32_t unitPosX = 0; // float x,y,z
    std::uint32_t unitRotation = 0;
    std::uint32_t unitAutoAttackTarget = 0; // uint64
    std::uint32_t unitCastingSpell = 0;     // uint32 spell id
    std::uint32_t unitChannelSpell = 0;     // uint32 spell id. 0: use descUnitChannelSpell
    std::uint32_t unitNamePtr1 = 0;         // unit -> creature cache entry
    std::uint32_t unitNamePtr2 = 0;         // entry + this -> char*

    AuraStorage auraStorage = AuraStorage::ObjectTable;
    // ObjectTable
    std::uint32_t auraCount1 = 0, auraTable1 = 0, auraCount2 = 0, auraTable2 = 0, auraSize = 0;
    std::uint32_t auraCreatorGuid = 0, auraSpellId = 0, auraFlags = 0, auraStacks = 0;
    // Descriptors
    std::uint32_t descUnitAura = 0;             // auraSlots x uint32 spell id
    std::uint32_t descUnitAuraApplications = 0; // auraSlots x uint8 (stacks - 1)
    int auraSlots = 0;
    int auraPositiveSlots = 0; // slots before this index are buffs

    std::uint32_t gameObjectPosX = 0; // float x,y,z in the object. 0: use descGameObjectPosX
    std::uint32_t gameObjectNamePtr1 = 0;
    std::uint32_t gameObjectNamePtr2 = 0;
    std::uint32_t gameObjectBobbing = 0; // byte, 1 while a fishing bobber bites

    // --- Descriptors -------------------------------------------------------------
    std::uint32_t descEntry = 0;
    std::uint32_t descUnitSummonedBy = 0, descUnitCreatedBy = 0, descUnitTarget = 0;
    std::uint32_t descUnitBytes0 = 0; // race, class, gender, power type
    std::uint32_t descUnitBytes1 = 0; // stand state, pet flags, vis flags, anim tier
    std::uint32_t descUnitHealth = 0, descUnitPower1 = 0, descUnitMaxHealth = 0, descUnitMaxPower1 = 0;
    std::uint32_t descUnitLevel = 0, descUnitFactionTemplate = 0;
    std::uint32_t descUnitFlags = 0, descUnitDynamicFlags = 0, descUnitNpcFlags = 0;
    std::uint32_t descUnitChannelSpell = 0;
    std::uint32_t unitDescriptorSize = 0; // UNIT_END * 4

    std::uint32_t descPlayerFlags = 0;
    std::uint32_t descPlayerInvSlot1 = 0;  // 19 equipped item GUIDs (inventory slots 0-18)
    std::uint32_t descPlayerBagSlot1 = 0;  // 4 bag GUIDs (inventory slots 19-22)
    std::uint32_t descPlayerPackSlot1 = 0; // 16 backpack item GUIDs
    std::uint32_t descPlayerXp = 0, descPlayerNextLevelXp = 0;
    std::uint32_t descPlayerCoinage = 0; // uint32 copper carried
    std::uint32_t descPlayerSkills = 0;  // PLAYER_SKILL_INFO_1_1 (384 dwords)

    std::uint32_t descItemStackCount = 0;
    std::uint32_t descItemDurability = 0, descItemMaxDurability = 0; // max 0: the item has none
    std::uint32_t descContainerNumSlots = 0, descContainerSlot1 = 0;

    std::uint32_t descGameObjectCreatedBy = 0;
    std::uint32_t descGameObjectType = 0; // field holding the GO type
    int gameObjectTypeShift = 0;          // bit position of the type byte in that field
    std::uint32_t descGameObjectPosX = 0;

    std::uint32_t descCorpseOwner = 0, descCorpsePosX = 0;

    // Dynamic flag set when *we* tapped a unit (0: the client only marks units
    // tapped by others).
    std::uint32_t dynFlagTappedByPlayer = 0;

    // UNIT_NPC_FLAGS bits for selling and repairing. 1.x numbers the flags
    // differently - its 0x80 is an innkeeper - so each build names its own.
    std::uint32_t npcFlagVendor = 0x00000080;
    std::uint32_t npcFlagRepair = 0x00001000;
};

const ClientLayout& layout335a();
const ClientLayout& layout1121();

struct LayoutDocument; // LayoutJson.h

// The clients that are free and always compiled in: 3.3.5a and 1.12.1.
const std::vector<const ClientLayout*>& legacyLayouts();
// Every client that can be attached to right now: the legacy ones, then those
// registered at run time, then any compiled in. A copy, because registration
// can change the list on another thread.
std::vector<const ClientLayout*> knownLayouts();
// A layout the application compiled in (64-bit clients are not part of
// OpenGlider; a development build of an application may carry some). Listed
// after those registered at run time: one from there wins. It must live for
// the rest of the program.
void addCompiledLayout(const ClientLayout& layout);
// Adds a 64-bit layout from outside (the license server) and returns it; it
// replaces a compiled-in or earlier one for the same build. Always read-only.
// nullptr for the build of a legacy client. A registered layout stays in
// memory for good, so pointers to it outlive clearDynamicLayouts().
const ClientLayout* registerLayout(const LayoutDocument& document);
// Takes every registered layout off the list, as when a license lapses.
void clearDynamicLayouts();
const ClientLayout* layoutForBuild(std::uint32_t build);
// A name ("3.3.5a", "WoW Forever"), a client version ("1.60.1"), both
// case-insensitive, or a build number.
const ClientLayout* layoutByName(std::string_view name);

// Values shared by all supported clients.
namespace consts
{
inline constexpr std::uint32_t ImageBase = 0x400000;
inline constexpr std::uint64_t ImageBase64 = 0x140000000; // the 64-bit clients

inline constexpr std::uint32_t UnitFlagNonAttackable = 0x00000002;
inline constexpr std::uint32_t UnitFlagImmuneToPc = 0x00000100; // 1.x: "not attackable out of combat"
inline constexpr std::uint32_t UnitFlagInCombat = 0x00080000;
inline constexpr std::uint32_t UnitFlagNotSelectable = 0x02000000;
inline constexpr std::uint32_t UnitFlagFleeing = 0x00800000; // feared, or running at low health
inline constexpr std::uint32_t UnitFlagOnTaxi = 0x00100000;  // flying a flight path
inline constexpr std::uint32_t UnitFlagSkinnable = 0x04000000;
inline constexpr std::uint32_t DynFlagLootable = 0x0001;
inline constexpr std::uint32_t DynFlagTapped = 0x0004;
inline constexpr std::uint32_t DynFlagDead = 0x0020;
inline constexpr std::uint32_t PlayerFlagGhost = 0x00000010;

inline constexpr std::uint32_t TypeItem = 1;
inline constexpr std::uint32_t TypeContainer = 2;
inline constexpr std::uint32_t TypeUnit = 3;
inline constexpr std::uint32_t TypePlayer = 4;
inline constexpr std::uint32_t TypeGameObject = 5;
inline constexpr std::uint32_t TypeDynamicObject = 6;
inline constexpr std::uint32_t TypeCorpse = 7;

inline constexpr std::uint8_t GoTypeDoor = 0;
inline constexpr std::uint8_t GoTypeChest = 3; // also herbs and mining veins
inline constexpr std::uint8_t GoTypeGeneric = 5;
inline constexpr std::uint8_t GoTypeTransport = 11;
inline constexpr std::uint8_t GoTypeMoTransport = 15;
inline constexpr std::uint8_t GoTypeFishingNode = 17;
inline constexpr std::uint8_t GoTypeFishingHole = 25;
} // namespace consts

} // namespace og::wowclient
