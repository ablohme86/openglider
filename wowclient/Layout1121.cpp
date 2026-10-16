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
 *  File: Layout1121.cpp
 *
 *  Desc:
 *  Implementation of Layout1121 (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ClientLayout.h"

// World of Warcraft 1.12.1 (build 5875).
//
// NOT YET VERIFIED against a real client - check with a probe.
//
// Descriptor offsets follow the 1.12.1 UpdateFields (OBJECT_END 0x06,
// ITEM_END 0x30, UNIT_END 0xBC); they add up consistently (inventory, pack,
// bank, bank bags, buyback and keyring slots end exactly at PLAYER_XP).
// The static and object-struct addresses are the commonly published 5875
// values. Everything that is not known with reasonable confidence is left at 0
// and handled by a fallback:
//   - in-world check      -> the local player object exists
//   - target GUID         -> the player's UNIT_FIELD_TARGET
//   - corpse position     -> the player's corpse object
//   - channelled spell    -> UNIT_CHANNEL_SPELL
//   - game object position -> GAMEOBJECT_POS_X/Y/Z
// Not available yet: combo points, zone id, auto-attack state, game object
// names, fishing bites, chat, click-to-move and reading Lua results back (no
// CVar list) - so action bars are read from Bar 1 on the game's default keys,
// and corpses and vendors are right-clicked through the client's own function.
namespace og::wowclient
{

namespace
{

ClientLayout make()
{
    ClientLayout l;
    l.name = "1.12.1";
    l.build = 5875;
    l.powerSlots = 5; // mana, rage, focus, energy, happiness

    // Static addresses
    l.objectManager = 0x00B41414; // -> object manager (no connection object in between)
    l.playerName = 0x00C27D88;
    // Both confirmed on a live client: the spellbook ends with a 0, and the
    // action bar stores items as 0x80000000 | item id, as 3.3.5a does.
    l.spellBook = 0x00B700F0;
    l.actionBarFirstSlot = 0x00BC6980;
    l.cameraManager = 0x0074B2BC;
    l.cameraOffset = 0x65B8;
    l.cameraPos = 0x08;
    l.cameraMatrix = 0x14;
    l.cameraFov = 0x40;
    l.cameraAspect = 0x44;

    // Lua and client function calls from the render thread, as on 3.3.5a.
    // Checked on a live client: the D3D chain resolves to EndScene,
    // FrameScript::Execute takes the script in ecx and the name in edx, and
    // OnRightClick takes the unit in ecx and returns with ret 4. The scripts
    // written for 3.x functions and Lua 5.1 report back through CVars, which
    // this layout has no address for, so they never run here.
    l.luaDoString = 0x00704CD0;
    l.luaFastcall = true;
    l.vanillaLuaApi = true;
    l.d3dDevicePointer = 0x00C0ED38;
    l.d3dDeviceOffset = 0x38A8;
    l.d3dEndSceneIndex = 42;
    // Padding past the end of a section, which the client never uses: the stub
    // after .text (executable, read-only), its flag and buffers after .data
    // (writable). The client writes the flag itself - a flag in .rdata's
    // padding crashed it.
    l.codeCave = 0x007FEE00; // .text ends at 0x7FEDAC
    l.codeCaveSize = 0x100;
    l.dataCave = 0x00CFA200; // .data ends at 0xCFA154
    l.dataCaveSize = 0xC00;
    l.onRightClickUnit = 0x0060BEA0; // CGUnit_C::OnRightClick(int autoLoot)

    // Object manager and objects
    l.mgrFirstObject = 0xAC;
    l.mgrLocalGuid = 0xC0;
    l.objNext = 0x3C;
    l.objDescriptors = 0x08;
    l.objType = 0x14;
    l.objGuid = 0x30;

    l.unitPosX = 0x9B8;
    l.unitRotation = 0x9C4;
    // Found on a live client: the spell id sits here for as long as a
    // non-channelled cast lasts (Frostbolt, Conjure Water) and is 0 otherwise.
    // Without it the bot only had its own estimate, walked off a fraction
    // early and broke every 3 s Conjure. (0xC84 next to it is garbage.)
    l.unitCastingSpell = 0xC8C;
    l.unitNamePtr1 = 0xB30;
    l.unitNamePtr2 = 0x00;

    // Auras live in the descriptors: 48 spell ids, the first 32 are buffs.
    l.auraStorage = AuraStorage::Descriptors;
    l.descUnitAura = 0x2F * 4;
    l.descUnitAuraApplications = 0x71 * 4;
    l.auraSlots = 48;
    l.auraPositiveSlots = 32;

    // Descriptors: OBJECT_END = 0x06, UNIT_END = 0xBC
    l.descEntry = 0x03 * 4;
    l.descUnitSummonedBy = 0x0C * 4;
    l.descUnitCreatedBy = 0x0E * 4;
    l.descUnitTarget = 0x10 * 4;
    l.descUnitHealth = 0x16 * 4;
    l.descUnitPower1 = 0x17 * 4;
    l.descUnitMaxHealth = 0x1C * 4;
    l.descUnitMaxPower1 = 0x1D * 4;
    l.descUnitLevel = 0x22 * 4;
    l.descUnitFactionTemplate = 0x23 * 4;
    l.descUnitBytes0 = 0x24 * 4;
    l.descUnitBytes1 = 0x8A * 4; // stand state in the low byte: 0 standing, 1 sitting (seen on a live client)
    l.descUnitFlags = 0x2E * 4;
    l.descUnitDynamicFlags = 0x8F * 4;
    l.descUnitChannelSpell = 0x90 * 4;
    l.descUnitNpcFlags = 0x93 * 4;
    l.unitDescriptorSize = 0xBC * 4;

    l.descPlayerFlags = (0xBC + 0x02) * 4;
    l.descPlayerInvSlot1 = (0xBC + 0x12A) * 4; // the 19 equipment slots before the bag slots
    l.descPlayerBagSlot1 = (0xBC + 0x150) * 4;
    l.descPlayerPackSlot1 = (0xBC + 0x158) * 4;
    l.descPlayerXp = (0xBC + 0x210) * 4;
    l.descPlayerNextLevelXp = (0xBC + 0x211) * 4;
    l.descPlayerSkills = (0xBC + 0x212) * 4;
    // As 3.3.5a, but 1.x has fewer percentage fields and half the explored-zone
    // block, putting PLAYER_FIELD_COINAGE 0x1CA fields past the skill block.
    l.descPlayerCoinage = (0xBC + 0x212 + 0x1CA) * 4;

    // ITEM_END = 0x06 + 0x2A
    l.descItemStackCount = (0x06 + 0x08) * 4;
    // Published 1.12.1 field numbers, not yet checked against a real client.
    l.descItemDurability = (0x06 + 0x2E) * 4;
    l.descItemMaxDurability = (0x06 + 0x2F) * 4;
    l.descContainerNumSlots = 0x30 * 4;
    l.descContainerSlot1 = (0x30 + 0x02) * 4;

    l.descGameObjectCreatedBy = 0x06 * 4;
    l.descGameObjectPosX = 0x0F * 4;
    l.descGameObjectType = 0x15 * 4; // GAMEOBJECT_TYPE_ID
    l.gameObjectTypeShift = 0;

    l.descCorpseOwner = 0x06 * 4;
    l.descCorpsePosX = 0x09 * 4;

    // 1.x marks units tapped by someone else only; 0x0008 means "rooted" here.
    l.dynFlagTappedByPlayer = 0;

    // UNIT_NPC_FLAGS: 0x4 vendor, 0x4000 repair; 3.x moved them to 0x80 (an
    // innkeeper here) and 0x1000.
    l.npcFlagVendor = 0x00000004;
    l.npcFlagRepair = 0x00004000;
    return l;
}

} // namespace

const ClientLayout& layout1121()
{
    static const ClientLayout layout = make();
    return layout;
}

} // namespace og::wowclient
