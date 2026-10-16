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
 *  File: ModernClientReader.cpp
 *
 *  Desc:
 *  Implementation of ModernClientReader (WoW client memory reading,
 *  process inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/ModernClientReader.h"

#include "og/state/AreaTable.h"
#include "og/state/SpecialBags.h"

#include "og/wowclient/ClientLayout.h"
#include "og/wowclient/FactionTemplate.h"
#include "og/wowclient/GameObjectKind.h"
#include "og/wowclient/ZoneMaps.h"
#include "og/wowclient/ItemNames.h"
#include "og/wowclient/ProcessMemory.h"
#include "og/wowclient/SpellNames.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <set>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_set>

namespace og::wowclient
{

namespace c = consts;

namespace
{

template <class T> T field(const std::uint8_t* data, std::size_t offset)
{
    T v{};
    std::memcpy(&v, data + offset, sizeof(T));
    return v;
}

// A user-space address of a 64-bit process; anything else is not a pointer.
bool isPointer(std::uint64_t p)
{
    return p >= 0x10000 && p < 0x7FFFFFFFFFFFull;
}

int toInt(std::int64_t v)
{
    return static_cast<int>(std::clamp<std::int64_t>(v, 0, std::numeric_limits<int>::max()));
}

// Only mana has been found in the unit fields so far.
PowerType powerTypeOf(PlayerClass playerClass)
{
    switch (playerClass)
    {
    case PlayerClass::Warrior:
        return PowerType::Rage;
    case PlayerClass::Rogue:
        return PowerType::Energy;
    default:
        return PowerType::Mana;
    }
}

} // namespace

Guid foldGuid(const Guid128& guid)
{
    return guid.low ^ (guid.high * 0x9E3779B97F4A7C15ull);
}

ModernClientReader::ModernClientReader(const IProcessMemory& memory, const ModernLayout& layout, std::uint64_t imageBase, const SpellNames& spellNames) : memory_(memory), L_(layout), imageBase_(imageBase), spellNames_(spellNames)
{
    headerSize_ = L_.unitMovement ? std::max(L_.unitMovement + 8, L_.unitCastingSpell + 4) : std::max({L_.unitPosition + 12, L_.unitFacing + 4, L_.unitCastingSpell + 4});
    // On 1.15.9 the dynamic flags sit in the object header, which is read anyway.
    dynamicFlagsInHeader_ = L_.unitDynamicFlags + 4 <= headerSize_;
    std::vector<std::uint32_t> fields = {dynamicFlagsInHeader_ ? L_.unitPower : L_.unitDynamicFlags, L_.unitPower, L_.unitHealth, L_.unitMaxHealth, L_.unitTarget, L_.unitBytes0, L_.unitLevel, L_.unitFactionTemplate, L_.unitFlags, L_.unitMaxPower};

    for (const auto owner : {L_.unitSummonedBy, L_.unitCreatedBy})
    {
        if (owner)
        {
            fields.push_back(owner);
        }
    }

    fieldsBegin_ = *std::min_element(fields.begin(), fields.end());
    fieldsSize_ = *std::max_element(fields.begin(), fields.end()) + 16 - fieldsBegin_; // the widest field is a GUID
}

std::uint64_t ModernClientReader::objectManager() const
{
    auto isValidMgr = [this](std::uint64_t mgr)
    {
        if (!isPointer(mgr))
        {
            return false;
        }

        const auto slots = memory_.read<std::uint64_t>(mgr + L_.mgrSlots).value_or(0);
        const auto count = memory_.read<std::uint32_t>(mgr + L_.mgrSlotCount).value_or(0);
        return isPointer(slots) && count > 0 && count <= L_.maxSlots;
    };

    const auto primary = memory_.read<std::uint64_t>(imageBase_ + L_.objectManager).value_or(0);

    if (isValidMgr(primary))
    {
        return primary;
    }

    const auto fallback = L_.objectManagerFallback ? memory_.read<std::uint64_t>(imageBase_ + L_.objectManagerFallback).value_or(0) : 0;

    if (isValidMgr(fallback))
    {
        return fallback;
    }

    if (isPointer(primary))
    {
        return primary;
    }

    if (isPointer(fallback))
    {
        return fallback;
    }

    return 0;
}

Guid128 ModernClientReader::readGuid128(std::uint64_t address) const
{
    std::array<std::uint64_t, 2> halves{};

    if (!memory_.read(address, halves.data(), sizeof(halves)))
    {
        return {};
    }

    return {halves[0], halves[1]};
}

std::vector<ModernClientReader::Object> ModernClientReader::objects() const
{
    std::vector<Object> out;
    const std::uint64_t mgr = objectManager();

    if (!isPointer(mgr))
    {
        return out;
    }

    const auto slots = memory_.read<std::uint64_t>(mgr + L_.mgrSlots).value_or(0);
    const auto count = std::min(memory_.read<std::uint32_t>(mgr + L_.mgrSlotCount).value_or(0), L_.maxSlots);

    if (!isPointer(slots) || count == 0)
    {
        return out;
    }

    std::vector<std::uint8_t> table(std::size_t{count} * L_.slotSize);

    if (!memory_.read(slots, table.data(), table.size()))
    {
        return out;
    }

    std::vector<std::uint8_t> header(std::max({L_.objEntry + 8, L_.objGuid + 16, L_.objTypeId + 1}));

    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto entry = field<std::uint64_t>(table.data(), std::size_t{i} * L_.slotSize + L_.slotEntry);

        if (!isPointer(entry))
        {
            continue;
        }

        // World objects and items hang off different fields of the entry. Free
        // slots keep stale pointers; a live object links back to its entry.
        std::uint64_t base = 0;

        for (const std::uint32_t link : {L_.entryObject, L_.entryItemObject})
        {
            const auto candidate = link ? memory_.read<std::uint64_t>(entry + link).value_or(0) : 0;

            if (isPointer(candidate) && memory_.read(candidate, header.data(), header.size()) && field<std::uint64_t>(header.data(), L_.objEntry) == entry)
            {
                base = candidate;
                break;
            }
        }

        if (base == 0)
        {
            continue;
        }

        Object obj;
        obj.base = base;
        obj.type = header[L_.objTypeId];
        obj.guid128 = {field<std::uint64_t>(header.data(), L_.objGuid), field<std::uint64_t>(header.data(), L_.objGuid + 8)};

        if (obj.guid128.high == 0)
        {
            continue;
        }

        obj.guid = foldGuid(obj.guid128);
        out.push_back(obj);
    }

    return out;
}

std::vector<ModernClientReader::ItemObject> ModernClientReader::itemObjects() const
{
    std::vector<ItemObject> out;

    for (const auto& obj : objects())
    {
        if (obj.type != modern::TypeItem && obj.type != modern::TypeContainer)
        {
            continue;
        }

        out.push_back({obj.base, memory_.read<std::uint32_t>(obj.base + L_.itemEntry).value_or(0), memory_.read<std::uint32_t>(obj.base + L_.itemStackCount).value_or(0)});
    }

    return out;
}

bool ModernClientReader::inWorld() const
{
    const auto all = objects();
    return std::any_of(all.begin(), all.end(), [](const Object& o) { return o.type == modern::TypeActivePlayer; });
}

std::string ModernClientReader::cachedName(Guid guid, std::uint64_t base, std::uint32_t cacheOffset, std::uint32_t nameOffset) const
{
    if (const auto it = names_.find(guid); it != names_.end())
    {
        return it->second;
    }

    const auto entry = memory_.read<std::uint64_t>(base + cacheOffset).value_or(0);

    if (!isPointer(entry))
    {
        return {};
    }

    const auto text = memory_.read<std::uint64_t>(entry + nameOffset).value_or(0);

    if (!isPointer(text))
    {
        return {};
    }

    std::string name = memory_.readString(text, 64);

    // Not cached while empty: the client fills the cache in after the object appears.
    if (!name.empty())
    {
        names_.emplace(guid, name);
    }

    return name;
}

void ModernClientReader::readAuras(std::uint64_t base, std::uint32_t clock, std::vector<Aura>& auras) const
{
    if (!L_.unitAuraData || !L_.unitAuraCount || !L_.auraSize)
    {
        return;
    }

    const auto count = memory_.read<std::uint32_t>(base + L_.unitAuraCount).value_or(0);
    const auto data = memory_.read<std::uint64_t>(base + L_.unitAuraData).value_or(0);

    if (count == 0 || count > 512 || !isPointer(data))
    {
        return;
    }

    std::vector<std::uint8_t> raw(std::size_t{count} * L_.auraSize);

    if (!memory_.read(data, raw.data(), raw.size()))
    {
        return;
    }

    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t* record = raw.data() + std::size_t{i} * L_.auraSize;
        const auto spellId = field<std::uint32_t>(record, L_.auraSpellId);

        if (spellId == 0)
        {
            continue; // a removed aura leaves its record empty
        }

        const auto flags = field<std::uint16_t>(record, L_.auraFlags);
        Aura a;
        a.spellId = spellId;
        a.harmful = (flags & modern::AuraFlagNegative) != 0;
        a.stacks = std::max(1, static_cast<int>(field<std::uint8_t>(record, L_.auraStacks)));
        a.caster = foldGuid({field<std::uint64_t>(record, L_.auraCaster), field<std::uint64_t>(record, L_.auraCaster + 8)});

        if (field<std::uint32_t>(record, L_.auraDuration) != 0 && clock != 0)
        {
            const std::int64_t left = std::int64_t{field<std::uint32_t>(record, L_.auraExpiration)} - clock;
            a.remaining = static_cast<float>(std::max<std::int64_t>(left, 0)) / 1000.0f;
        }

        if (const std::string* name = spellNames_.find(spellId))
        {
            a.name = *name;
        }

        auras.push_back(std::move(a));
    }
}

bool ModernClientReader::readUnit(const Object& obj, std::uint32_t clock, UnitState& u) const
{
    std::vector<std::uint8_t> head(headerSize_);
    std::vector<std::uint8_t> fields(fieldsSize_);

    if (!memory_.read(obj.base, head.data(), head.size()) || !memory_.read(obj.base + fieldsBegin_, fields.data(), fields.size()))
    {
        return false;
    }

    // Offsets into `fields`, which starts fieldsBegin_ bytes into the object.
    const std::uint8_t* f = fields.data();
    const std::uint32_t at = fieldsBegin_;

    u.guid = obj.guid;
    u.isPlayer = obj.type != modern::TypeUnit;

    if (!u.isPlayer)
    {
        u.entry = static_cast<std::uint32_t>((obj.guid128.high >> 6) & 0x7FFFFF);
    }

    u.level = static_cast<int>(field<std::uint32_t>(f, L_.unitLevel - at));
    u.factionTemplate = field<std::uint32_t>(f, L_.unitFactionTemplate - at);
    u.health = toInt(field<std::int64_t>(f, L_.unitHealth - at));
    u.maxHealth = toInt(field<std::int64_t>(f, L_.unitMaxHealth - at));

    const auto bytes0 = field<std::uint32_t>(f, L_.unitBytes0 - at);
    u.powerType = powerTypeOf(static_cast<PlayerClass>((bytes0 >> 8) & 0xFF));

    if (u.isPlayer)
    {
        u.playerClass = static_cast<PlayerClass>((bytes0 >> 8) & 0xFF);
        u.race = static_cast<int>(bytes0 & 0xFF);
    }

    // The class's own power comes first in the power arrays: mana, or a
    // rogue's energy (100/100 at the same offsets).
    u.power = static_cast<int>(field<std::uint32_t>(f, L_.unitPower - at));
    u.maxPower = static_cast<int>(field<std::uint32_t>(f, L_.unitMaxPower - at));

    if (u.powerType == PowerType::Rage)
    { // tenths of rage, as on the older clients
        u.power /= 10;
        u.maxPower /= 10;
    }

    const auto flags = field<std::uint32_t>(f, L_.unitFlags - at);
    u.inCombat = (flags & c::UnitFlagInCombat) != 0;
    u.isSkinnable = (flags & c::UnitFlagSkinnable) != 0;
    u.isFleeing = (flags & c::UnitFlagFleeing) != 0;
    u.isAttackable = (flags & (c::UnitFlagNonAttackable | c::UnitFlagImmuneToPc | c::UnitFlagNotSelectable)) == 0;
    u.isDead = u.health == 0;

    if (L_.unitNpcFlags)
    {
        const auto npcFlags = field<std::uint32_t>(f, L_.unitNpcFlags - at);
        u.isVendor = (npcFlags & modern::NpcFlagVendor) != 0;
        u.canRepair = (npcFlags & modern::NpcFlagRepair) != 0;

        // A vendor, trainer or quest giver is no mob to pull.
        if (npcFlags != 0)
        {
            u.isAttackable = false;
        }
    }

    if (L_.unitQuestGiverStatus && L_.unitQuestGiverStatus + 4 <= headerSize_ && !u.isPlayer)
    {
        const auto status = field<std::uint32_t>(head.data(), L_.unitQuestGiverStatus);
        u.questMarker = (status & modern::QuestGiverReward) ? QuestMarker::Reward : (status & modern::QuestGiverAvailable) ? QuestMarker::Available : (status & modern::QuestGiverIncomplete) ? QuestMarker::Incomplete : QuestMarker::None;
    }

    // A beast is skinnable from the moment it dies, loot or not: only the dynamic flags say.
    const auto dynamicFlags = dynamicFlagsInHeader_ ? field<std::uint32_t>(head.data(), L_.unitDynamicFlags) : field<std::uint32_t>(f, L_.unitDynamicFlags - at);
    u.isLootable = u.isDead && (dynamicFlags & modern::UnitDynFlagLootable) != 0;
    u.isTappedByOther = (dynamicFlags & modern::UnitDynFlagTappedByOther) != 0;
    u.targetGuid = foldGuid({field<std::uint64_t>(f, L_.unitTarget - at), field<std::uint64_t>(f, L_.unitTarget - at + 8)});

    // The pet's owner: summoned by, or else created by.
    for (const auto owner : {L_.unitSummonedBy, L_.unitCreatedBy})
    {
        if (!owner || u.ownerGuid)
        {
            continue;
        }

        u.ownerGuid = foldGuid({field<std::uint64_t>(f, owner - at), field<std::uint64_t>(f, owner - at + 8)});
    }

    if (L_.unitMovement)
    {
        // Position and facing sit in the movement block, in one read.
        const auto movement = field<std::uint64_t>(head.data(), L_.unitMovement);
        const std::uint32_t begin = std::min(L_.unitPosition, L_.unitFacing);
        std::array<std::uint8_t, 64> block{};
        const std::uint32_t size = std::max(L_.unitPosition + 12, L_.unitFacing + 4) - begin;

        if (isPointer(movement) && size <= block.size() && memory_.read(movement + begin, block.data(), size))
        {
            u.position = {field<float>(block.data(), L_.unitPosition - begin), field<float>(block.data(), L_.unitPosition - begin + 4), field<float>(block.data(), L_.unitPosition - begin + 8)};
            u.heading = field<float>(block.data(), L_.unitFacing - begin);
        }
    }
    else
    {
        u.position = {field<float>(head.data(), L_.unitPosition), field<float>(head.data(), L_.unitPosition + 4), field<float>(head.data(), L_.unitPosition + 8)};
        u.heading = field<float>(head.data(), L_.unitFacing);
    }

    u.castingSpell = field<std::uint32_t>(head.data(), L_.unitCastingSpell);
    u.isCasting = u.castingSpell != 0;

    if (!u.isPlayer)
    {
        u.name = cachedName(u.guid, obj.base, L_.unitNameCache, L_.nameCacheName);
        // Critters here have the level of the zone (a level 5 Deer), but only a
        // hit point or so; the creature cache says so outright.
        u.isCritter = isCritterFaction(u.factionTemplate) || (u.maxHealth > 0 && u.maxHealth <= 10);

        if (!u.isCritter && L_.hasCreatureCacheType)
        {
            const auto entry = memory_.read<std::uint64_t>(obj.base + L_.unitNameCache).value_or(0);

            if (isPointer(entry))
            {
                u.isCritter = memory_.read<std::uint8_t>(entry + L_.creatureCacheType).value_or(0) == 8;
                // Its rank two fields on, after the type and the family: 0
                // normal, 1 elite, 2 rare elite, 3 boss, 4 rare. Found on
                // 70205 by Lake Thresher and Blackrock Sentry (1) among
                // murlocs and condors (0).
                // Not read: not known, and left as it was.
                if (const auto rank = memory_.read<std::uint8_t>(entry + L_.creatureCacheType + 8))
                {
                    u.classification = *rank <= static_cast<std::uint8_t>(Classification::Rare) ? static_cast<Classification>(*rank) : Classification::Normal;
                }
            }
        }
    }

    readAuras(obj.base, clock, u.auras);
    return true;
}

void ModernClientReader::readPlayer(const Object& obj, PlayerState& p) const
{
    const auto bytes0 = memory_.read<std::uint32_t>(obj.base + L_.unitBytes0).value_or(0);
    p.playerClass = static_cast<PlayerClass>((bytes0 >> 8) & 0xFF);
    p.race = static_cast<int>(bytes0 & 0xFF);

    // Stand state: 1 sat on the ground, 2-6 on a chair or a bed, kneeling
    // (1.60.1: 0 -> 1 on /sit, back on /stand).
    if (L_.unitBytes1)
    {
        const std::uint8_t standState = memory_.read<std::uint8_t>(obj.base + L_.unitBytes1).value_or(0);
        p.isSitting = standState >= 1 && standState <= 6;
    }

    p.setPower(p.powerType, p.power, p.maxPower);
    p.reaction = Reaction::Friendly;

    // The character's name follows its GUID; checking the GUID keeps a stale
    // name from a previous character out.
    if (L_.playerNameGuid && readGuid128(imageBase_ + L_.playerNameGuid) == obj.guid128)
    {
        p.name = memory_.readString(imageBase_ + L_.playerNameGuid + 16, 48);

        if (L_.lastNameAfterFirst && !p.name.empty())
        {
            if (const auto last = memory_.readString(imageBase_ + L_.playerNameGuid + 16 + L_.lastNameAfterFirst, 48); !last.empty())
            {
                p.name += ' ' + last;
            }
        }
    }

    if (obj.type != modern::TypeActivePlayer)
    {
        return;
    }

    // A ghost has 1 hp and the Ghost aura (8326; a night elf's Wisp Spirit is 20584).
    p.isGhost = std::any_of(p.auras.begin(), p.auras.end(), [](const Aura& a) { return a.spellId == 8326 || a.spellId == 20584; });

    if (p.isGhost)
    {
        p.isDead = false;
    }

    if (L_.playerCoinage)
    {
        if (const auto copper = memory_.read<std::uint64_t>(obj.base + L_.playerCoinage))
        {
            p.money = static_cast<std::int64_t>(*copper);
        }
    }

    if (L_.playerAutoAttackTarget)
    {
        p.isAutoAttacking = readGuid128(obj.base + L_.playerAutoAttackTarget) != Guid128{};
    }

    // Only where the slot's maximum says it holds combo points (a rogue's 5).
    if (L_.playerComboPoints && memory_.read<std::uint32_t>(obj.base + L_.playerMaxComboPoints).value_or(0) == 5)
    {
        p.comboPoints = static_cast<int>(memory_.read<std::uint32_t>(obj.base + L_.playerComboPoints).value_or(0));
    }

    if (L_.playerXp)
    {
        p.xp = static_cast<int>(memory_.read<std::uint32_t>(obj.base + L_.playerXp).value_or(0));
    }

    if (L_.playerNextLevelXp)
    {
        p.nextLevelXp = static_cast<int>(memory_.read<std::uint32_t>(obj.base + L_.playerNextLevelXp).value_or(0));
    }

    readSkills(obj, p);
    readQuestLog(obj, p);
}

void ModernClientReader::readQuestLog(const Object& obj, PlayerState& p) const
{
    if (!L_.playerQuestLog || !L_.playerQuestLogStride)
    {
        return;
    }

    // Classic's log holds 20 quests; the 25 read leave room for a longer one.
    constexpr std::size_t kSlots = 25;
    std::vector<std::uint8_t> log(kSlots * L_.playerQuestLogStride);

    if (!memory_.read(obj.base + L_.playerQuestLog, log.data(), log.size()))
    {
        return;
    }

    p.questLog.clear();
    p.questProgress.clear();

    for (std::size_t i = 0; i < kSlots; ++i)
    {
        const std::size_t entry = i * L_.playerQuestLogStride;
        const auto id = field<std::uint32_t>(log.data(), entry);

        if (id == 0)
        {
            continue;
        }

        p.questLog.push_back(id);

        if (!L_.playerQuestLogProgress)
        {
            continue;
        }

        PlayerState::QuestProgress progress{id, {}};

        for (std::size_t k = 0; k < progress.kills.size(); ++k)
        {
            if (L_.playerQuestLogProgress + 2 * (k + 1) <= L_.playerQuestLogStride)
            {
                progress.kills[k] = field<std::uint16_t>(log.data(), entry + L_.playerQuestLogProgress + 2 * k);
            }
        }

        p.questProgress.push_back(progress);
    }
}

void ModernClientReader::readSkills(const Object& obj, PlayerState& p) const
{
    if (!L_.playerSkillIds || !L_.playerSkillStride)
    {
        return;
    }

    const std::size_t count = L_.playerSkillStride / 2;
    std::vector<std::uint16_t> lines(count * 3);

    if (!memory_.read(obj.base + L_.playerSkillIds, lines.data(), lines.size() * 2))
    {
        return;
    }

    p.skills.clear();

    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint16_t id = lines[i];
        const std::uint16_t maxRank = lines[2 * count + i];

        if (id == 0 || maxRank == 0)
        {
            continue;
        }

        p.skills.push_back(PlayerSkill{id, lines[count + i], maxRank, 0, 0});
    }
}

void ModernClientReader::readBags(const std::vector<Object>& all, const Object& self, PlayerState& p) const
{
    if (L_.playerInventory == 0)
    {
        return;
    }

    struct Item
    {
        const Object* object = nullptr;
        std::uint32_t entry = 0;
        int stack = 1;
        Guid128 container;
    };
    // Entry, container and stack count come from one read per item.
    const std::uint32_t begin = std::min({L_.itemEntry, L_.itemContainedIn, L_.itemStackCount});
    const std::uint32_t size = std::max({L_.itemEntry + 4, L_.itemContainedIn + 16, L_.itemStackCount + 4}) - begin;
    std::vector<std::uint8_t> block(size);
    std::unordered_map<Guid, Item> items;

    for (const auto& obj : all)
    {
        if (obj.type != modern::TypeItem && obj.type != modern::TypeContainer)
        {
            continue;
        }

        if (!memory_.read(obj.base + begin, block.data(), block.size()))
        {
            continue;
        }

        Item item;
        item.object = &obj;
        item.entry = field<std::uint32_t>(block.data(), L_.itemEntry - begin);
        item.stack = std::max(1, static_cast<int>(field<std::uint32_t>(block.data(), L_.itemStackCount - begin)));
        item.container = {field<std::uint64_t>(block.data(), L_.itemContainedIn - begin), field<std::uint64_t>(block.data(), L_.itemContainedIn - begin + 8)};
        items.emplace(obj.guid, item);
    }

    const int backpackSlots = L_.inventoryBackpackCount + L_.inventoryBackpackExtraCount;
    const int slots = std::max(L_.inventoryBagFirst + L_.inventoryBagCount, L_.inventoryBackpackFirst + backpackSlots);
    std::vector<std::uint8_t> inventory(static_cast<std::size_t>(slots) * 16);

    if (!memory_.read(self.base + L_.playerInventory, inventory.data(), inventory.size()))
    {
        return;
    }

    const auto slotGuid = [&](int slot)
    {
        const auto at = static_cast<std::size_t>(slot) * 16;
        return Guid128{field<std::uint64_t>(inventory.data(), at), field<std::uint64_t>(inventory.data(), at + 8)};
    };

    if (L_.itemEnchantments && L_.itemEnchantmentSize)
    {
        // Inventory slot 16 (the 16th record): the main hand. Its second enchantment is the temporary one.
        const Guid128 mainHand = slotGuid(15);
        const auto it = mainHand.high != 0 ? items.find(foldGuid(mainHand)) : items.end();
        p.mainHandTempEnchant = 0;

        if (it != items.end())
        {
            const std::uint64_t at = it->second.object->base + L_.itemEnchantments + L_.itemEnchantmentSize;
            const auto id = memory_.read<std::uint32_t>(at).value_or(0);
            const auto expires = memory_.read<std::uint32_t>(at + 4).value_or(0);
            const auto unixNow = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            p.mainHandTempEnchant = id;
            p.mainHandTempEnchantSecondsLeft = id ? static_cast<int>(std::max<std::int64_t>(0, expires - unixNow)) : 0;
        }
    }

    std::map<std::uint32_t, int> counts;
    int freeSlots = 0;
    int usedSlots = 0;
    // The extra backpack slots hold loot like any other; they count as free only
    // when the backpack is seen using them, since not every character has them.
    int extraFree = 0;
    bool extraUsed = false;

    for (int i = 0; i < backpackSlots; ++i)
    {
        const bool extra = i >= L_.inventoryBackpackCount;
        const Guid128 guid = slotGuid(L_.inventoryBackpackFirst + i);

        if (guid.high == 0)
        {
            ++(extra ? extraFree : freeSlots);
            continue;
        }

        extraUsed = extraUsed || extra;
        ++usedSlots;

        if (const auto it = items.find(foldGuid(guid)); it != items.end())
        {
            counts[it->second.entry] += it->second.stack;
        }
    }

    // The player says how many slots the backpack has. Anything but the two
    // sizes it can be is not trusted, and the backpack's own use decides.
    const auto packSize = L_.playerBackpackSlots ? memory_.read<std::uint8_t>(self.base + L_.playerBackpackSlots).value_or(0) : std::uint8_t{0};

    if (packSize == L_.inventoryBackpackCount + L_.inventoryBackpackExtraCount)
    {
        freeSlots += extraFree;
    }
    else if (packSize != L_.inventoryBackpackCount && extraUsed)
    {
        freeSlots += extraFree;
    }

    for (int i = 0; i < L_.inventoryBagCount; ++i)
    {
        const Guid128 bag = slotGuid(L_.inventoryBagFirst + i);
        const auto it = bag.high != 0 ? items.find(foldGuid(bag)) : items.end();

        if (it == items.end() || it->second.object->type != modern::TypeContainer)
        {
            continue;
        }

        // 0 until a bag has been probed on the client: its free slots are not known.
        const auto bagSlots = L_.containerSlots ? std::min<std::uint32_t>(memory_.read<std::uint32_t>(it->second.object->base + L_.containerSlots).value_or(0), 36) : 0u;
        int used = 0;

        for (const auto& [guid, item] : items)
        {
            if (item.container != bag)
            {
                continue;
            }

            counts[item.entry] += item.stack;
            ++used;
        }

        usedSlots += used;

        // A herb bag, quiver, ...: its empty slots take no loot.
        if (!isSpecialBag(it->second.entry))
        {
            freeSlots += std::max(0, static_cast<int>(bagSlots) - used);
        }
    }

    p.freeBagSlots = freeSlots;
    p.usedBagSlots = usedSlots;

    // Equipped items at zero durability, or low on it, for a repair trip.
    if (L_.itemMaxDurability && L_.inventoryEquipmentCount)
    {
        int broken = 0;
        int damaged = 0;

        for (int slot = 0; slot < L_.inventoryEquipmentCount; ++slot)
        {
            const Guid128 guid = slotGuid(slot);

            if (guid.high == 0)
            {
                continue;
            }

            const auto it = items.find(foldGuid(guid));

            if (it == items.end())
            {
                continue;
            }

            const std::uint64_t item = it->second.object->base;
            const auto max = memory_.read<std::uint32_t>(item + L_.itemMaxDurability).value_or(0);

            if (max == 0)
            {
                continue;
            }

            const auto durability = memory_.read<std::uint32_t>(item + L_.itemDurability).value_or(max);

            if (durability == 0)
            {
                ++broken;
            }

            if (durabilityLow(durability, max))
            {
                ++damaged;
            }
        }

        p.brokenItems = broken;
        p.damagedItems = damaged;
    }

    p.bagItems.clear();

    for (const auto& [id, count] : counts)
    {
        ItemStack stack;
        stack.itemId = id;
        stack.count = count;

        if (const std::string* name = itemName(id))
        {
            stack.name = *name;
        }

        p.bagItems.push_back(std::move(stack));
    }
}

std::vector<ModernActionSlot> ModernClientReader::actionBars() const
{
    std::vector<ModernActionSlot> out;
    std::array<std::uint32_t, 240> raw{};

    if (L_.actionBars == 0 || !memory_.read(imageBase_ + L_.actionBars, raw.data(), sizeof(raw)))
    {
        return out;
    }

    for (int slot = 0; slot < 120; ++slot)
    {
        const std::uint32_t id = raw[static_cast<std::size_t>(slot) * 2];
        const std::uint32_t flags = raw[static_cast<std::size_t>(slot) * 2 + 1];

        // A kind of action other than a spell, an item or a macro: not a
        // spell by that id, and nothing the bot presses.
        const std::uint32_t kind = flags & modern::ActionFlagTypeMask;

        if (id == 0 || (kind != 0 && kind != modern::ActionFlagItem && kind != modern::ActionFlagMacro))
        {
            continue;
        }

        out.push_back({slot + 1, id, kind == modern::ActionFlagItem, kind == modern::ActionFlagMacro});
    }

    return out;
}

void ModernClientReader::refreshPlayerNames(TimePoint now)
{
    // Walking the whole table is a few hundred small reads: not every tick.
    if (namesRefreshed_ != TimePoint{} && now - namesRefreshed_ < Duration(2000))
    {
        return;
    }

    namesRefreshed_ = now;
    const auto buckets = memory_.read<std::uint64_t>(imageBase_ + L_.playerNameCacheBuckets).value_or(0);
    const auto capacity = memory_.read<std::uint64_t>(imageBase_ + L_.playerNameCacheBuckets - 8).value_or(0);

    if (!isPointer(buckets) || capacity == 0 || capacity > (1u << 20))
    {
        return;
    }

    std::vector<std::uint64_t> heads(capacity);

    if (!memory_.read(buckets, heads.data(), heads.size() * sizeof(std::uint64_t)))
    {
        return;
    }

    if (playerNames_.size() > 5000)
    {
        playerNames_.clear();
    }

    std::array<std::uint8_t, 0x90> node{}; // to the end of a 48-character last name on 1.60.1
    const auto text = [&](std::size_t at)
    {
        if (at >= node.size())
        {
            return std::string();
        }

        const char* start = reinterpret_cast<const char*>(node.data() + at);
        return std::string(start, strnlen(start, node.size() - at));
    };

    for (std::uint64_t head : heads)
    {
        for (int hops = 0; isPointer(head) && hops < 64; ++hops)
        {
            if (!memory_.read(head, node.data(), node.size()))
            {
                break;
            }

            const Guid128 guid{field<std::uint64_t>(node.data(), L_.nameNodeGuid), field<std::uint64_t>(node.data(), L_.nameNodeGuid + 8)};
            std::string name = text(L_.nameNodeName);

            if (!name.empty() && L_.lastNameAfterFirst)
            {
                if (const std::string last = text(L_.nameNodeName + L_.lastNameAfterFirst); !last.empty())
                {
                    name += ' ' + last;
                }
            }

            if (!name.empty())
            {
                playerNames_[foldGuid(guid)] = name;
            }

            head = field<std::uint64_t>(node.data(), L_.nameNodeNext);
        }
    }
}

std::string ModernClientReader::realmName() const
{
    return L_.realmName ? memory_.readString(imageBase_ + L_.realmName, 64) : std::string{};
}

bool ModernClientReader::uiObjectNamed(std::uint64_t object, std::string_view name) const
{
    const auto text = memory_.read<std::uint64_t>(object + L_.uiObjectName);
    // Frame names are globals; 96 is well past the longest the default UI has.
    return text && *text && memory_.readString(*text, 96) == name;
}

void ModernClientReader::loadActionButtonTable() const
{
    if (!L_.uiActionButtonTable || !L_.uiObjectName)
    {
        return;
    }

    std::vector<std::uint8_t> table(102 * 24);
    if (!memory_.read(imageBase_ + L_.uiActionButtonTable, table.data(), table.size()))
    {
        return;
    }

    for (std::size_t i = 0; i < 102; ++i)
    {
        const auto btn = field<std::uint64_t>(table.data(), i * 24);
        const auto cd = field<std::uint64_t>(table.data(), i * 24 + 8);

        if (btn && isPointer(btn))
        {
            const auto namePtr = memory_.read<std::uint64_t>(btn + L_.uiObjectName);
            if (namePtr && *namePtr && isPointer(*namePtr))
            {
                const auto btnName = memory_.readString(*namePtr, 64);
                if (!btnName.empty())
                {
                    uiObjects_[btnName] = btn;
                }
            }
        }

        if (cd && isPointer(cd))
        {
            const auto namePtr = memory_.read<std::uint64_t>(cd + L_.uiObjectName);
            if (namePtr && *namePtr && isPointer(*namePtr))
            {
                const auto cdName = memory_.readString(*namePtr, 64);
                if (!cdName.empty())
                {
                    uiObjects_[cdName] = cd;
                }
            }
        }
    }

    actionButtonTableLoaded_ = true;
}

std::uint64_t ModernClientReader::findUiObject(std::string_view name) const
{
    if (const auto cached = uiObjects_.find(std::string(name)); cached != uiObjects_.end())
    {
        if (uiObjectNamed(cached->second, name))
        {
            return cached->second;
        }

        uiObjects_.erase(cached);
        actionButtonTableLoaded_ = false;
    }

    if (!actionButtonTableLoaded_ && L_.uiActionButtonTable)
    {
        loadActionButtonTable();

        if (const auto cached = uiObjects_.find(std::string(name)); cached != uiObjects_.end() && uiObjectNamed(cached->second, name))
        {
            return cached->second;
        }
    }

    if (!L_.uiTop)
    {
        return 0;
    }

    const auto top = memory_.read<std::uint64_t>(imageBase_ + L_.uiTop);

    if (!top || !*top)
    {
        return 0;
    }

    if (uiObjectNamed(*top, name))
    {
        uiObjects_[std::string(name)] = *top;
        return *top;
    }

    if (name == "GameTooltip")
    {
        const auto tooltip = memory_.read<std::uint64_t>(imageBase_ + L_.uiTop - 8);

        if (tooltip && *tooltip && uiObjectNamed(*tooltip, "GameTooltip"))
        {
            uiObjects_["GameTooltip"] = *tooltip;
            return *tooltip;
        }
    }

    if (!L_.uiTopObjects)
    {
        return 0;
    }

    const std::uint64_t head = *top + L_.uiTopObjects;
    auto node = memory_.read<std::uint64_t>(head);

    // Some 13500 objects on 1.60.1; the bound stops a ring broken by a /reload.
    for (int i = 0; i < 200000 && node && *node && *node != head; ++i)
    {
        const std::uint64_t object = *node - L_.uiObjectLink;

        if (uiObjectNamed(object, name))
        {
            uiObjects_[std::string(name)] = object;
            return object;
        }

        node = memory_.read<std::uint64_t>(*node);
    }

    uiObjects_.erase(std::string(name));
    return 0;
}

std::vector<ModernClientReader::UiTextEntry> ModernClientReader::uiVisibleTexts() const
{
    std::vector<UiTextEntry> out;

    if (!L_.uiTop || !L_.uiFontStringText)
    {
        return out;
    }

    std::set<std::uint64_t> seen;
    const auto name = [this](std::uint64_t object)
    {
        const auto text = memory_.read<std::uint64_t>(object + L_.uiObjectName);
        return text && *text ? memory_.readString(*text, 96) : std::string{};
    };
    const auto top = memory_.read<std::uint64_t>(imageBase_ + L_.uiTop);

    if (!top || !*top)
    {
        return out;
    }

    const std::uint64_t head = *top + L_.uiTopObjects;
    auto node = memory_.read<std::uint64_t>(head);

    for (int i = 0; i < 200000 && node && *node && *node != head; ++i)
    {
        const std::uint64_t object = *node - L_.uiObjectLink;
        node = memory_.read<std::uint64_t>(*node);
        const auto flags = memory_.read<std::uint32_t>(object + L_.uiObjectFlags);

        if (!flags || !(*flags & L_.uiVisibleMask))
        {
            continue;
        }

        std::string frame = name(object);

        if (frame.empty() && L_.uiObjectParent)
        {
            auto parent = memory_.read<std::uint64_t>(object + L_.uiObjectParent);

            for (int up = 0; up < 8 && parent && isPointer(*parent) && frame.empty(); ++up)
            {
                if (const auto n = name(*parent); !n.empty())
                {
                    frame = "~" + n;
                }

                parent = memory_.read<std::uint64_t>(*parent + L_.uiObjectParent);
            }
        }

        const std::uint64_t regions = object + L_.uiFrameRegions;
        auto r = memory_.read<std::uint64_t>(regions);

        for (int j = 0; j < 500 && r && *r && *r != regions; ++j)
        {
            const std::uint64_t region = *r - L_.uiRegionLink;
            r = memory_.read<std::uint64_t>(*r);

            if (!seen.insert(region).second)
            {
                break;
            }

            const auto text = memory_.read<std::uint64_t>(region + L_.uiFontStringText);

            if (!text || !isPointer(*text))
            {
                continue;
            }

            auto value = memory_.readString(*text, 256);

            if (value.empty())
            {
                continue;
            }

            out.push_back({frame, name(region), std::move(value)});
        }
    }

    return out;
}

std::optional<Guid> ModernClientReader::mouseoverGuid() const
{
    if (!L_.mouseoverGuid)
    {
        return std::nullopt;
    }

    return foldGuid(readGuid128(imageBase_ + L_.mouseoverGuid));
}

std::optional<std::string> ModernClientReader::uiText(std::string_view frame, std::string_view fontString) const
{
    if (!L_.uiTop || !L_.uiFontStringText)
    {
        return std::nullopt;
    }

    const std::uint64_t parent = findUiObject(frame);

    if (!parent)
    {
        return std::nullopt;
    }

    const std::uint64_t head = parent + L_.uiFrameRegions;
    auto node = memory_.read<std::uint64_t>(head);

    // A dialog has a dozen regions; a busy frame a few dozen.
    for (int i = 0; i < 500 && node && *node && *node != head; ++i)
    {
        const std::uint64_t region = *node - L_.uiRegionLink;

        if (uiObjectNamed(region, fontString))
        {
            const auto text = memory_.read<std::uint64_t>(region + L_.uiFontStringText);
            // A dialog's text runs to a few hundred characters with its colour codes.
            return text && *text ? memory_.readString(*text, 512) : std::string{};
        }

        node = memory_.read<std::uint64_t>(*node);
    }

    return std::nullopt;
}

std::optional<std::string> ModernClientReader::minimapZoneText() const
{
    if (!L_.uiTop || !L_.uiFontStringText)
    {
        return std::nullopt;
    }

    constexpr std::string_view kName = "MinimapZoneText";
    const auto textOf = [this](std::uint64_t region) -> std::optional<std::string>
    {
        const auto at = memory_.read<std::uint64_t>(region + L_.uiFontStringText);
        return at && isPointer(*at) ? std::optional(memory_.readString(*at, 128)) : std::nullopt;
    };

    if (minimapZoneRegion_ && uiObjectNamed(minimapZoneRegion_, kName))
    {
        return textOf(minimapZoneRegion_);
    }

    minimapZoneRegion_ = 0;
    const auto top = memory_.read<std::uint64_t>(imageBase_ + L_.uiTop);

    if (!top || !*top)
    {
        return std::nullopt;
    }

    const std::uint64_t head = *top + L_.uiTopObjects;
    auto node = memory_.read<std::uint64_t>(head);

    for (int i = 0; i < 200000 && node && *node && *node != head; ++i)
    {
        const std::uint64_t object = *node - L_.uiObjectLink;
        node = memory_.read<std::uint64_t>(*node);
        const std::uint64_t regions = object + L_.uiFrameRegions;
        auto r = memory_.read<std::uint64_t>(regions);
        std::unordered_set<std::uint64_t> seen;

        for (int j = 0; j < 512 && r && *r && *r != regions && seen.insert(*r).second; ++j)
        {
            const std::uint64_t region = *r - L_.uiRegionLink;
            r = memory_.read<std::uint64_t>(*r);

            if (uiObjectNamed(region, kName))
            {
                minimapZoneRegion_ = region;
                return textOf(region);
            }
        }
    }

    return std::nullopt;
}

std::optional<UiFrame> ModernClientReader::uiFrame(std::string_view name) const
{
    if ((!L_.uiTop && !L_.uiActionButtonTable) || !L_.uiObjectRect)
    {
        return std::nullopt;
    }

    const std::uint64_t object = findUiObject(name);
    return object ? frameOf(object) : std::nullopt;
}

std::optional<ModernClientReader::UiCooldown> ModernClientReader::uiCooldown(std::string_view frame) const
{
    if ((!L_.uiTop && !L_.uiActionButtonTable) || !L_.uiCooldownStart || !L_.uiCooldownDuration)
    {
        return std::nullopt;
    }

    const std::uint64_t object = findUiObject(frame);

    if (!object)
    {
        return std::nullopt;
    }

    const auto start = memory_.read<std::uint32_t>(object + L_.uiCooldownStart);
    const auto duration = memory_.read<std::uint32_t>(object + L_.uiCooldownDuration);

    if (!start || !duration)
    {
        return std::nullopt;
    }

    return UiCooldown{*start, *duration};
}

std::uint32_t ModernClientReader::gameTime() const
{
    return L_.gameTime ? memory_.read<std::uint32_t>(imageBase_ + L_.gameTime).value_or(0) : 0;
}

std::optional<UiFrame> ModernClientReader::uiRegion(std::string_view frame, std::string_view name) const
{
    if (!L_.uiTop || !L_.uiObjectRect)
    {
        return std::nullopt;
    }

    const auto parent = findUiObject(frame);
    const auto bounds = frameOf(parent);

    if (!bounds || !bounds->visible)
    {
        return std::nullopt;
    }

    const auto head = parent + L_.uiFrameRegions;
    auto node = memory_.read<std::uint64_t>(head);
    std::unordered_set<std::uint64_t> seen;

    for (int i = 0; i < 500 && node && *node && *node != head && seen.insert(*node).second; ++i)
    {
        const auto region = *node - L_.uiRegionLink;

        if (uiObjectNamed(region, name))
        {
            return frameOf(region);
        }

        node = memory_.read<std::uint64_t>(*node);
    }

    return std::nullopt;
}

std::optional<UiFrame> ModernClientReader::frameOf(std::uint64_t object) const
{
    const std::uint64_t screen = findUiObject("UIParent");

    if (!screen || !object)
    {
        return std::nullopt;
    }

    // Stored bottom, left, top, right, measured up from the bottom left.
    const auto rect = [this](std::uint64_t at) -> std::optional<std::array<float, 4>>
    {
        std::array<float, 4> r{};

        if (!memory_.read(at + L_.uiObjectRect, r.data(), sizeof(r)))
        {
            return std::nullopt;
        }

        return r;
    };
    const auto whole = rect(screen);
    const auto own = rect(object);

    if (!whole || !own || (*whole)[2] <= 0 || (*whole)[3] <= 0)
    {
        return std::nullopt;
    }

    const float height = (*whole)[2];
    const float width = (*whole)[3];
    UiFrame frame;
    frame.left = (*own)[1] / width;
    frame.right = (*own)[3] / width;
    frame.top = 1.0f - (*own)[2] / height;
    frame.bottom = 1.0f - (*own)[0] / height;
    const auto flags = memory_.read<std::uint32_t>(object + L_.uiObjectFlags);
    // A frame that was never laid out has an empty rect; nothing there to click.
    frame.visible = flags && (*flags & L_.uiVisibleMask) && frame.right > frame.left && frame.bottom > frame.top;
    return frame;
}

namespace
{
// Lower case, and the typographic apostrophe (U+2019) as a straight one.
std::string foldUiText(std::string_view text)
{
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text.compare(i, 3, "\xE2\x80\x99") == 0)
        {
            out += '\'';
            i += 2;
            continue;
        }

        out += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
    }

    return out;
}

} // namespace

int sellPriceInLines(const std::vector<std::string>& lines)
{
    constexpr std::string_view kPrice = "Sell Price:";

    for (const auto& line : lines)
    {
        if (!line.starts_with(kPrice))
        {
            continue;
        }

        int copper = 0, amount = 0;
        bool any = false;
        const std::string_view rest = std::string_view(line).substr(kPrice.size());

        for (std::size_t i = 0; i < rest.size(); ++i)
        {
            if (std::isdigit(static_cast<unsigned char>(rest[i])))
            {
                amount = amount * 10 + (rest[i] - '0');
                continue;
            }

            if (rest.substr(i).starts_with("|A:coin-"))
            {
                const std::string_view coin = rest.substr(i + 8);
                const int worth = coin.starts_with("gold") ? 10000 : coin.starts_with("silver") ? 100 : coin.starts_with("copper") ? 1 : 0;
                copper += amount * worth;
                any = any || worth > 0;
                const auto close = rest.find("|a", i);
                i = close == std::string_view::npos ? rest.size() : close + 1;
            }

            amount = 0;
        }

        return any ? copper : -1;
    }

    return -1;
}

bool uiTextContains(std::string_view shown, std::string_view wanted)
{
    if (wanted.empty())
    {
        return false;
    }

    return foldUiText(shown).find(foldUiText(wanted)) != std::string::npos;
}

std::vector<std::string> ModernClientReader::chatFrameLines() const
{
    // A line a player said: it has the player's link. The chat window's font
    // strings sit in a frame of no name whose parent the client does not keep
    // where other frames do, so they are known by that rather than by where
    // they are.
    std::vector<std::string> out;

    for (auto& shown : shownTexts())
    {
        if (shown.text.find("|Hplayer:") != std::string::npos)
        {
            out.push_back(std::move(shown.text));
        }
    }

    return out;
}

std::vector<PlacedText> ModernClientReader::shownTexts() const
{
    std::vector<PlacedText> out;

    if (!L_.uiTop || !L_.uiFontStringText)
    {
        return out;
    }

    const auto top = memory_.read<std::uint64_t>(imageBase_ + L_.uiTop);

    if (!top || !*top)
    {
        return out;
    }

    const std::uint64_t head = *top + L_.uiTopObjects;
    auto node = memory_.read<std::uint64_t>(head);

    for (int i = 0; i < 200000 && node && *node && *node != head; ++i)
    {
        const std::uint64_t object = *node - L_.uiObjectLink;
        node = memory_.read<std::uint64_t>(*node);
        const auto flags = memory_.read<std::uint32_t>(object + L_.uiObjectFlags);

        if (!flags || !(*flags & L_.uiVisibleMask))
        {
            continue;
        }

        const std::uint64_t regions = object + L_.uiFrameRegions;
        auto r = memory_.read<std::uint64_t>(regions);
        std::unordered_set<std::uint64_t> seen;

        // A window of a hundred lines or so; the bound stops a broken ring.
        for (int j = 0; j < 512 && r && *r && *r != regions && seen.insert(*r).second; ++j)
        {
            const std::uint64_t region = *r - L_.uiRegionLink;
            r = memory_.read<std::uint64_t>(*r);
            const auto at = memory_.read<std::uint64_t>(region + L_.uiFontStringText);

            if (at && isPointer(*at))
            {
                // Short first: a long read past the end of the text's page
                // fails as a whole. Longer only for a line that fills it.
                std::string line = memory_.readString(*at, 256);

                if (line.size() >= 255)
                {
                    if (std::string longer = memory_.readString(*at, 1024); longer.size() > line.size())
                    {
                        line = std::move(longer);
                    }
                }

                if (!line.empty())
                {
                    // The rect after scale: bottom, left, top, right.
                    std::array<float, 4> rect{};

                    // A read that failed half way leaves no half of a rect.
                    if (L_.uiObjectRect && !memory_.read(region + L_.uiObjectRect, rect.data(), sizeof rect))
                    {
                        rect = {};
                    }

                    out.push_back({std::move(line), rect[0], rect[1]});
                }
            }
        }
    }

    return out;
}

std::optional<UiFrame> ModernClientReader::uiFrameWithText(std::string_view under, std::string_view text) const
{
    if (text.empty())
    {
        return std::nullopt;
    }

    const auto all = uiFramesWithText(under, text);
    return all.empty() ? std::nullopt : std::optional(all.front());
}

std::vector<UiFrame> ModernClientReader::uiFramesWithText(std::string_view under, std::string_view text) const
{
    std::vector<UiFrame> out;

    if (!L_.uiTop || !L_.uiFontStringText || !L_.uiObjectParent)
    {
        return out;
    }

    const std::uint64_t parent = findUiObject(under);

    if (!parent)
    {
        return out;
    }

    const auto top = memory_.read<std::uint64_t>(imageBase_ + L_.uiTop);

    if (!top || !*top)
    {
        return out;
    }

    const std::uint64_t head = *top + L_.uiTopObjects;
    auto node = memory_.read<std::uint64_t>(head);

    for (int i = 0; i < 200000 && node && *node && *node != head; ++i)
    {
        const std::uint64_t object = *node - L_.uiObjectLink;
        node = memory_.read<std::uint64_t>(*node);
        const auto flags = memory_.read<std::uint32_t>(object + L_.uiObjectFlags);

        if (!flags || !(*flags & L_.uiVisibleMask))
        {
            continue;
        }

        // Somewhere inside `under`: a list's buttons sit a few frames down.
        bool inside = false;
        auto up = memory_.read<std::uint64_t>(object + L_.uiObjectParent);

        for (int depth = 0; depth < 8 && up && isPointer(*up) && !inside; ++depth)
        {
            inside = *up == parent;
            up = memory_.read<std::uint64_t>(*up + L_.uiObjectParent);
        }

        if (!inside)
        {
            continue;
        }

        const std::uint64_t regions = object + L_.uiFrameRegions;
        auto r = memory_.read<std::uint64_t>(regions);
        std::unordered_set<std::uint64_t> seen;

        for (int j = 0; j < 64 && r && *r && *r != regions && seen.insert(*r).second; ++j)
        {
            const std::uint64_t region = *r - L_.uiRegionLink;
            r = memory_.read<std::uint64_t>(*r);
            const auto at = memory_.read<std::uint64_t>(region + L_.uiFontStringText);

            if (!at || !isPointer(*at))
            {
                continue;
            }

            // Titles come coloured ("|cff000000Wolves Across the Border|r") or with a level.
            const std::string shown = memory_.readString(*at, 256);

            if (text.empty() ? !shown.empty() : uiTextContains(shown, text))
            {
                if (const auto frame = frameOf(object))
                {
                    out.push_back(*frame);
                }

                break;
            }
        }
    }

    return out;
}

bool ModernClientReader::supportsUiBagSlots() const
{
    return L_.uiTop && L_.uiObjectParent && L_.uiBagButtonVtable && L_.uiTooltipOwner && L_.uiFontStringText && L_.uiFontStringColor;
}

std::vector<UiBagSlot> ModernClientReader::uiBagSlots() const
{
    std::vector<UiBagSlot> slots;

    if (!supportsUiBagSlots())
    {
        return slots;
    }

    std::unordered_set<std::uint64_t> parents;

    for (int i = 0; i <= 5; ++i)
    {
        const std::string name = i == 0 ? "ContainerFrameCombinedBags" : "ContainerFrame" + std::to_string(i);
        const auto frame = uiFrame(name);

        if (frame && frame->visible)
        {
            parents.insert(findUiObject(name));
        }
    }

    if (parents.empty())
    {
        return slots;
    }

    std::array<float, 4> screen{};
    const auto root = findUiObject("UIParent");

    if (!root || !memory_.read(root + L_.uiObjectRect, screen.data(), sizeof(screen)) || !std::isfinite(screen[2]) || !std::isfinite(screen[3]) || screen[2] <= 0 || screen[3] <= 0)
    {
        return slots;
    }

    const auto top = memory_.read<std::uint64_t>(imageBase_ + L_.uiTop).value_or(0);

    if (!top)
    {
        return slots;
    }

    const auto head = top + L_.uiTopObjects;
    auto node = memory_.read<std::uint64_t>(head).value_or(0);
    std::unordered_set<std::uint64_t> seen;
    const auto size = std::max({L_.uiObjectParent + 8, L_.uiObjectFlags + 4, L_.uiObjectRect + 16, L_.uiObjectName + 8, L_.uiObjectLink + 8});
    std::vector<std::uint8_t> block(size);

    for (int i = 0; i < 200000 && node && node != head && seen.insert(node).second; ++i)
    {
        if (node < L_.uiObjectLink)
        {
            break;
        }

        const auto object = node - L_.uiObjectLink;

        if (!memory_.read(object, block.data(), block.size()))
        {
            break;
        }

        node = field<std::uint64_t>(block.data(), L_.uiObjectLink);

        if (field<std::uint64_t>(block.data(), 0) != imageBase_ + L_.uiBagButtonVtable || field<std::uint64_t>(block.data(), L_.uiObjectName) != 0 || !parents.contains(field<std::uint64_t>(block.data(), L_.uiObjectParent)) || !(field<std::uint32_t>(block.data(), L_.uiObjectFlags) & L_.uiVisibleMask))
        {
            continue;
        }

        const float bottom = field<float>(block.data(), L_.uiObjectRect);
        const float left = field<float>(block.data(), L_.uiObjectRect + 4);
        const float topEdge = field<float>(block.data(), L_.uiObjectRect + 8);
        const float right = field<float>(block.data(), L_.uiObjectRect + 12);
        // Other anonymous bag buttons (the title bar, for example) aren't square.
        const float width = right - left, height = topEdge - bottom;

        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 || std::abs(width - height) > height * 0.1f)
        {
            continue;
        }

        UiFrame frame{true, left / screen[3], 1 - topEdge / screen[2], right / screen[3], 1 - bottom / screen[2]};

        if (frame.left < 0 || frame.top < 0 || frame.right > 1 || frame.bottom > 1)
        {
            continue;
        }

        slots.push_back({object, frame});
    }

    std::sort(slots.begin(), slots.end(), [](const UiBagSlot& a, const UiBagSlot& b) { return a.frame.top == b.frame.top ? a.frame.left < b.frame.left : a.frame.top < b.frame.top; });
    return slots;
}

std::optional<UiItemTooltip> ModernClientReader::uiItemTooltip() const
{
    return readTooltip("GameTooltip", true);
}

std::vector<UiItemTooltip> ModernClientReader::uiComparisonTooltips() const
{
    std::vector<UiItemTooltip> out;

    for (const char* frame : {"ShoppingTooltip1", "ShoppingTooltip2"})
    {
        if (auto tooltip = readTooltip(frame, false))
        {
            out.push_back(std::move(*tooltip));
        }
    }

    return out;
}

std::optional<UiItemTooltip> ModernClientReader::readTooltip(std::string_view frame, bool owned) const
{
    if (!supportsUiBagSlots())
    {
        return std::nullopt;
    }

    const auto visible = uiFrame(frame);

    if (!visible || !visible->visible)
    {
        return std::nullopt;
    }

    const auto tooltip = findUiObject(frame);
    const std::string left = std::string(frame) + "TextLeft", right = std::string(frame) + "TextRight";
    UiItemTooltip out;

    if (!tooltip)
    {
        return std::nullopt;
    }

    const auto owner = memory_.read<std::uint64_t>(tooltip + L_.uiTooltipOwner);

    if (!owner)
    {
        return std::nullopt;
    }

    out.owner = *owner;

    if (owned && !out.owner)
    {
        return std::nullopt;
    }

    const auto head = tooltip + L_.uiFrameRegions;
    auto node = memory_.read<std::uint64_t>(head).value_or(0);
    std::unordered_set<std::uint64_t> seen;

    for (int i = 0; i < 500 && node && node != head && seen.insert(node).second; ++i)
    {
        if (node < L_.uiRegionLink)
        {
            break;
        }

        const auto region = node - L_.uiRegionLink;
        node = memory_.read<std::uint64_t>(node).value_or(0);
        const auto flags = memory_.read<std::uint32_t>(region + L_.uiObjectFlags).value_or(0);

        if (!(flags & L_.uiVisibleMask))
        {
            continue;
        }

        const auto nameAt = memory_.read<std::uint64_t>(region + L_.uiObjectName).value_or(0);
        const auto name = nameAt ? memory_.readString(nameAt, 96) : std::string{};

        if (!name.starts_with(left) && !name.starts_with(right))
        {
            continue;
        }

        const auto textAt = memory_.read<std::uint64_t>(region + L_.uiFontStringText).value_or(0);

        if (!textAt)
        {
            continue;
        }

        const auto text = memory_.readString(textAt, 2048);

        if (text.empty())
        {
            continue;
        }

        out.lines.push_back(text);
        const auto color = memory_.read<std::uint32_t>(region + L_.uiFontStringColor);
        out.lineColors.push_back(color ? *color & 0xffffff : 0xffffff);

        if (name != left + "1")
        {
            continue;
        }

        out.name = text;

        if (!color)
        {
            continue;
        }

        switch (*color & 0xffffff)
        {
        case 0x9d9d9d:
            out.quality = 0;
            break;
        case 0xffffff:
            out.quality = 1;
            break;
        case 0x1eff00:
            out.quality = 2;
            break;
        case 0x0070dd:
            out.quality = 3;
            break;
        case 0xa335ee:
            out.quality = 4;
            break;
        case 0xff8000:
            out.quality = 5;
            break;
        default:
            break;
        }
    }

    // The sell price: the money frame's gold, silver and copper, where it shows.
    const std::string moneyFrame = std::string(frame) + "MoneyFrame1";

    if (const auto money = uiFrame(moneyFrame); money && money->visible)
    {
        int copper = 0;
        bool any = false;

        for (const auto& [coin, worth] : {std::pair{"Gold", 10000}, std::pair{"Silver", 100}, std::pair{"Copper", 1}})
        {
            const std::string button = moneyFrame + coin + "Button";
            const auto shown = uiFrame(button);

            if (!shown || !shown->visible)
            {
                continue;
            }

            if (const auto value = uiText(button, button + "Text"); value && !value->empty())
            {
                copper += std::atoi(value->c_str()) * worth;
                any = true;
            }
        }

        if (any)
        {
            out.sellPrice = copper;
        }
    }

    // 1.60.1 says it in a line: "Sell Price: 1|A:coin-silver:...|a 20|A:coin-copper:...|a".
    if (out.sellPrice < 0)
    {
        out.sellPrice = sellPriceInLines(out.lines);
    }

    // The mouse may have moved while the regions were read.
    if (const auto still = memory_.read<std::uint64_t>(tooltip + L_.uiTooltipOwner); out.name.empty() || !still || *still != out.owner)
    {
        return std::nullopt;
    }

    return out;
}

std::optional<std::pair<std::uint64_t, Guid128>> ModernClientReader::findObject(Guid guid) const
{
    for (const auto& obj : objects())
    {
        if (obj.guid == guid)
        {
            return std::pair{obj.base, obj.guid128};
        }
    }

    return std::nullopt;
}

bool ModernClientReader::readGameObject(const Object& obj, GameObjectState& g) const
{
    std::array<float, 3> pos{};

    if (!memory_.read(obj.base + L_.gameObjectPosition, pos.data(), sizeof(pos)))
    {
        return false;
    }

    g.guid = obj.guid;
    g.entry = static_cast<std::uint32_t>((obj.guid128.high >> 6) & 0x7FFFFF);
    g.position = {pos[0], pos[1], pos[2]};
    g.name = cachedName(g.guid, obj.base, L_.gameObjectNameCache, L_.gameObjectCacheName);
    std::uint8_t type = c::GoTypeChest;

    if (L_.hasGameObjectCacheType)
    {
        const auto entry = memory_.read<std::uint64_t>(obj.base + L_.gameObjectNameCache).value_or(0);

        if (isPointer(entry))
        {
            type = static_cast<std::uint8_t>(memory_.read<std::uint32_t>(entry + L_.gameObjectCacheType).value_or(c::GoTypeGeneric));
        }
    }

    g.kind = classifyGameObject(type, g.name);

    if (L_.gameObjectCreatedBy)
    {
        g.createdBy = foldGuid(readGuid128(obj.base + L_.gameObjectCreatedBy));
    }

    // Gathered or emptied: still listed until the server takes it away.
    if (L_.gameObjectState && (g.kind == GameObjectKind::Herb || g.kind == GameObjectKind::Mineral || g.kind == GameObjectKind::Chest))
    {
        g.canInteract = memory_.read<std::uint8_t>(obj.base + L_.gameObjectState).value_or(1) == 1;
    }

    if (g.kind == GameObjectKind::FishingBobber && L_.gameObjectBobbing)
    {
        g.isBobbing = (memory_.read<std::uint8_t>(obj.base + L_.gameObjectBobbing).value_or(0) & L_.gameObjectBobbingMask) == L_.gameObjectBobbingValue;
    }

    return true;
}

std::optional<GameSnapshot> ModernClientReader::capture(TimePoint now, float visibilityRange)
{
    const auto all = objects();
    const auto self = std::find_if(all.begin(), all.end(), [](const Object& o) { return o.type == modern::TypeActivePlayer; });

    if (self == all.end())
    {
        lastMap_.reset(); // a loading screen: another map may follow
        return std::nullopt;
    }

    const std::uint32_t clock = L_.gameTime ? memory_.read<std::uint32_t>(imageBase_ + L_.gameTime).value_or(0) : 0;

    GameSnapshot s;
    s.timestamp = now;

    if (!readUnit(*self, clock, s.player))
    {
        return std::nullopt;
    }

    readPlayer(*self, s.player);

    // A firing wand: pressing Shoot again would turn it off.
    if (L_.autoRepeatSpell)
    {
        s.player.isShooting = memory_.read<std::uint32_t>(imageBase_ + L_.autoRepeatSpell).value_or(0) != 0;
    }

    // Where the body lies, known however long ago the character died.
    if (L_.corpsePosition && s.player.isDeadOrGhost())
    {
        const auto x = memory_.read<float>(imageBase_ + L_.corpsePosition).value_or(0.0f);
        const auto y = memory_.read<float>(imageBase_ + L_.corpsePosition + 4).value_or(0.0f);
        const auto z = memory_.read<float>(imageBase_ + L_.corpsePosition + 8).value_or(0.0f);

        if ((x != 0.0f || y != 0.0f) && std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
        {
            s.player.corpsePosition = Vec3{x, y, z};
        }
    }

    s.targetGuid = L_.targetGuid ? foldGuid(readGuid128(imageBase_ + L_.targetGuid)) : s.player.targetGuid;

    if (L_.zoneId)
    {
        s.zoneId = memory_.read<std::uint32_t>(imageBase_ + L_.zoneId).value_or(0);
    }

    if (s.zoneId == 0 && L_.unitZoneId)
    {
        s.zoneId = memory_.read<std::uint32_t>(self->base + L_.unitZoneId).value_or(0);
    }

    // A zone of its own says the continent. The map global comes second: on
    // 1.15.9 it read 0xFFFFFFFF after a fresh login in Orgrimmar (Kalimdor).
    // A subzone's guessed zone (canonicalZone) comes last: on 1.60.1 the zone
    // is the area the player stands in, and a house in Dun Morogh came out as
    // a Teldrassil area - the radar and the collisions turned to Kalimdor.
    s.mapId = mapForZone(s.zoneId);

    if (!s.mapId && L_.mapId)
    {
        const auto map = memory_.read<std::uint32_t>(imageBase_ + L_.mapId);

        if (map && *map != 0xFFFFFFFF)
        {
            s.mapId = map;
        }
    }

    if (!s.mapId)
    {
        s.mapId = mapForZone(canonicalZone(s.zoneId));
    }

    // A subzone no table knows (a ghost by Moonbrook, area 919) is still on
    // the map the character was on: only a loading screen changes it. The
    // way stopped half way with "waiting for the client's map ID".
    const auto manager = objectManager();

    if (s.mapId)
    {
        lastMap_ = s.mapId;
        lastMapManager_ = manager;
    }
    else if (lastMap_ && manager == lastMapManager_)
    {
        s.mapId = lastMap_;
    }

    readBags(all, *self, s.player);

    std::vector<Guid> taggedByUs;

    for (const auto& obj : all)
    {
        if (obj.base == self->base)
        {
            continue;
        }

        if (obj.type == modern::TypeUnit || obj.type == modern::TypePlayer)
        {
            UnitState u;

            if (readUnit(obj, clock, u) && distance(u.position, s.player.position) <= visibilityRange)
            {
                if (L_.unitTapper && obj.type == modern::TypeUnit && readGuid128(obj.base + L_.unitTapper) == self->guid128)
                {
                    taggedByUs.push_back(u.guid);
                }

                s.units.push_back(std::move(u));
            }
        }
        else if (obj.type == modern::TypeGameObject)
        {
            GameObjectState g;

            if (readGameObject(obj, g) && distance(g.position, s.player.position) <= visibilityRange)
            {
                s.objects.push_back(std::move(g));
            }
        }
    }

    // Other players' names come from the client's name cache, not the unit.
    if (L_.playerNameCacheBuckets && std::any_of(s.units.begin(), s.units.end(), [](const UnitState& u) { return u.isPlayer && u.name.empty(); }))
    {
        refreshPlayerNames(now);

        for (auto& u : s.units)
        {
            if (u.isPlayer && u.name.empty())
            {
                if (const auto it = playerNames_.find(u.guid); it != playerNames_.end())
                {
                    u.name = it->second;
                }
            }
        }
    }

    // The client shows no one else's tag, only ours: a living mob fighting
    // another player that we have not tagged is theirs - no loot or experience.
    // Only once it is hurt, though: a mob is tagged by the first to damage it,
    // not by whom it runs after. Garrick Padfoot, at full health and chasing
    // a player who ran from him, was nobody's - and was left for that.
    if (L_.unitTapper)
    {
        for (auto& u : s.units)
        {
            if (u.isPlayer || u.isDead || u.targetGuid == 0 || u.targetGuid == s.player.guid)
            {
                continue;
            }

            if (u.maxHealth > 0 && u.health >= u.maxHealth)
            {
                continue;
            }

            if (std::find(taggedByUs.begin(), taggedByUs.end(), u.guid) != taggedByUs.end())
            {
                continue;
            }

            const auto target = std::find_if(s.units.begin(), s.units.end(), [&](const UnitState& other) { return other.guid == u.targetGuid; });
            u.isTappedByOther = u.isTappedByOther || (target != s.units.end() && target->isPlayer);
        }
    }

    const auto templateOf = [&](std::uint32_t id) { return L_.factionTemplate ? L_.factionTemplate(id) : nullptr; };
    const FactionTemplate* mine = templateOf(s.player.factionTemplate);

    if (mine && s.player.factionOverride == Faction::Unknown)
    {
        s.player.factionOverride = mine->side();
    }

    // A race the built-in table predates (1.60.1's race 95, template 3629):
    // without its row nothing near could be judged, and every mob came out
    // Unknown and was never pulled. Stand in a plain player's row - a player,
    // against monsters, and on its side where the race tells which - so the
    // mobs' own rows still say how they stand.
    FactionTemplate standIn;

    if (!mine && s.player.factionTemplate != 0 && L_.factionTemplate)
    {
        standIn.id = s.player.factionTemplate;
        standIn.factionGroupMask = 1;
        standIn.enemyGroupMask = 8;

        switch (s.player.faction())
        {
        case Faction::Alliance:
            standIn.factionGroupMask |= 2;
            standIn.friendGroupMask = 2;
            standIn.enemyGroupMask |= 4;
            break;
        case Faction::Horde:
            standIn.factionGroupMask |= 4;
            standIn.friendGroupMask = 4;
            standIn.enemyGroupMask |= 2;
            break;
        case Faction::Unknown:
            break;
        }

        mine = &standIn;
    }

    for (auto& u : s.units)
    {
        if (u.inCombat && u.targetGuid == s.player.guid)
        {
            u.reaction = Reaction::Hostile;
        }
        else
        {
            u.reaction = reactionBetween(s.player.factionTemplate, mine, u.factionTemplate, templateOf(u.factionTemplate));
        }

        // This client has no NPC flags yet, so nothing else tells a guard from
        // a mob: a unit whose reaction is unknown is not pulled.
        if (u.reaction == Reaction::Unknown)
        {
            u.isAttackable = false;
        }
    }

    const Faction playerFact = s.player.faction();

    for (auto& u : s.units)
    {
        if (!u.isPlayer)
        {
            continue;
        }

        if (u.factionOverride == Faction::Unknown && u.factionTemplate != 0)
        {
            if (const auto* ft = templateOf(u.factionTemplate))
            {
                u.factionOverride = ft->side();
            }
        }

        if (u.faction() == Faction::Unknown && playerFact != Faction::Unknown)
        {
            if (u.reaction == Reaction::Friendly)
            {
                u.factionOverride = playerFact;
            }
            else if (u.reaction == Reaction::Hostile)
            {
                u.factionOverride = (playerFact == Faction::Alliance ? Faction::Horde : Faction::Alliance);
            }
        }
    }

    if (names_.size() > 5000)
    {
        names_.clear();
    }

    return s;
}

} // namespace og::wowclient
