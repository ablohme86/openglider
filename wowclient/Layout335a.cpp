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
 *  File: Layout335a.cpp
 *
 *  Desc:
 *  Implementation of Layout335a (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ClientLayout.h"

// World of Warcraft 3.3.5a (build 12340).
//
// The commonly published values for 12340; player, position, target and
// objects are confirmed against a real client with a probe.
namespace og::wowclient
{

namespace
{

ClientLayout make()
{
    ClientLayout l;
    l.name = "3.3.5a";
    l.build = 12340;
    l.powerSlots = 7;

    // Static addresses
    l.objectManager = 0x00C79CE0; // client connection
    l.curMgrOffset = 0x2ED0;      // connection + this -> object manager
    l.isInGame = 0x00BD0792;
    l.playerName = 0x00C79D18;
    l.targetGuid = 0x00BD07B0;
    l.mouseoverGuid = 0x00BD07A0;
    l.comboPoints = 0x00BD084D;
    l.corpsePosition = 0x00BD0A58;
    l.zoneId = 0x00BD080C;
    l.actionBarFirstSlot = 0x00C1E358;
    l.spellBook = 0x00BE5D8C;
    l.factionTemplateStore = 0x00AD3884;
    l.autoRepeatSpell = 0x00BD07B8;

    // Chat ring buffer (commonly published for 12340, not verified yet).
    l.chatBuffer = 0x00B75A60;
    l.chatPosition = 0x00BCEFF4;
    l.chatMessageStride = 0x17C0;
    l.chatTextOffset = 0x3C;
    l.chatMessageCount = 60;
    l.cvarList = 0x00CA1A04;

    l.cameraManager = 0x00B7436C;
    l.cameraOffset = 0x7E20;
    l.cameraPos = 0x08;    // float x, y, z
    l.cameraMatrix = 0x14; // 3x3 float matrix
    l.cameraFov = 0x40;
    l.cameraAspect = 0x44;

    // Lua execution. Verified against a running 12340 client: the D3D chain
    // resolves and FrameScript::Execute starts with a real prologue.
    l.luaDoString = 0x00819210;      // FrameScript::Execute
    l.d3dDevicePointer = 0x00C5DF88; // -> device container
    l.d3dDeviceOffset = 0x397C;      // container + this -> IDirect3DDevice9
    l.d3dEndSceneIndex = 42;
    // Padding in the image tail; ~350 KiB of it is free on 12340. Only a hint:
    // the executor checks it really is unused and otherwise scans the image.
    l.codeCave = 0x00B76000;
    l.codeCaveSize = 0x1000;

    l.ctmBase = 0x00CA11D8;
    l.ctmDistance = 0x0C;
    l.ctmAction = 0x1C; // Move=4, NpcInteract=5, Loot=6, ObjectInteract=7, None=13
    l.ctmGuid = 0x20;
    l.ctmX = 0x8C;
    l.ctmY = 0x90;
    l.ctmZ = 0x94;
    l.ctmActivatePointer = 0x00BD08F4; // -> CVar autointeract
    l.ctmActivateOffset = 0x30;

    // Object manager and objects
    l.mgrFirstObject = 0xAC;
    l.mgrLocalGuid = 0xC0;
    l.objNext = 0x3C;
    l.objDescriptors = 0x08;
    l.objType = 0x14;
    l.objGuid = 0x30;

    l.unitPosX = 0x798;
    l.unitRotation = 0x7A8;
    l.unitAutoAttackTarget = 0xA20;
    l.unitCastingSpell = 0xA6C;
    l.unitChannelSpell = 0xA80;
    l.unitNamePtr1 = 0x964;
    l.unitNamePtr2 = 0x5C;

    l.auraStorage = AuraStorage::ObjectTable;
    l.auraCount1 = 0xDD0; // -1 => use the second table
    l.auraTable1 = 0xC50;
    l.auraCount2 = 0xC54;
    l.auraTable2 = 0xC58;
    l.auraSize = 0x18;
    l.auraCreatorGuid = 0x00;
    l.auraSpellId = 0x08;
    l.auraFlags = 0x0C;
    l.auraStacks = 0x0E;

    l.gameObjectPosX = 0xE8;
    l.gameObjectNamePtr1 = 0x1A4;
    l.gameObjectNamePtr2 = 0x90;
    l.gameObjectBobbing = 0xBC;

    // Descriptors: OBJECT_END = 0x06, UNIT_END = 0x94
    l.descEntry = 0x03 * 4;
    l.descUnitSummonedBy = 0x0E * 4;
    l.descUnitCreatedBy = 0x10 * 4;
    l.descUnitTarget = 0x12 * 4;
    l.descUnitChannelSpell = 0x16 * 4;
    l.descUnitBytes0 = 0x17 * 4;
    l.descUnitBytes1 = (0x06 + 0x44) * 4;
    l.descUnitHealth = 0x18 * 4;
    l.descUnitPower1 = 0x19 * 4;
    l.descUnitMaxHealth = 0x20 * 4;
    l.descUnitMaxPower1 = 0x21 * 4;
    l.descUnitLevel = 0x36 * 4;
    l.descUnitFactionTemplate = 0x37 * 4;
    l.descUnitFlags = 0x3B * 4;
    l.descUnitDynamicFlags = 0x4F * 4;
    l.descUnitNpcFlags = 0x52 * 4;
    l.unitDescriptorSize = 0x94 * 4;

    l.descPlayerFlags = (0x94 + 0x02) * 4;
    l.descPlayerInvSlot1 = (0x94 + 0xB0) * 4; // verified: a broken staff in slot 15 reads 0/50
    l.descPlayerBagSlot1 = (0x94 + 0xD6) * 4;
    l.descPlayerPackSlot1 = (0x94 + 0xDE) * 4;
    l.descPlayerXp = (0x94 + 0x1E6) * 4;
    l.descPlayerNextLevelXp = (0x94 + 0x1E7) * 4;
    l.descPlayerSkills = (0x94 + 0x1E8) * 4;
    // PLAYER_FIELD_COINAGE sits a fixed 0x216 fields past PLAYER_SKILL_INFO_1_1:
    // the skill block (0x180) then character points, the percentage block,
    // PLAYER_EXPLORED_ZONES_1 (0x80) and PLAYER_REST_STATE_EXPERIENCE.
    l.descPlayerCoinage = (0x94 + 0x1E8 + 0x216) * 4;

    // ITEM_END = 0x06 + 0x3A
    l.descItemStackCount = (0x06 + 0x08) * 4;
    l.descItemDurability = (0x06 + 0x36) * 4;
    l.descItemMaxDurability = (0x06 + 0x37) * 4;
    l.descContainerNumSlots = (0x06 + 0x3A) * 4;
    l.descContainerSlot1 = (0x06 + 0x3A + 0x02) * 4;

    l.descGameObjectCreatedBy = 0x06 * 4;
    l.descGameObjectType = 0x11 * 4; // GAMEOBJECT_BYTES_1: state, type, artkit, animprogress
    l.gameObjectTypeShift = 8;

    l.dynFlagTappedByPlayer = 0x0008;
    return l;
}

} // namespace

const ClientLayout& layout335a()
{
    static const ClientLayout layout = make();
    return layout;
}

} // namespace og::wowclient
