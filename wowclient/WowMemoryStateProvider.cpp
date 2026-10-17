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
 *  File: WowMemoryStateProvider.cpp
 *
 *  Desc:
 *  Implementation of WowMemoryStateProvider (WoW client memory reading,
 *  process inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/WowMemoryStateProvider.h"

#include "og/actions/ActionBinding.h"
#include "og/common/FileUtil.h"
#include "og/state/AreaTable.h"
#include "og/state/Shapeshift.h"
#include "og/state/SpecialBags.h"
#include "og/wowclient/ChatLog.h"
#include "og/wowclient/QuestCounts.h"
#include "og/wowclient/GameObjectKind.h"
#include "og/wowclient/ItemNames.h"
#include "og/wowclient/WowBindings.h"
#include "og/wowclient/ZoneMaps.h"

#include <algorithm>
#include <limits>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace og::wowclient
{

namespace c = consts;

namespace
{

template <class T> T field(const std::uint8_t* data, std::uint32_t offset)
{
    T v{};
    std::memcpy(&v, data + offset, sizeof(T));
    return v;
}

std::string knownClients()
{
    std::string out;

    for (const ClientLayout* l : knownLayouts())
    {
        out += std::format("{}{}{} ({})", out.empty() ? "" : ", ", l->name, l->clientVersion.empty() ? "" : " " + std::string(l->clientVersion), l->build);
    }

    return out;
}

} // namespace

WowMemoryStateProvider::WowMemoryStateProvider(WowProviderConfig config, Logger& log, const IClock& clock) : WowMemoryStateProvider(std::move(config), log, clock, std::make_unique<ProcessMemory>())
{
}

WowMemoryStateProvider::WowMemoryStateProvider(WowProviderConfig config, Logger& log, const IClock& clock, std::unique_ptr<IProcessMemory> memory) : config_(std::move(config)), log_(log), clock_(clock), memory_(std::move(memory))
{
    // Until a process is attached, use the configured client (or 3.3.5a).
    const ClientLayout* configured = config_.client == "auto" ? nullptr : layoutByName(config_.client);
    layout_ = configured ? configured : &layout335a();

    if (memory_ && layout_->readOnly)
    {
        memory_->lockWrites();
    }
}

std::string WowMemoryStateProvider::clientDescription() const
{
    std::scoped_lock lock(infoMutex_);
    return clientDescription_;
}

std::string WowMemoryStateProvider::attachProblem() const
{
    std::scoped_lock lock(infoMutex_);
    return attachProblem_;
}

void WowMemoryStateProvider::setAttachProblem(std::string problem)
{
    {
        std::scoped_lock lock(infoMutex_);

        if (attachProblem_ == problem)
        {
            return; // already reported
        }

        attachProblem_ = problem;
    }

    if (!problem.empty())
    {
        log_.error(logcat::Game, "{}", problem);
    }
}

std::vector<ProcessMemory::ProcessInfo> WowMemoryStateProvider::findClientProcesses() const
{
    if (!config_.processName.empty())
    {
        return ProcessMemory::findProcesses(config_.processName);
    }

    auto found = ProcessMemory::findProcesses("wow.exe");

    for (auto& p : ProcessMemory::findProcesses("wowclassic.exe"))
    {
        found.push_back(std::move(p));
    }

    for (auto& p : ProcessMemory::findProcesses("wowb.exe"))
    {
        found.push_back(std::move(p)); // Classic beta
    }

    return found;
}

std::optional<RunningClient> WowMemoryStateProvider::peekClient()
{
    std::vector<int> candidates;

    if (config_.pid > 0)
    {
        candidates.push_back(config_.pid);
    }
    else
    {
        for (const auto& p : findClientProcesses())
        {
            candidates.push_back(p.pid);
        }
    }

    for (int pid : candidates)
    {
        if (!memory_->attach(pid))
        {
            continue;
        }

        const std::uint64_t base = findImageBase(*memory_);
        const auto version = readClientVersion(*memory_, base);
        memory_->detach();

        if (version)
        {
            return RunningClient{pid, version->build, version->toString(), base >= 0x100000000ull};
        }
    }

    return std::nullopt;
}

bool WowMemoryStateProvider::attach()
{
    std::vector<int> candidates;

    if (config_.pid > 0)
    {
        candidates.push_back(config_.pid);
    }
    else
    {
        for (const auto& p : findClientProcesses())
        {
            candidates.push_back(p.pid);
        }
    }

    if (candidates.empty())
    {
        log_.debug(logcat::Game, "no '{}' process found", config_.processName.empty() ? "WoW.exe, WoWClassic.exe or WowB.exe" : config_.processName);
        return false;
    }

    // Several clients (one left hanging at a loading screen, a second one
    // started): take the first one in the world, else the first that attaches.
    std::optional<int> chosen;
    std::optional<int> fallback;

    for (int pid : candidates)
    {
        if (!memory_->attach(pid))
        {
            continue;
        }

        if (!selectLayout())
        {
            memory_->detach();

            if (candidates.size() == 1)
            {
                return false;
            }

            continue;
        }

        if (candidates.size() == 1 || isInGame())
        {
            chosen = pid;
            break;
        }

        if (!fallback)
        {
            fallback = pid;
        }

        memory_->detach();
    }

    if (!chosen && fallback && memory_->attach(*fallback) && selectLayout())
    {
        chosen = fallback;
    }

    if (chosen)
    {
        const int pid = *chosen;
        log_.info(logcat::Game, "attached to WoW process {} - {}", pid, clientDescription());
        loggedNotInGame_ = false;
        nameCache_.clear();
        lootHookInstalled_ = false;
        chatHookInstalled_ = false;
        chatCVarAddress_ = 0;
        lastChatSeq_ = 0;
        lastRawChatLog_.clear();
        return true;
    }

    if (!memory_->attached() && !attachProblem().empty())
    {
        return false;
    }

    log_.warn(logcat::Game, "could not attach to WoW: {}", memory_->lastError());
    return false;
}

bool WowMemoryStateProvider::selectLayout()
{
    imageBase_ = findImageBase(*memory_);
    const auto version = readClientVersion(*memory_, imageBase_);
    licenseRequired_ = false;
    // Only a registered copy attaches to a 64-bit client, known build or not.
    const auto refuseModern = [&]
    {
        licenseRequired_ = true;
        setAttachProblem(std::format("{} is a 64-bit client - it needs a registered copy of {}", version ? std::format("WoW {} (build {})", version->toString(), version->build) : std::string("this WoW"), applicationName()));
        return false;
    };

    if (!config_.allowModernClients && imageBase_ >= 0x100000000ull)
    {
        return refuseModern();
    }

    const bool automatic = config_.client.empty() || config_.client == "auto";
    const ClientLayout* chosen = nullptr;

    if (!automatic)
    {
        chosen = layoutByName(config_.client);

        if (!chosen)
        {
            setAttachProblem(std::format("unknown client '{}' - supported: {}", config_.client, knownClients()));
            return false;
        }

        if (version && version->build != chosen->build)
        {
            log_.warn(logcat::Game, "client is set to {} but WoW.exe reports {} - using the {} layout anyway", chosen->name, version->toString(), chosen->name);
        }
    }
    else if (version)
    {
        chosen = layoutForBuild(version->build);

        if (!chosen)
        {
            setAttachProblem(std::format("WoW {} (build {}) is not supported - supported: {}", version->toString(), version->build, knownClients()));
            return false;
        }
    }
    else
    {
        chosen = &layout335a();
        log_.warn(logcat::Game, "could not read the WoW.exe version - assuming {}", chosen->name);
    }

    if (chosen->modern && !config_.allowModernClients)
    {
        return refuseModern();
    }

    // The build matched; the version the layout was made for should too.
    if (version && !chosen->clientVersion.empty() && std::format("{}.{}.{}", version->major, version->minor, version->patch) != chosen->clientVersion)
    {
        log_.warn(logcat::Game, "the {} layout is for version {} but WoW.exe reports {} - its offsets may not fit", chosen->name, chosen->clientVersion, version->toString());
    }

    layout_ = chosen;

    // Lock the memory itself, not only the provider's own entry points.
    if (chosen->readOnly)
    {
        memory_->lockWrites();
    }

    modern_ = chosen->modern ? std::make_unique<ModernClientReader>(*memory_, *chosen->modern, imageBase_, spellNames_) : nullptr;
    setAttachProblem({});
    std::scoped_lock lock(infoMutex_);
    clientDescription_ = chosen->clientVersion.empty() ? std::format("WoW {} (build {})", chosen->name, chosen->build) : std::format("{} {} (build {})", chosen->name, chosen->clientVersion, chosen->build);
    return true;
}

std::uint32_t WowMemoryStateProvider::objectManager() const
{
    const ClientLayout& L = *layout_;
    std::uint32_t mgr = memory_->readPointer(L.objectManager).value_or(0);

    if (mgr != 0 && L.curMgrOffset != 0)
    {
        mgr = memory_->readPointer(mgr + L.curMgrOffset).value_or(0);
    }

    return mgr;
}

bool WowMemoryStateProvider::isInGame() const
{
    if (modern_)
    {
        return modern_->inWorld();
    }

    const ClientLayout& L = *layout_;

    if (L.isInGame)
    {
        return memory_->read<std::uint8_t>(L.isInGame).value_or(0) == 1;
    }

    // No in-world flag known: the world is loaded once the local player exists.
    const std::uint32_t mgr = objectManager();
    return mgr != 0 && memory_->read<std::uint64_t>(mgr + L.mgrLocalGuid).value_or(0) != 0;
}

std::vector<WowMemoryStateProvider::RawObject> WowMemoryStateProvider::enumerateObjects(Guid& localGuid) const
{
    const ClientLayout& L = *layout_;
    std::vector<RawObject> out;
    const std::uint32_t mgr = objectManager();

    if (mgr == 0)
    {
        return out;
    }

    localGuid = memory_->read<std::uint64_t>(mgr + L.mgrLocalGuid).value_or(0);

    const std::size_t headerSize = std::max({L.objDescriptors + 4, L.objType + 4, L.objGuid + 8, L.objNext + 4});
    std::vector<std::uint8_t> header(headerSize);
    std::uint32_t p = memory_->readPointer(mgr + L.mgrFirstObject).value_or(0);

    for (int i = 0; i < 20000 && p != 0 && (p & 1) == 0; ++i)
    {
        if (!memory_->read(p, header.data(), header.size()))
        {
            break;
        }

        RawObject obj;
        obj.base = p;
        obj.descriptors = field<std::uint32_t>(header.data(), L.objDescriptors);
        obj.type = field<std::uint32_t>(header.data(), L.objType);
        obj.guid = field<std::uint64_t>(header.data(), L.objGuid);

        if (obj.type >= c::TypeItem && obj.type <= c::TypeCorpse && obj.descriptors != 0)
        {
            out.push_back(obj);
        }

        const auto next = field<std::uint32_t>(header.data(), L.objNext);

        if (next == p)
        {
            break;
        }

        p = next;
    }

    return out;
}

std::string WowMemoryStateProvider::unitName(std::uint32_t base) const
{
    const ClientLayout& L = *layout_;

    if (L.unitNamePtr1 == 0)
    {
        return {};
    }

    const auto cache = memory_->readPointer(base + L.unitNamePtr1);

    if (!cache || *cache == 0)
    {
        return {};
    }

    const auto name = memory_->readPointer(*cache + L.unitNamePtr2);

    if (!name || *name == 0)
    {
        return {};
    }

    return memory_->readString(*name, 64);
}

void WowMemoryStateProvider::readAuras(const RawObject& obj, const std::uint8_t* d, std::vector<Aura>& auras) const
{
    const ClientLayout& L = *layout_;
    auto add = [&](std::uint32_t spellId, bool harmful, int stacks, Guid caster)
    {
        Aura a;
        a.spellId = spellId;
        a.harmful = harmful;
        a.stacks = std::max(1, stacks);
        a.caster = caster; // 0 = unknown

        if (const std::string* n = spellNames_.find(spellId))
        {
            a.name = *n;
        }

        auras.push_back(std::move(a));
    };

    if (L.auraStorage == AuraStorage::Descriptors)
    {
        for (int slot = 0; slot < L.auraSlots; ++slot)
        {
            const auto spellId = field<std::uint32_t>(d, L.descUnitAura + static_cast<std::uint32_t>(slot) * 4);

            if (spellId == 0)
            {
                continue;
            }

            const auto applications = field<std::uint8_t>(d, L.descUnitAuraApplications + static_cast<std::uint32_t>(slot));
            add(spellId, slot >= L.auraPositiveSlots, applications + 1, 0);
        }

        return;
    }

    auto count = memory_->read<std::int32_t>(obj.base + L.auraCount1);
    std::uint32_t table = obj.base + L.auraTable1;

    if (count && *count == -1)
    {
        count = memory_->read<std::int32_t>(obj.base + L.auraCount2);
        table = memory_->readPointer(obj.base + L.auraTable2).value_or(0);
    }

    if (!count || *count <= 0 || *count > 256 || table == 0)
    {
        return;
    }

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(*count) * L.auraSize);

    if (!memory_->read(table, raw.data(), raw.size()))
    {
        return;
    }

    for (int i = 0; i < *count; ++i)
    {
        const std::uint8_t* e = raw.data() + static_cast<std::size_t>(i) * L.auraSize;
        const auto spellId = field<std::uint32_t>(e, L.auraSpellId);

        if (spellId == 0)
        {
            continue;
        }

        const auto flags = field<std::uint8_t>(e, L.auraFlags);
        add(spellId, (flags & 0x80) != 0 /* AFLAG_NEGATIVE */, field<std::uint8_t>(e, L.auraStacks), field<std::uint64_t>(e, L.auraCreatorGuid));
    }
}

bool WowMemoryStateProvider::readUnit(const RawObject& obj, UnitState& u) const
{
    const ClientLayout& L = *layout_;
    std::vector<std::uint8_t> buffer(L.unitDescriptorSize);

    if (!memory_->read(obj.descriptors, buffer.data(), buffer.size()))
    {
        return false;
    }

    const std::uint8_t* d = buffer.data();

    u.guid = obj.guid;
    u.isPlayer = obj.type == c::TypePlayer;
    u.entry = field<std::uint32_t>(d, L.descEntry);
    u.level = static_cast<int>(field<std::uint32_t>(d, L.descUnitLevel));
    u.factionTemplate = L.descUnitFactionTemplate ? field<std::uint32_t>(d, L.descUnitFactionTemplate) : 0;
    u.health = static_cast<int>(field<std::uint32_t>(d, L.descUnitHealth));
    u.maxHealth = static_cast<int>(field<std::uint32_t>(d, L.descUnitMaxHealth));

    const auto bytes0 = field<std::uint32_t>(d, L.descUnitBytes0);

    if (u.isPlayer)
    {
        u.race = static_cast<int>(bytes0 & 0xFF);
    }

    const std::uint32_t pt = (bytes0 >> 24) & 0xFF;
    u.powerType = pt < static_cast<std::uint32_t>(L.powerSlots) ? static_cast<PowerType>(pt) : PowerType::Mana;
    const std::uint32_t pi = static_cast<std::uint32_t>(u.powerType);
    u.power = static_cast<int>(field<std::uint32_t>(d, L.descUnitPower1 + pi * 4));
    u.maxPower = static_cast<int>(field<std::uint32_t>(d, L.descUnitMaxPower1 + pi * 4));

    if (u.powerType == PowerType::Rage || u.powerType == PowerType::RunicPower)
    {
        u.power /= 10;
        u.maxPower /= 10;
    }

    if (L.descUnitBytes1)
    {
        const auto bytes1 = field<std::uint32_t>(d, L.descUnitBytes1);
        const std::uint8_t standState = bytes1 & 0xFF;
        u.isSitting = (standState >= 1 && standState <= 6);
    }

    const auto flags = field<std::uint32_t>(d, L.descUnitFlags);
    const auto dyn = field<std::uint32_t>(d, L.descUnitDynamicFlags);
    u.inCombat = (flags & c::UnitFlagInCombat) != 0;
    u.isSkinnable = (flags & c::UnitFlagSkinnable) != 0;
    u.isFleeing = (flags & c::UnitFlagFleeing) != 0;
    u.isLootable = (dyn & c::DynFlagLootable) != 0;
    u.isTappedByOther = (dyn & c::DynFlagTapped) != 0 && (dyn & L.dynFlagTappedByPlayer) == 0;
    // NPCs with a service (vendor, quest giver, guard gossip, ...) or flagged
    // non-attackable are never pull targets, even with an empty target list.
    const auto npcFlags = field<std::uint32_t>(d, L.descUnitNpcFlags);
    u.isAttackable = npcFlags == 0 && (flags & (c::UnitFlagNonAttackable | c::UnitFlagImmuneToPc | c::UnitFlagNotSelectable)) == 0;
    u.isVendor = (npcFlags & L.npcFlagVendor) != 0;
    u.canRepair = (npcFlags & L.npcFlagRepair) != 0;
    u.isDead = u.health == 0 || (dyn & c::DynFlagDead) != 0;
    u.targetGuid = field<std::uint64_t>(d, L.descUnitTarget);
    u.ownerGuid = field<std::uint64_t>(d, L.descUnitSummonedBy);

    if (u.ownerGuid == 0)
    {
        u.ownerGuid = field<std::uint64_t>(d, L.descUnitCreatedBy);
    }

    std::array<float, 3> pos{};

    if (!memory_->read(obj.base + L.unitPosX, pos.data(), sizeof(pos)))
    {
        return false;
    }

    u.position = {pos[0], pos[1], pos[2]};
    u.heading = memory_->read<float>(obj.base + L.unitRotation).value_or(0.0f);
    const bool casting = L.unitCastingSpell && memory_->read<std::uint32_t>(obj.base + L.unitCastingSpell).value_or(0) != 0;
    const bool channelling = L.unitChannelSpell ? memory_->read<std::uint32_t>(obj.base + L.unitChannelSpell).value_or(0) != 0 : L.descUnitChannelSpell && field<std::uint32_t>(d, L.descUnitChannelSpell) != 0;
    u.isCasting = casting || channelling;

    if (!u.isPlayer)
    {
        auto it = nameCache_.find(u.guid);

        if (it == nameCache_.end())
        {
            it = nameCache_.emplace(u.guid, unitName(obj.base)).first;
        }

        u.name = it->second;
        // Newer clients give critters the level of the zone, but never more than a few hit points.
        u.isCritter = isCritterFaction(u.factionTemplate) || (u.maxHealth > 0 && u.maxHealth <= 10);
    }

    readAuras(obj, d, u.auras);
    return true;
}

void WowMemoryStateProvider::readPlayerExtras(const RawObject& obj, PlayerState& p) const
{
    const ClientLayout& L = *layout_;
    // Must reach the last player field read below; coinage is the furthest out.
    std::vector<std::uint8_t> buffer(std::max({L.descPlayerNextLevelXp, L.descPlayerFlags, L.descPlayerCoinage}) + 4);

    if (!memory_->read(obj.descriptors, buffer.data(), buffer.size()))
    {
        return;
    }

    const std::uint8_t* d = buffer.data();

    const auto bytes0 = field<std::uint32_t>(d, L.descUnitBytes0);
    p.playerClass = static_cast<PlayerClass>((bytes0 >> 8) & 0xFF);
    p.race = static_cast<int>(bytes0 & 0xFF);
    const auto slots = static_cast<std::uint32_t>(std::min<int>(L.powerSlots, static_cast<int>(kPowerTypeCount)));

    for (std::uint32_t i = 0; i < slots; ++i)
    {
        int value = static_cast<int>(field<std::uint32_t>(d, L.descUnitPower1 + i * 4));
        int maxValue = static_cast<int>(field<std::uint32_t>(d, L.descUnitMaxPower1 + i * 4));
        const auto type = static_cast<PowerType>(i);

        if (type == PowerType::Rage || type == PowerType::RunicPower)
        {
            value /= 10;
            maxValue /= 10;
        }

        p.setPower(type, value, maxValue);
    }

    const auto playerFlags = field<std::uint32_t>(d, L.descPlayerFlags);
    p.isGhost = (playerFlags & c::PlayerFlagGhost) != 0;

    if (p.isGhost)
    {
        p.isDead = false; // ghosts have 1 hp; isDead means "lying dead"
    }

    p.xp = static_cast<int>(field<std::uint32_t>(d, L.descPlayerXp));
    p.nextLevelXp = static_cast<int>(field<std::uint32_t>(d, L.descPlayerNextLevelXp));

    if (L.descPlayerCoinage)
    {
        p.money = static_cast<std::int64_t>(field<std::uint32_t>(d, L.descPlayerCoinage));
    }

    if (L.playerName)
    {
        p.name = memory_->readString(L.playerName, 48);
    }

    p.reaction = Reaction::Friendly;

    if (L.comboPoints)
    {
        p.comboPoints = memory_->read<std::uint8_t>(L.comboPoints).value_or(0);
    }

    if (L.unitAutoAttackTarget)
    {
        p.isAutoAttacking = memory_->read<std::uint64_t>(obj.base + L.unitAutoAttackTarget).value_or(0) != 0;
    }

    if (L.autoRepeatSpell)
    {
        p.isShooting = memory_->read<std::uint32_t>(L.autoRepeatSpell).value_or(0) != 0;
    }

    if ((p.isGhost || p.isDead) && L.corpsePosition)
    {
        std::array<float, 3> cp{};

        if (memory_->read(L.corpsePosition, cp.data(), sizeof(cp)) && (cp[0] != 0.0f || cp[1] != 0.0f))
        {
            p.corpsePosition = Vec3{cp[0], cp[1], cp[2]};
        }
    }

    p.knownSpells = readKnownSpells();

    if (L.descPlayerSkills)
    {
        std::array<std::uint32_t, 384> skillData{};

        if (memory_->read(obj.descriptors + L.descPlayerSkills, skillData.data(), sizeof(skillData)))
        {
            p.skills.clear();

            for (std::size_t i = 0; i < 128; ++i)
            {
                const std::uint32_t field0 = skillData[i * 3];
                const auto skillId = static_cast<std::uint16_t>(field0 & 0xFFFF);

                if (skillId == 0)
                {
                    continue;
                }

                const std::uint32_t field1 = skillData[i * 3 + 1];
                const auto rank = static_cast<std::uint16_t>(field1 & 0xFFFF);
                const auto maxRank = static_cast<std::uint16_t>((field1 >> 16) & 0xFFFF);
                const std::uint32_t field2 = skillData[i * 3 + 2];
                const auto tempBonus = static_cast<std::int16_t>(field2 & 0xFFFF);
                const auto permBonus = static_cast<std::int16_t>((field2 >> 16) & 0xFFFF);
                p.skills.push_back(PlayerSkill{skillId, rank, maxRank, tempBonus, permBonus});
            }
        }
    }
}

std::optional<Vec3> WowMemoryStateProvider::readCorpseObject(const std::vector<RawObject>& objects, Guid owner) const
{
    const ClientLayout& L = *layout_;

    if (L.descCorpsePosX == 0)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> d(std::max(L.descCorpseOwner + 8, L.descCorpsePosX + 12));

    for (const auto& obj : objects)
    {
        if (obj.type != c::TypeCorpse || !memory_->read(obj.descriptors, d.data(), d.size()))
        {
            continue;
        }

        if (field<std::uint64_t>(d.data(), L.descCorpseOwner) != owner)
        {
            continue;
        }

        return Vec3{field<float>(d.data(), L.descCorpsePosX), field<float>(d.data(), L.descCorpsePosX + 4), field<float>(d.data(), L.descCorpsePosX + 8)};
    }

    return std::nullopt;
}

void WowMemoryStateProvider::readBags(const std::vector<RawObject>& objects, const RawObject& self, PlayerState& p) const
{
    const ClientLayout& L = *layout_;
    std::unordered_map<Guid, const RawObject*> items;

    for (const auto& obj : objects)
    {
        if (obj.type == c::TypeItem || obj.type == c::TypeContainer)
        {
            items.emplace(obj.guid, &obj);
        }
    }

    if (L.descPlayerInvSlot1 != 0 && L.descItemMaxDurability != 0)
    {
        std::array<std::uint64_t, 19> equipped{};

        if (memory_->read(self.descriptors + L.descPlayerInvSlot1, equipped.data(), sizeof(equipped)))
        {
            int broken = 0;
            int damaged = 0;

            for (const Guid guid : equipped)
            {
                const auto it = guid != 0 ? items.find(guid) : items.end();

                if (it == items.end())
                {
                    continue;
                }

                const auto max = memory_->read<std::uint32_t>(it->second->descriptors + L.descItemMaxDurability);
                const auto cur = memory_->read<std::uint32_t>(it->second->descriptors + L.descItemDurability);

                if (!max || !cur || *max == 0)
                {
                    continue;
                }

                if (*cur == 0)
                {
                    ++broken;
                }

                if (durabilityLow(*cur, *max))
                {
                    ++damaged;
                }
            }

            p.brokenItems = broken;
            p.damagedItems = damaged;
        }
    }

    std::array<std::uint64_t, 16> backpack{};
    std::array<std::uint64_t, 4> bags{};

    if (!memory_->read(self.descriptors + L.descPlayerPackSlot1, backpack.data(), sizeof(backpack)) || !memory_->read(self.descriptors + L.descPlayerBagSlot1, bags.data(), sizeof(bags)))
    {
        return;
    }

    std::map<std::uint32_t, int> counts;
    int freeSlots = 0;
    bool special = false; // counting a herb bag, quiver, ...: its empty slots take no loot
    std::vector<std::uint8_t> itemFields(std::max(L.descItemStackCount, L.descEntry) + 4);
    auto countSlot = [&](Guid guid)
    {
        if (guid == 0)
        {
            if (!special)
            {
                ++freeSlots;
            }

            return;
        }

        const auto it = items.find(guid);

        if (it == items.end())
        {
            return;
        }

        if (!memory_->read(it->second->descriptors, itemFields.data(), itemFields.size()))
        {
            return;
        }

        const auto stack = field<std::uint32_t>(itemFields.data(), L.descItemStackCount);
        counts[field<std::uint32_t>(itemFields.data(), L.descEntry)] += static_cast<int>(std::max<std::uint32_t>(stack, 1));
    };

    for (const Guid slot : backpack)
    {
        countSlot(slot);
    }

    std::vector<std::uint8_t> container(L.descContainerSlot1 + 36 * 8);

    for (const Guid bag : bags)
    {
        const auto it = bag != 0 ? items.find(bag) : items.end();

        if (it == items.end() || it->second->type != c::TypeContainer)
        {
            continue;
        }

        if (!memory_->read(it->second->descriptors, container.data(), container.size()))
        {
            continue;
        }

        const auto slots = std::min<std::uint32_t>(field<std::uint32_t>(container.data(), L.descContainerNumSlots), 36);
        special = isSpecialBag(field<std::uint32_t>(container.data(), L.descEntry));

        for (std::uint32_t i = 0; i < slots; ++i)
        {
            countSlot(field<std::uint64_t>(container.data(), L.descContainerSlot1 + i * 8));
        }
    }

    p.freeBagSlots = freeSlots;
    p.bagItems.clear();

    for (const auto& [id, count] : counts)
    {
        ItemStack item;
        item.itemId = id;
        item.count = count;

        if (const std::string* name = itemName(id))
        {
            item.name = *name;
        }

        p.bagItems.push_back(std::move(item));
    }
}

bool WowMemoryStateProvider::readGameObject(const RawObject& obj, GameObjectState& g) const
{
    const ClientLayout& L = *layout_;
    std::vector<std::uint8_t> buffer(std::max({L.descGameObjectType + 4, L.descGameObjectCreatedBy + 8, L.descGameObjectPosX + 12, L.descEntry + 4}));

    if (!memory_->read(obj.descriptors, buffer.data(), buffer.size()))
    {
        return false;
    }

    const std::uint8_t* d = buffer.data();
    g.guid = obj.guid;
    g.entry = field<std::uint32_t>(d, L.descEntry);
    g.createdBy = field<std::uint64_t>(d, L.descGameObjectCreatedBy);
    const auto type = static_cast<std::uint8_t>((field<std::uint32_t>(d, L.descGameObjectType) >> L.gameObjectTypeShift) & 0xFF);

    if (L.gameObjectPosX)
    {
        std::array<float, 3> pos{};

        if (!memory_->read(obj.base + L.gameObjectPosX, pos.data(), sizeof(pos)))
        {
            return false;
        }

        g.position = {pos[0], pos[1], pos[2]};
    }
    else
    {
        g.position = {field<float>(d, L.descGameObjectPosX), field<float>(d, L.descGameObjectPosX + 4), field<float>(d, L.descGameObjectPosX + 8)};
    }

    auto it = nameCache_.find(g.guid);

    if (it == nameCache_.end())
    {
        std::string name;

        if (L.gameObjectNamePtr1)
        {
            if (auto p1 = memory_->readPointer(obj.base + L.gameObjectNamePtr1); p1 && *p1)
            {
                if (auto p2 = memory_->readPointer(*p1 + L.gameObjectNamePtr2); p2 && *p2)
                {
                    name = memory_->readString(*p2);
                }
            }
        }

        it = nameCache_.emplace(g.guid, std::move(name)).first;
    }

    g.name = it->second;
    g.kind = classifyGameObject(type, g.name);

    if (g.kind == GameObjectKind::FishingBobber && L.gameObjectBobbing)
    {
        g.isBobbing = memory_->read<std::uint8_t>(obj.base + L.gameObjectBobbing).value_or(0) == 1;
    }

    return true;
}

// The 64-bit client has no spell error or bag reading yet: only what its
// reader fills in, the chat window's lines, and movement, which the provider
// tracks itself.
std::optional<GameSnapshot> WowMemoryStateProvider::captureModern(TimePoint now)
{
    auto s = modern_->capture(now, config_.visibilityRange);

    if (!s && switchToClientInWorld(now))
    {
        return std::nullopt;
    }

    if (!s)
    {
        if (!loggedNotInGame_)
        {
            log_.info(logcat::Game, "WoW is not in the world (login/loading screen)");
        }

        loggedNotInGame_ = true;
        return std::nullopt;
    }

    loggedNotInGame_ = false;
    const float dt = (lastPlayerPositionAt_ && now > *lastPlayerPositionAt_) ? static_cast<float>(toSeconds(now - *lastPlayerPositionAt_)) : 0.0f;
    const float dist = lastPlayerPosition_ ? distance2D(*lastPlayerPosition_, s->player.position) : 0.0f;
    s->player.speed = (dt > 0.001f) ? (dist / dt) : 0.0f;
    if (dist <= 0.01f || s->player.speed < 0.05f)
    {
        s->player.speed = 0.0f;
    }
    s->player.isMoving = (s->player.speed > 0.1f) || (lastPlayerPosition_ && dist > 0.05f);
    lastPlayerPosition_ = s->player.position;
    lastPlayerPositionAt_ = now;
    summarizeFishing(*s, layout_->modern && layout_->modern->gameObjectBobbing != 0);
    // Another character (logged in on this client, or another client
    // switched to): not its counts.
    if (s->player.guid != objectiveWantedOf_)
    {
        objectiveWanted_.clear();
        trackedQuests_.clear();
        objectiveWantedOf_ = s->player.guid;
        // Nor its chat: what stands in the frame is read as old, not as said now.
        chatFrameReadAt_.reset();
        chatSeen_.clear();
        chatSeenOrder_.clear();
    }

    readChatFrame(now);
    s->player.objectiveWanted = objectiveWanted_;

    if (supportsUi())
    {
        s->player.breathDraining = breathShown_;
    }
    s->player.trackedQuests = trackedQuests_;
    s->zoneText = zoneText_;
    return s;
}

void WowMemoryStateProvider::readChatFrame(TimePoint now)
{
    // Once a second: a line stays shown for a couple of minutes.
    constexpr Duration kEvery{1000};

    if (chatFrameReadAt_ && now - *chatFrameReadAt_ < kEvery)
    {
        return;
    }

    const bool first = !chatFrameReadAt_;
    chatFrameReadAt_ = now;
    const auto texts = modern_->shownTexts();
    // The breath bar (MirrorTimer1) is shown only while the breath runs out.
    breathShown_ = std::any_of(texts.begin(), texts.end(), [](const PlacedText& t) { return t.text == "Breath"; });
    zoneText_ = modern_->minimapZoneText().value_or("");

    // The quest objectives' counts the tracker and the progress messages
    // show; kept for the session, as a quest's lines leave the tracker.
    // The tracker's quests, as shown now: a quest no longer in it keeps its
    // last lines (it may only have been untracked).
    for (auto& [title, lines] : trackedQuests_)
    {
        for (auto& line : lines)
        {
            line.shown = false;
        }
    }

    for (auto& [title, lines] : trackedQuests(texts))
    {
        auto& kept = trackedQuests_[title];
        kept.clear();

        for (auto& line : lines)
        {
            kept.push_back({std::move(line.name), line.have, line.want});
        }
    }

    for (const auto& [name, want] : objectiveCountsIn(texts))
    {
        if (want <= 0)
        {
            objectiveWanted_.erase(name);
            continue;
        }

        if (auto [at, added] = objectiveWanted_.try_emplace(name, want); added || at->second != want)
        {
            at->second = want;
            log_.debug(logcat::Game, "the game asks for {} of {}", want, name);
        }
    }

    // A line never seen before is new. Each has its own number in the
    // player's link (|Hplayer:Name:3243:SAY:), so one said twice is two
    // lines - and one shown again (faded lines come back under the mouse, or
    // scrolling) or in two chat windows is one. Those seen at the first read
    // were said before the bot came.
    constexpr std::size_t kRemembered = 4000;

    for (const auto& [line, bottom, left] : texts)
    {
        if (line.find("|Hplayer:") == std::string::npos || !chatSeen_.insert(line).second)
        {
            continue;
        }

        chatSeenOrder_.push_back(line);

        if (chatSeenOrder_.size() > kRemembered)
        {
            chatSeen_.erase(chatSeenOrder_.front());
            chatSeenOrder_.pop_front();
        }

        const auto parsed = first ? std::nullopt : parseChatFrameLine(line);

        if (!parsed)
        {
            continue;
        }

        log_.info(logcat::Chat, "[{}] {}: {}", parsed->channel, parsed->sender, parsed->text);

        if (parsed->channel == "Party" || parsed->channel == "Whisper")
        {
            chatIn_.push_back({parsed->channel, parsed->sender, parsed->text});
        }
    }
}

bool WowMemoryStateProvider::switchToClientInWorld(TimePoint now)
{
    // Only when the client was picked automatically and another one runs.
    if (config_.pid > 0 || now - lastAttachAttempt_ < config_.reattachInterval)
    {
        return false;
    }

    lastAttachAttempt_ = now;

    if (findClientProcesses().size() < 2)
    {
        return false;
    }

    const int before = memory_->pid();
    memory_->detach();
    attach();

    if (memory_->pid() == before)
    {
        return false;
    }

    log_.info(logcat::Game, "WoW process {} is not in the world - switched to process {}", before, memory_->pid());
    return true;
}

std::optional<GameSnapshot> WowMemoryStateProvider::capture()
{
    const TimePoint now = clock_.now();

    if (!memory_->attached() || !memory_->isAlive())
    {
        memory_->detach();
        chatHookInstalled_ = false;
        chatCVarAddress_ = 0;
        lastChatSeq_ = 0;
        lastRawChatLog_.clear();
        chatFrameReadAt_.reset();
        chatSeen_.clear();
        chatSeenOrder_.clear();
        objectiveWanted_.clear();
        trackedQuests_.clear();

        if (now - lastAttachAttempt_ < config_.reattachInterval)
        {
            return std::nullopt;
        }

        lastAttachAttempt_ = now;

        if (!attach())
        {
            return std::nullopt;
        }
    }

    if (modern_)
    {
        return captureModern(now);
    }

    if (!isInGame() && switchToClientInWorld(now))
    {
        return std::nullopt;
    }

    if (!isInGame())
    {
        if (!loggedNotInGame_)
        {
            log_.info(logcat::Game, "WoW is not in the world (login/loading screen)");
        }

        loggedNotInGame_ = true;
        nameCache_.clear();
        chatPosition_.reset(); // do not replay the old buffer after the next login
        lootHookInstalled_ = false;
        chatHookInstalled_ = false;
        chatCVarAddress_ = 0;
        lastChatSeq_ = 0;
        lastRawChatLog_.clear();
        spellErrorHookInstalled_ = false;
        spellErrorCVarAddress_ = 0;
        lastSpellErrorCount_.reset();
        lastOutOfReachCount_.reset();
        return std::nullopt;
    }

    loggedNotInGame_ = false;
    readChat();

    const ClientLayout& L = *layout_;
    Guid localGuid = 0;
    const auto objects = enumerateObjects(localGuid);

    if (objects.empty() || localGuid == 0)
    {
        return std::nullopt;
    }

    GameSnapshot s;
    s.timestamp = now;

    if (L.zoneId)
    {
        s.zoneId = memory_->read<std::uint32_t>(L.zoneId).value_or(0);
        s.mapId = mapForZone(s.zoneId);

        if (!s.mapId)
        {
            s.mapId = mapForZone(canonicalZone(s.zoneId));
        }
    }

    const auto self = std::find_if(objects.begin(), objects.end(), [&](const RawObject& r) { return r.guid == localGuid; });

    if (self == objects.end() || !readUnit(*self, s.player))
    {
        return std::nullopt;
    }

    readPlayerExtras(*self, s.player);
    readBags(objects, *self, s.player);

    if ((s.player.isGhost || s.player.isDead) && !s.player.corpsePosition)
    {
        s.player.corpsePosition = readCorpseObject(objects, localGuid);
    }

    s.targetGuid = L.targetGuid ? memory_->read<std::uint64_t>(L.targetGuid).value_or(0) : s.player.targetGuid;

    for (const auto& obj : objects)
    {
        if (obj.guid == localGuid)
        {
            continue;
        }

        if (obj.type == c::TypeUnit || obj.type == c::TypePlayer)
        {
            UnitState u;

            if (readUnit(obj, u) && distance(u.position, s.player.position) <= config_.visibilityRange)
            {
                s.units.push_back(std::move(u));
            }
        }
        else if (obj.type == c::TypeGameObject)
        {
            GameObjectState g;

            if (readGameObject(obj, g) && (distance(g.position, s.player.position) <= config_.visibilityRange || keptAtAnyDistance(g)))
            {
                s.objects.push_back(std::move(g));
            }
        }
    }

    if (s.player.factionOverride == Faction::Unknown && s.player.factionTemplate != 0)
    {
        if (const auto* ft = getFactionTemplate(s.player.factionTemplate))
        {
            s.player.factionOverride = ft->side();
        }
    }

    for (auto& u : s.units)
    {
        if (u.inCombat && u.targetGuid == localGuid)
        {
            u.reaction = Reaction::Hostile;
        }
        else if (s.player.factionTemplate != 0 && u.factionTemplate != 0)
        {
            u.reaction = calculateReaction(s.player.factionTemplate, u.factionTemplate);
        }
    }

    if (s.player.faction() == Faction::Unknown)
    {
        for (const auto& u : s.units)
        {
            Faction uFact = Faction::Unknown;

            if (u.factionTemplate != 0)
            {
                if (const auto* ft = getFactionTemplate(u.factionTemplate))
                {
                    uFact = ft->side();
                }

                if (uFact == Faction::Unknown)
                {
                    uFact = factionOfTemplateId(u.factionTemplate);
                }
            }

            if (uFact == Faction::Unknown && u.isPlayer)
            {
                uFact = factionOfRace(u.race);
            }

            if (uFact != Faction::Unknown)
            {
                if (u.reaction == Reaction::Friendly)
                {
                    s.player.factionOverride = uFact;
                    break;
                }
                else if (u.reaction == Reaction::Hostile)
                {
                    s.player.factionOverride = (uFact == Faction::Alliance ? Faction::Horde : Faction::Alliance);
                    break;
                }
            }
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
            if (const auto* ft = getFactionTemplate(u.factionTemplate))
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

    const float dt = (lastPlayerPositionAt_ && now > *lastPlayerPositionAt_) ? static_cast<float>(toSeconds(now - *lastPlayerPositionAt_)) : 0.0f;
    const float dist = lastPlayerPosition_ ? distance2D(*lastPlayerPosition_, s.player.position) : 0.0f;
    s.player.speed = (dt > 0.001f) ? (dist / dt) : 0.0f;
    if (dist <= 0.01f || s.player.speed < 0.05f)
    {
        s.player.speed = 0.0f;
    }
    s.player.isMoving = (s.player.speed > 0.1f) || (lastPlayerPosition_ && dist > 0.05f);
    lastPlayerPosition_ = s.player.position;
    lastPlayerPositionAt_ = now;

    if (nameCache_.size() > 5000)
    {
        nameCache_.clear();
    }

    readSpellErrors(s);
    summarizeFishing(s, L.gameObjectBobbing != 0);
    return s;
}

std::optional<WowMemoryStateProvider::RawObjectInfo> WowMemoryStateProvider::rawObject(Guid guid) const
{
    if (!memory_->attached())
    {
        return std::nullopt;
    }

    RawObjectInfo info;

    if (modern_)
    {
        const auto found = modern_->findObject(guid);

        if (!found)
        {
            return std::nullopt;
        }

        info.base = found->first;
        info.guid.resize(16);
        std::memcpy(info.guid.data(), &found->second.low, 8);
        std::memcpy(info.guid.data() + 8, &found->second.high, 8);
        return info;
    }

    Guid localGuid = 0;

    for (const auto& obj : enumerateObjects(localGuid))
    {
        if (obj.guid != guid)
        {
            continue;
        }

        info.base = obj.base;
        info.guid.resize(8);
        std::memcpy(info.guid.data(), &obj.guid, 8);
        return info;
    }

    return std::nullopt;
}

std::vector<std::uint8_t> WowMemoryStateProvider::readRaw(std::uint64_t address, std::size_t size) const
{
    std::vector<std::uint8_t> out(size);

    if (!memory_->attached() || !memory_->read(address, out.data(), size))
    {
        out.clear();
    }

    return out;
}

void WowMemoryStateProvider::summarizeFishing(GameSnapshot& s, bool biteKnown)
{
    s.fishing.biteKnown = biteKnown;

    for (const auto& g : s.objects)
    {
        // Ours, preferring one that bites: an old one lingers after its catch.
        if (g.kind == GameObjectKind::FishingBobber && g.createdBy != 0 && g.createdBy == s.player.guid && !(s.fishing.hasBobber && s.fishing.biteDetected))
        {
            s.fishing.hasBobber = true;
            s.fishing.bobberGuid = g.guid;
            s.fishing.biteDetected = g.isBobbing;
        }
    }
}

std::uint32_t WowMemoryStateProvider::findCVar(std::string_view name) const
{
    if (!layout_->cvarList || !memory_->attached())
    {
        return 0;
    }

    const auto head = memory_->read<std::uint32_t>(layout_->cvarList);

    if (!head)
    {
        return 0;
    }

    std::uint32_t curr = *head;
    std::size_t visited = 0;

    while (curr >= 0x01000000 && curr <= 0x7FFFFFFF && visited++ < 1000)
    {
        const std::uint32_t cvarBase = curr - 0x0C;
        const auto namePtr = memory_->read<std::uint32_t>(cvarBase + 0x14);

        if (namePtr && *namePtr >= 0x00400000 && *namePtr <= 0x7FFFFFFF)
        {
            const std::string cvarName = memory_->readString(*namePtr, 64);

            if (cvarName == name)
            {
                return cvarBase;
            }
        }

        const auto next = memory_->read<std::uint32_t>(cvarBase + 0x0C);

        if (!next || *next == curr)
        {
            break;
        }

        curr = *next;
    }

    return 0;
}

std::optional<std::string> WowMemoryStateProvider::readCVar(std::string_view name, std::uint32_t& address) const
{
    if (address != 0)
    {
        const auto namePtr = memory_->read<std::uint32_t>(address + 0x14);

        if (!namePtr || memory_->readString(*namePtr, 64) != name)
        {
            address = 0;
        }
    }

    if (address == 0)
    {
        address = findCVar(name);
    }

    if (address == 0)
    {
        return std::nullopt;
    }

    const auto valPtr = memory_->read<std::uint32_t>(address + 0x28);

    if (!valPtr || !*valPtr)
    {
        return std::nullopt;
    }

    return memory_->readString(*valPtr, 4096);
}

// UI_ERROR_MESSAGE carries the localized text, so it is compared against the
// client's own ERR_* strings rather than English.
void WowMemoryStateProvider::ensureSpellErrorHook()
{
    if (!supportsLua() || !memory_->attached() || spellErrorHookInstalled_ || !luaIdle())
    {
        return;
    }

    if (!memory_->readPointer(layout_->d3dDevicePointer).value_or(0))
    {
        return;
    }

    // "N R M": not-ready refusals, out-of-range / line-of-sight ones, and moving refusals.
    // Earlier versions only counted N or N R; upgrade if an existing session carries it.
    if (executeLua("if _OG_SE and not _OG_SE2 then _OG_SE:UnregisterAllEvents() end "
                   "if not _OG_SE2 or not _OG_SE2_M then "
                   "if not _OG_SE2 then _OG_SE2=CreateFrame('Frame') end "
                   "_OG_SE2_N=_OG_SE2_N or 0 "
                   "_OG_SE2_R=_OG_SE2_R or 0 "
                   "_OG_SE2_M=0 "
                   "RegisterCVar('og_spell_errors2', '0 0 0') "
                   "_OG_SE2:RegisterEvent('UI_ERROR_MESSAGE') "
                   "_OG_SE2:SetScript('OnEvent', function(self, event, a1, a2) "
                   "local msg = type(a1)=='string' and a1 or (type(a2)=='string' and a2 or '') "
                   "if msg==ERR_SPELL_COOLDOWN or msg==ERR_ABILITY_COOLDOWN or msg==SPELL_FAILED_NOT_READY then "
                   "_OG_SE2_N=_OG_SE2_N+1 "
                   "elseif msg==ERR_OUT_OF_RANGE or msg==SPELL_FAILED_OUT_OF_RANGE or msg==SPELL_FAILED_LINE_OF_SIGHT then "
                   "_OG_SE2_R=_OG_SE2_R+1 "
                   "elseif msg==ERR_SPELL_FAILED_MOVING or msg==SPELL_FAILED_MOVING or msg==[[Can't do that while moving]] then "
                   "_OG_SE2_M=_OG_SE2_M+1 "
                   "else return end "
                   "SetCVar('og_spell_errors2', _OG_SE2_N..' '.._OG_SE2_R..' '.._OG_SE2_M) "
                   "end) "
                   "end"))
    {
        spellErrorHookInstalled_ = true;
    }
}

void WowMemoryStateProvider::readSpellErrors(GameSnapshot& s)
{
    if (!supportsLua() || layout_->cvarList == 0)
    {
        return;
    }

    ensureSpellErrorHook();
    const auto raw = readCVar("og_spell_errors2", spellErrorCVarAddress_);

    if (!raw)
    {
        return;
    }

    std::istringstream in(*raw);
    long notReady = 0;
    long outOfReach = 0;
    long moving = 0;

    if (!(in >> notReady))
    {
        return;
    }

    in >> outOfReach;
    in >> moving;
    // The first reading is the baseline; a lower one means a /reload reset the counter.
    const auto added = [](long count, std::optional<long>& last)
    {
        const long n = !last ? 0 : count >= *last ? count - *last : count;
        last = count;
        return static_cast<int>(n);
    };
    s.spellNotReadyErrors = added(notReady, lastSpellErrorCount_);
    s.spellOutOfReachErrors = added(outOfReach, lastOutOfReachCount_);
    s.spellMovingErrors = added(moving, lastMovingCount_);
}

void WowMemoryStateProvider::ensureChatHook()
{
    if (!supportsLua() || !memory_->attached() || !isInGame() || chatHookInstalled_)
    {
        return;
    }

    if (!memory_->readPointer(layout_->d3dDevicePointer).value_or(0))
    {
        return;
    }

    const char* script = "if not _OG_CH then "
                         "_OG_CH=CreateFrame('Frame') "
                         "_OG_CH_SEQ=0 "
                         "_OG_CH_BUF={} "
                         "RegisterCVar('og_chat_log', '') "
                         "local evs={'CHAT_MSG_WHISPER','CHAT_MSG_WHISPER_INFORM','CHAT_MSG_BN_WHISPER','CHAT_MSG_BN_WHISPER_INFORM','CHAT_MSG_MONSTER_WHISPER','CHAT_MSG_SAY','CHAT_MSG_YELL','CHAT_MSG_PARTY','CHAT_MSG_PARTY_LEADER','CHAT_MSG_GUILD','CHAT_MSG_OFFICER','CHAT_MSG_RAID','CHAT_MSG_RAID_LEADER','CHAT_MSG_RAID_WARNING','CHAT_MSG_EMOTE'} "
                         "for _,e in ipairs(evs) do _OG_CH:RegisterEvent(e) end "
                         "_OG_CH:SetScript('OnEvent', function(self, event, msg, sender) "
                         "_OG_CH_SEQ=_OG_CH_SEQ+1 "
                         "local m=string.gsub(string.gsub(tostring(msg or ''), string.char(9), ' '), string.char(10), ' ') "
                         "local s=string.gsub(string.gsub(tostring(sender or ''), string.char(9), ' '), string.char(10), ' ') "
                         "table.insert(_OG_CH_BUF, tostring(_OG_CH_SEQ)..string.char(9)..tostring(event)..string.char(9)..s..string.char(9)..m) "
                         "while #_OG_CH_BUF>20 do table.remove(_OG_CH_BUF, 1) end "
                         "SetCVar('og_chat_log', table.concat(_OG_CH_BUF, string.char(10))) "
                         "end) "
                         "end";

    if (executeLua(script))
    {
        chatHookInstalled_ = true;
    }
}

void WowMemoryStateProvider::readChat()
{
    const ClientLayout& L = *layout_;

    if (L.chatBuffer != 0 && L.chatMessageCount > 0 && L.chatPosition != 0)
    {
        const auto position = memory_->read<std::int32_t>(L.chatPosition);

        if (position && position != chatPosition_)
        {
            const bool first = !chatPosition_.has_value();
            chatPosition_ = position;

            const auto count = static_cast<std::size_t>(L.chatMessageCount);
            chatEntries_.resize(count);
            std::vector<char> buffer(1024);
            const std::size_t newest = static_cast<std::size_t>(std::max(0, *position)) % count;

            for (std::size_t k = 1; k <= count; ++k)
            {
                const std::size_t slot = (newest + k) % count;
                const std::uint32_t address = L.chatBuffer + static_cast<std::uint32_t>(slot) * L.chatMessageStride + L.chatTextOffset;

                if (!memory_->read(address, buffer.data(), buffer.size() - 1))
                {
                    continue;
                }

                buffer.back() = '\0';
                std::string entry(buffer.data());

                if (entry == chatEntries_[slot])
                {
                    continue;
                }

                chatEntries_[slot] = entry;

                if (first)
                {
                    continue; // lines from before we attached
                }

                if (const auto line = parseChatEntry(entry))
                {
                    if (const std::string text = formatChatLine(*line); !text.empty())
                    {
                        log_.info(logcat::Chat, "{}", text);
                    }

                    // Party chat for the Party system (CHAT_MSG_PARTY).
                    if (line->type == 0x02)
                    {
                        chatIn_.push_back({"Party", line->sender, line->text});
                    }
                }
            }
        }
    }

    if (supportsLua() && L.cvarList != 0)
    {
        ensureChatHook();

        if (chatCVarAddress_ == 0)
        {
            chatCVarAddress_ = findCVar("og_chat_log");
        }
        else
        {
            const auto namePtr = memory_->read<std::uint32_t>(chatCVarAddress_ + 0x14);

            if (!namePtr || memory_->readString(*namePtr, 16) != "og_chat_log")
            {
                chatCVarAddress_ = findCVar("og_chat_log");
            }
        }

        if (chatCVarAddress_ != 0)
        {
            const auto valPtr = memory_->read<std::uint32_t>(chatCVarAddress_ + 0x28);

            if (valPtr && *valPtr)
            {
                const std::string raw = memory_->readString(*valPtr, 4096);

                if (!raw.empty() && raw != lastRawChatLog_)
                {
                    lastRawChatLog_ = raw;
                    std::stringstream ss(raw);
                    std::string line;

                    while (std::getline(ss, line))
                    {
                        if (line.empty())
                        {
                            continue;
                        }

                        std::stringstream ls(line);
                        std::string seqStr, event, sender, msg;

                        if (!std::getline(ls, seqStr, '\t') || !std::getline(ls, event, '\t') || !std::getline(ls, sender, '\t') || !std::getline(ls, msg, '\t'))
                        {
                            continue;
                        }

                        int seq = 0;

                        try
                        {
                            seq = std::stoi(seqStr);
                        }
                        catch (...)
                        {
                            continue;
                        }

                        if (lastChatSeq_ == 0)
                        {
                            if (seq > lastChatSeq_)
                            {
                                lastChatSeq_ = seq;
                            }

                            continue;
                        }

                        if (seq > lastChatSeq_)
                        {
                            lastChatSeq_ = seq;
                            std::string label;

                            if (event == "CHAT_MSG_WHISPER" || event == "CHAT_MSG_BN_WHISPER" || event == "CHAT_MSG_MONSTER_WHISPER")
                            {
                                label = "Whisper";
                            }
                            else if (event == "CHAT_MSG_WHISPER_INFORM" || event == "CHAT_MSG_BN_WHISPER_INFORM")
                            {
                                label = "To";
                            }
                            else if (event == "CHAT_MSG_SAY")
                            {
                                label = "Say";
                            }
                            else if (event == "CHAT_MSG_YELL")
                            {
                                label = "Yell";
                            }
                            else if (event == "CHAT_MSG_PARTY" || event == "CHAT_MSG_PARTY_LEADER")
                            {
                                label = "Party";
                            }
                            else if (event == "CHAT_MSG_RAID" || event == "CHAT_MSG_RAID_LEADER" || event == "CHAT_MSG_RAID_WARNING")
                            {
                                label = "Raid";
                            }
                            else if (event == "CHAT_MSG_GUILD")
                            {
                                label = "Guild";
                            }
                            else if (event == "CHAT_MSG_OFFICER")
                            {
                                label = "Officer";
                            }
                            else if (event == "CHAT_MSG_EMOTE")
                            {
                                label = "Emote";
                            }
                            else
                            {
                                continue;
                            }

                            std::string formatted;

                            if (label == "Emote")
                            {
                                formatted = std::format("[{}] {} {}", label, sender, msg);
                            }
                            else if (!sender.empty())
                            {
                                formatted = std::format("[{}] {}: {}", label, sender, msg);
                            }
                            else
                            {
                                formatted = std::format("[{}] {}", label, msg);
                            }

                            log_.info(logcat::Chat, "{}", formatted);

                            if (event == "CHAT_MSG_PARTY" || event == "CHAT_MSG_PARTY_LEADER")
                            {
                                chatIn_.push_back({"Party", sender, msg});
                            }
                        }
                    }
                }
            }
        }
    }
}

std::vector<std::uint32_t> WowMemoryStateProvider::readKnownSpells() const
{
    std::vector<std::uint32_t> out;

    if (!memory_->attached() || layout_->spellBook == 0)
    {
        return out;
    }

    std::array<std::uint32_t, 512> buf{};

    if (memory_->read(layout_->spellBook, buf.data(), sizeof(buf)))
    {
        for (std::uint32_t id : buf)
        {
            if (id == 0)
            {
                break;
            }

            out.push_back(id);
        }
    }

    return out;
}

std::vector<std::uint32_t> WowMemoryStateProvider::readActionBarSlots() const
{
    std::vector<std::uint32_t> out(120, 0);

    if (!memory_->attached() || layout_->actionBarFirstSlot == 0)
    {
        return out;
    }

    memory_->read(layout_->actionBarFirstSlot, out.data(), sizeof(std::uint32_t) * out.size());
    return out;
}

namespace
{

// Scans every action bar and the spellbook into CVars the bot reads back:
//   og_ab1..8  one line per filled slot: slot, type, id, name, key, cast ms, min range, max range
//   og_sb1..8  one line per spellbook entry: id, name
//   og_ab_seq  "<scan count> <1 when Lua runs untainted>"
// It only scans when asked, which is when the bot starts; an earlier version
// rescanned on every bar and spellbook event (casting fires them), and is
// switched off here. Each bar names its binding command; the number is the
// bar's fixed first slot, for buttons that never ran ActionButton_UpdateAction.
constexpr std::string_view kActionBarHook = R"LUA(if not _OG_AB then
_OG_AB=CreateFrame('Frame')
_OG_AB.seq=0
RegisterCVar('og_ab_seq','')
for i=1,8 do RegisterCVar('og_ab'..i,'') RegisterCVar('og_sb'..i,'') end
local function W(p,t)
local pg,n={''},1
for _,l in ipairs(t) do if #pg[n]+#l>1500 then n=n+1 pg[n]='' end pg[n]=pg[n]..l end
for i=1,8 do SetCVar(p..i,pg[i] or '') end
end
local B={{'ActionButton','ACTIONBUTTON'},{'MultiBarBottomLeftButton','MULTIACTIONBAR1BUTTON',60},{'MultiBarBottomRightButton','MULTIACTIONBAR2BUTTON',48},{'MultiBarRightButton','MULTIACTIONBAR3BUTTON',24},{'MultiBarLeftButton','MULTIACTIONBAR4BUTTON',36}}
function _OG_AB.scan()
local k,t={},{}
for _,m in ipairs(B) do for i=1,12 do
local b=_G[m[1]..i]
local s=(b and b.action) or (m[3] and m[3]+i)
local key=GetBindingKey(m[2]..i)
if s and key and not k[s] then k[s]=key end
end end
for s=1,120 do
local ty,id,_,g=GetActionInfo(s)
local nm,c,lo,hi
if ty=='spell' then id=g or id nm,_,_,_,_,_,c,lo,hi=GetSpellInfo(id)
elseif ty=='item' then nm=GetItemInfo(id) else ty=nil end
if ty then t[#t+1]=s..'\t'..ty..'\t'..id..'\t'..(nm or '')..'\t'..(k[s] or '')..'\t'..(c or 0)..'\t'..(lo or 0)..'\t'..(hi or 0)..'\n' end
end
W('og_ab',t)
t={}
local i=1
while GetSpellName(i,'spell') do
local l=GetSpellLink(i,'spell')
t[#t+1]=((l and l:match('spell:(%d+)')) or 0)..'\t'..GetSpellName(i,'spell')..'\n'
i=i+1
end
W('og_sb',t)
_OG_AB.seq=_OG_AB.seq+1
SetCVar('og_ab_seq',_OG_AB.seq..' '.._OG_AB.sec)
end
end
_OG_AB:UnregisterAllEvents()
_OG_AB:SetScript('OnUpdate',nil)
_OG_AB.sec=issecure() and 1 or 0
_OG_AB.scan())LUA";

std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> out;

    while (true)
    {
        const auto at = text.find(separator);
        out.push_back(text.substr(0, at));

        if (at == std::string_view::npos)
        {
            return out;
        }

        text.remove_prefix(at + 1);
    }
}

template <class T> T number(std::string_view text)
{
    T value{};
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

} // namespace

bool WowMemoryStateProvider::requestActionBarScan()
{
    if (!supportsLua() || layout_->cvarList == 0 || !memory_->attached() || !isInGame() || !luaIdle())
    {
        return false;
    }

    return executeLua(kActionBarHook);
}

std::filesystem::path WowMemoryStateProvider::installDirectory() const
{
    if (!config_.installDirectory.empty())
    {
        return config_.installDirectory;
    }
#ifndef _WIN32
    // Clients run from their install directory, under wine too.
    std::error_code ec;
    auto cwd = std::filesystem::read_symlink(std::format("/proc/{}/cwd", memory_->pid()), ec);

    if (!ec)
    {
        return cwd;
    }
#endif
    return {};
}

std::vector<ActionBarSlot> WowMemoryStateProvider::readModernActionBars()
{
    std::vector<ActionBarSlot> bars;
    const auto snapshot = modern_->capture(clock_.now(), config_.visibilityRange);

    if (!snapshot)
    {
        return bars;
    }

    // Bindings are saved per account, and per character when it has its own.
    const auto install = installDirectory();
    const std::string realm = modern_->realmName();
    WowBindings bindings;
    const auto files = bindingFilesFor(install, realm, snapshot->player.name);

    for (const auto& file : files)
    {
        bindings.applyFile(file);
    }

    // Where the UI can be clicked (1.60.1), a slot without a known key is
    // clicked on its button, so the missing file is only worth a debug line.
    const bool clicksButtons = supportsUi();

    if (files.empty() && !warnedNoBindingFiles_)
    {
        warnedNoBindingFiles_ = true;
        log_.log(clicksButtons ? LogLevel::Debug : LogLevel::Warning, logcat::Action, std::format("no bindings-cache.wtf for '{}' under {} - only the main bar's default keys are known", snapshot->player.name, (install / "WTF" / "Account").string()));
    }

    const bool addon = bindingsaddon::usable(install, realm, snapshot->player.name);

    if (bindingsAddonUsable_.exchange(addon) != addon || !addonReported_)
    {
        addonReported_ = true;

        if (addon)
        {
            log_.info(logcat::Action, "the {} addon gives action bar slots without a key one of their own", bindingsaddon::Name);
        }
        else if (!clicksButtons)
        {
            log_.info(logcat::Action, "the {} addon is not in {} - action bar slots without a key cannot be pressed", bindingsaddon::Name, (install / "Interface" / "AddOns").string());
        }
    }

    // Looting on this client is a keypress on the corpse: the addon's F11 for
    // Interact With Target, or without it the player's own key.
    modernInteractKey_ = addon ? std::optional<KeyChord>(bindingsaddon::interactKey()) : bindings.keyFor("INTERACTTARGET");
    clientBindings_ = bindings;

    // A stance, form or Stealth swaps the page the main bar shows, and its keys
    // press that page: in Battle Stance "2" is slot 74, not slot 2. Reading
    // slots 1-12 regardless, a warrior pressed Heroic Strike for Battle Shout.
    const int mainBarFirst = mainBarFirstSlot(snapshot->player.auras);

    for (const auto& slot : modern_->actionBars())
    {
        // Macros have no spell id to go by.
        if (slot.macro)
        {
            continue;
        }

        std::string command;
        std::optional<KeyChord> key;

        if (WowBindings::onSwappingPage(slot.slot))
        {
            // The addon's key presses the slot itself, whatever page is shown,
            // so it holds through a change of stance. Without it, only the page
            // shown now has keys.
            if (addon)
            {
                key = bindingsaddon::keyForSlot(slot.slot);
            }
            else if (slot.slot >= mainBarFirst && slot.slot < mainBarFirst + 12)
            {
                command = WowBindings::commandForSlot(slot.slot - mainBarFirst + 1);
            }
        }
        else
        {
            command = WowBindings::commandForSlot(slot.slot);
        }

        if (!key && !command.empty())
        {
            key = bindings.keyFor(command);
        }

        // The addon presses any slot, the pages behind the main bar too, which
        // have no button of their own.
        if (!key && addon)
        {
            key = bindingsaddon::keyForSlot(slot.slot);
        }

        if (!key && command.empty())
        {
            continue;
        }

        ActionBarSlot s;
        s.slot = slot.slot;
        s.item = slot.item;
        s.id = slot.id;
        s.key = key;

        if (const std::string* name = s.item ? itemName(s.id) : spellNames_.find(s.id))
        {
            s.name = *name;
        }

        bars.push_back(std::move(s));
    }

    // Stances, forms and Stealth sit on the stance bar, which the slots do not
    // cover. The addon has a button for each that casts it by name; without it
    // the stance bar's own keys, for the buttons whose place is known. After
    // the slots, so one put on an action bar keeps its slot. Slot 0: none.
    // Where the UI can be read, a stance whose button is known is clicked there,
    // key or not: the stance bar's keys are chords, which do not always reach
    // the game.
    const std::string classPrefix = std::string(toString(snapshot->player.playerClass)) + ".";
    const bool clickStances = supportsUi();
    const auto shapeshifts = shapeshiftSpells();

    for (std::size_t i = 0; i < shapeshifts.size(); ++i)
    {
        const ShapeshiftSpell& spell = shapeshifts[i];

        if (!spell.action.starts_with(classPrefix))
        {
            continue;
        }

        std::optional<KeyChord> key;
        int slot = 0;

        if (clickStances && spell.barButton > 0)
        {
            slot = kStanceBarSlots + spell.barButton;
        }
        else if (addon)
        {
            key = bindingsaddon::keyForShapeshift(i);
        }
        else if (spell.barButton > 0)
        {
            key = bindings.keyFor(std::format("SHAPESHIFTBUTTON{}", spell.barButton));
        }

        if (!key && !slot)
        {
            continue;
        }

        ActionBarSlot s;
        s.slot = slot;
        s.id = spell.spellId;
        s.key = key;
        s.name = std::string(spell.name);
        bars.push_back(std::move(s));
    }

    return bars;
}

std::vector<ActionBarSlot> WowMemoryStateProvider::readActionBars(bool refresh)
{
    std::vector<ActionBarSlot> bars;

    if (!memory_->attached())
    {
        return bars;
    }

    if (modern_)
    {
        return readModernActionBars();
    }

    WowBindings bindings;
    const std::string playerName = layout_->playerName ? memory_->readString(layout_->playerName, 48) : "";

    for (const auto& file : bindingFilesFor(installDirectory(), "", playerName))
    {
        bindings.applyFile(file);
    }

    clientBindings_ = bindings;
    const auto memorySlots = readActionBarSlots();

    std::optional<std::string> seq;

    if (supportsLua() && layout_->cvarList != 0 && isInGame())
    {
        seq = readCVar("og_ab_seq", actionBarSeqAddress_);

        if (refresh && requestActionBarScan())
        {
            // The client runs the scan on its next frame.
            const auto before = seq;

            for (int i = 0; i < 40; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                seq = readCVar("og_ab_seq", actionBarSeqAddress_);

                if (seq && !seq->empty() && seq != before)
                {
                    break;
                }
            }
        }
    }

    if (seq && !seq->empty())
    {
        lastActionBarSeq_ = *seq;
        const auto space = seq->find(' ');
        luaCanUseActions_ = space != std::string::npos && seq->substr(space + 1) == "1";

        const auto readPages = [this](const char* prefix, std::array<std::uint32_t, 8>& addresses)
        {
            std::string text;

            for (std::size_t i = 0; i < addresses.size(); ++i)
            {
                if (auto page = readCVar(std::format("{}{}", prefix, i + 1), addresses[i]))
                {
                    text += *page;
                }
            }

            return text;
        };
        // The built-in names stay: they are English, which scripts are written in.
        const std::string spellbook = readPages("og_sb", spellbookPageAddresses_);

        for (const auto line : split(spellbook, '\n'))
        {
            const auto f = split(line, '\t');

            if (f.size() < 2 || f[1].empty())
            {
                continue;
            }

            const auto id = number<std::uint32_t>(f[0]);

            if (id != 0 && !spellNames_.find(id))
            {
                spellNames_.set(id, std::string(f[1]));
            }
        }

        const std::string slots = readPages("og_ab", actionBarPageAddresses_);

        for (const auto line : split(slots, '\n'))
        {
            const auto f = split(line, '\t');

            if (f.size() < 5)
            {
                continue;
            }

            ActionBarSlot s;
            s.slot = number<int>(f[0]);

            if (s.slot < 1 || s.slot > static_cast<int>(memorySlots.size()))
            {
                continue;
            }

            s.item = f[1] == "item";
            s.id = number<std::uint32_t>(f[2]);
            s.name = std::string(f[3]);

            if (!f[4].empty())
            {
                s.key = KeyChord::parseWow(f[4]);
            }

            if (f.size() >= 8)
            {
                s.castTime = Duration(number<std::uint32_t>(f[5]));
                s.range = ActionRange{number<float>(f[6]), number<float>(f[7])};
            }

            // The slot in memory holds the spell id itself; trust it over the API.
            const std::uint32_t raw = memorySlots[static_cast<std::size_t>(s.slot - 1)];

            if (!s.item && raw != 0 && (raw & 0xC0000000) == 0)
            {
                s.id = raw;
            }

            if (s.id != 0)
            {
                bars.push_back(std::move(s));
            }
        }

        return bars;
    }

    // No Lua: Bar 1 on the game's default keys.
    const std::array<Key, 12> bar1Keys = {Key::D1, Key::D2, Key::D3, Key::D4, Key::D5, Key::D6, Key::D7, Key::D8, Key::D9, Key::D0, Key::Minus, Key::Equals};

    for (std::size_t i = 0; i < bar1Keys.size(); ++i)
    {
        const std::uint32_t raw = memorySlots[i];

        if (raw == 0 || (raw & 0x40000000) != 0)
        {
            continue; // empty, or a macro
        }

        ActionBarSlot s;
        s.slot = static_cast<int>(i + 1);
        s.key = KeyChord{bar1Keys[i]};
        // In 3.3.5a, items placed directly on action bars have bit 31 set (0x80000000 | itemId).
        s.item = (raw & 0x80000000) != 0;
        s.id = raw & 0x7FFFFFFF;
        bars.push_back(std::move(s));
    }

    return bars;
}

void WowMemoryStateProvider::setLearnedSpellsFile(std::filesystem::path file)
{
    learnedSpellsFile_ = std::move(file);
    std::error_code ec;

    if (!std::filesystem::exists(learnedSpellsFile_, ec))
    {
        return;
    }

    SpellNames learned;

    if (learned.loadYaml(learnedSpellsFile_) < 0)
    {
        log_.warn(logcat::Action, "could not read the spell names learned earlier from {}", learnedSpellsFile_.string());
        return;
    }

    std::ifstream in(learnedSpellsFile_);
    std::string line;

    // The ids are the file's keys; the names come from the parsed file.
    while (std::getline(in, line))
    {
        std::uint32_t id = 0;
        const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), id);

        if (error != std::errc{} || id == 0)
        {
            continue;
        }

        if (const std::string* name = learned.find(id); name && !name->empty() && name->size() <= 80)
        {
            learnedSpells_[id] = *name;

            // The built-in table is the one trusted where both know a spell.
            if (!spellNames_.find(id))
            {
                spellNames_.set(id, *name);
            }
        }
    }
}

std::optional<UiItemTooltip> WowMemoryStateProvider::tooltipOver(const UiFrame& frame, const std::function<bool(const UiFrame&)>& hover, bool& moved)
{
    // The tooltip showing before the mouse moves is some other thing's.
    const auto before = uiItemTooltip();
    moved = hover(frame);

    if (!moved)
    {
        return std::nullopt;
    }

    constexpr auto kPoll = std::chrono::milliseconds(50);
    constexpr int kPolls = 24;
    constexpr int kOwnAfter = 12; // the mouse was on this button already
    std::optional<UiItemTooltip> seen;

    for (int poll = 0; poll < kPolls; ++poll)
    {
        std::this_thread::sleep_for(kPoll);
        auto tooltip = uiItemTooltip();

        if (!tooltip || tooltip->name.empty() || (before && tooltip->owner == before->owner && poll < kOwnAfter))
        {
            seen.reset();
            continue;
        }

        // Twice the same, a moment apart: not caught half filled in.
        if (seen && tooltip->name == seen->name && tooltip->lines == seen->lines)
        {
            return tooltip;
        }

        seen = std::move(tooltip);
    }

    return std::nullopt;
}

int WowMemoryStateProvider::readSpellDataFromTooltips(ActionMap& actionMap, const std::function<bool(const UiFrame&)>& hover)
{
    tooltipSpellData_.clear();

    if (!hover || !supportsUi() || !supportsUiBagSlots() || !capture())
    {
        return 0;
    }

    std::set<std::uint32_t> looked;

    for (const ActionBarSlot& slot : readActionBars(false))
    {
        if (slot.item || slot.id == 0 || !looked.insert(slot.id).second)
        {
            continue;
        }

        const std::string button = actionButtonFrame(slot.slot);
        const auto frame = button.empty() ? std::nullopt : uiFrame(button);

        // A bar not shown has no button to point at.
        if (!frame || !frame->visible)
        {
            continue;
        }

        bool moved = false;
        const auto tooltip = tooltipOver(*frame, hover, moved);

        if (!moved)
        {
            break;
        }

        const std::string* known = spellNames_.find(slot.id);
        const std::string& name = known ? *known : slot.name;

        // Some other button's tooltip, were the bars to change under the mouse.
        if (!tooltip || (!name.empty() && SpellNames::actionName(tooltip->name) != SpellNames::actionName(name)))
        {
            log_.debug(logcat::Action, "Slot {} ({}): its tooltip did not come up - cast time and range as actions.yaml has them", slot.slot, name.empty() ? std::to_string(slot.id) : name);
            continue;
        }

        const SpellTooltipData data = parseSpellTooltip(tooltip->lines);

        if (!data.castTime && !data.range)
        {
            continue;
        }

        tooltipSpellData_[slot.id] = data;
        log_.debug(logcat::Action, "read from its tooltip: {} casts in {}, range {}", tooltip->name, data.castTime ? std::format("{:.1f}s", toSeconds(*data.castTime)) : "?", data.range ? std::format("{:.0f}-{:.0f} yd", data.range->min, data.range->max) : "?");
    }

    if (tooltipSpellData_.empty())
    {
        return 0;
    }

    log_.info(logcat::Action, "spell data: cast times and ranges of {} spells, from their tooltips", tooltipSpellData_.size());
    autoDetectActionBindings(actionMap, false);
    return static_cast<int>(tooltipSpellData_.size());
}

int WowMemoryStateProvider::learnSpellNamesFromTooltips(ActionMap& actionMap, const std::function<bool(const UiFrame&)>& hover)
{
    if (!hover || !supportsUi() || !supportsUiBagSlots() || !capture())
    {
        return 0;
    }

    int learned = 0;

    for (const ActionBarSlot& slot : readActionBars(false))
    {
        if (slot.item || slot.id == 0 || !slot.name.empty() || spellNames_.find(slot.id) || tooltipsTried_.contains(slot.id))
        {
            continue;
        }

        const std::string button = actionButtonFrame(slot.slot);
        const auto frame = button.empty() ? std::nullopt : uiFrame(button);

        // A bar not shown has no button to point at.
        if (!frame || !frame->visible)
        {
            continue;
        }

        tooltipsTried_.insert(slot.id);
        bool moved = false;
        const auto tooltip = tooltipOver(*frame, hover, moved);

        if (!moved)
        {
            break;
        }

        const std::string name = tooltip ? tooltip->name : std::string{};

        if (name.empty() || name.size() > 80 || SpellNames::actionName(name).empty())
        {
            log_.warn(logcat::Action, "Slot {} holds spell {}, which the bot has no name for, and its tooltip told none - no rule can use it", slot.slot, slot.id);
            continue;
        }

        spellNames_.set(slot.id, name);
        learnedSpells_[slot.id] = name;
        ++learned;
        log_.info(logcat::Action, "read from its tooltip: spell {} in Slot {} is {}", slot.id, slot.slot, name);
    }

    if (learned == 0)
    {
        return 0;
    }

    if (!learnedSpellsFile_.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(learnedSpellsFile_.parent_path(), ec);
        std::ofstream out(learnedSpellsFile_, std::ios::trunc);

        for (const auto& [id, name] : learnedSpells_)
        {
            std::string quoted;

            for (const char c : name)
            {
                if (c == '"' || c == '\\')
                {
                    quoted += '\\';
                }

                quoted += c;
            }

            out << id << ": \"" << quoted << "\"\n";
        }

        if (!out)
        {
            log_.warn(logcat::Action, "could not keep the spell names read from tooltips in {}", learnedSpellsFile_.string());
        }
    }

    autoDetectActionBindings(actionMap, false);
    return learned;
}

void WowMemoryStateProvider::autoDetectActionBindings(ActionMap& actionMap, bool refresh)
{
    if (!memory_->attached() && !attach())
    {
        return;
    }

    auto ensureDefault = [&](std::string_view name, Key key)
    {
        if (!actionMap.contains(name))
        {
            const auto* info = actionMap.settingsFor(name);
            ActionBinding binding = info ? *info : ActionBinding{std::string(name), {}, Duration(0), false};
            binding.chord = KeyChord{key};
            actionMap.add(std::move(binding));
        }
    };
    ensureDefault(actions::MoveForward, Key::W);
    ensureDefault(actions::MoveBackward, Key::S);
    ensureDefault(actions::TurnLeft, Key::A);
    ensureDefault(actions::TurnRight, Key::D);
    ensureDefault(actions::StrafeLeft, Key::Q);
    ensureDefault(actions::StrafeRight, Key::E);
    ensureDefault(actions::Jump, Key::Space);
    ensureDefault(actions::SwimUp, Key::Space);
    ensureDefault(actions::SwimDown, Key::X);
    ensureDefault(actions::TargetNext, Key::Tab);
    ensureDefault(actions::TargetClear, Key::Escape);
    ensureDefault(actions::Interact, Key::F11); // F12 takes a screenshot
    ensureDefault(actions::ReleaseSpirit, Key::Minus);
    ensureDefault(actions::RetrieveCorpse, Key::Equals);

    auto bars = readActionBars(refresh);

    // Moving, jumping and Tab targeting on the keys the character has them
    // on, not on a list in a file: a player who moves with the arrows or on
    // ESDF is followed.
    if (clientBindings_)
    {
        static constexpr std::pair<std::string_view, std::string_view> kClientKeys[] = {
            {actions::MoveForward, "MOVEFORWARD"}, {actions::MoveBackward, "MOVEBACKWARD"}, {actions::TurnLeft, "TURNLEFT"}, {actions::TurnRight, "TURNRIGHT"}, {actions::StrafeLeft, "STRAFELEFT"}, {actions::StrafeRight, "STRAFERIGHT"}, {actions::Jump, "JUMP"}, {actions::SwimUp, "JUMP"}, {actions::SwimDown, "SITORSTAND"}, {actions::TargetNext, "TARGETNEARESTENEMY"}, {actions::TargetClear, "TOGGLEGAMEMENU"},
        };

        for (const auto& [action, command] : kClientKeys)
        {
            const auto chord = clientBindings_->keyFor(command);
            const ActionBinding* existing = actionMap.find(action);

            if (!chord)
            {
                actionMap.remove(action);
                continue;
            }

            if (!existing || existing->chord == *chord)
            {
                continue;
            }

            ActionBinding binding = *existing;
            binding.chord = *chord;
            log_.info(logcat::Action, "{} -> Key {} (the client's {})", action, chord->toString(), command);
            actionMap.add(std::move(binding));
        }
    }

    // The read-only client uses its Interact With Target binding.
    if (modern_)
    {
        const ActionBinding* interact = actionMap.find(actions::Interact);

        if (modernInteractKey_ && interact && interact->chord != *modernInteractKey_)
        {
            ActionBinding binding = *interact;
            binding.chord = *modernInteractKey_;
            log_.info(logcat::Action, "Auto-detected action: {} -> Key {} (Interact With Target)", binding.name, binding.chord.toString());
            actionMap.add(std::move(binding));
        }
        else if (!modernInteractKey_ && !bars.empty() && !warnedNoInteractKey_ && !supportsUi())
        {
            // With a clickable UI the bot binds Interact With Target itself, through the chat.
            warnedNoInteractKey_ = true;
            log_.warn(logcat::Action,
                      "no key is bound to Interact With Target and the {} addon is not there - looting presses {}, "
                      "which may do nothing",
                      bindingsaddon::Name, interact ? interact->chord.toString() : std::string("nothing"));
        }
    }

    if (bars.empty())
    {
        return;
    }

    std::string classPrefix;
    std::vector<ItemStack> carried;

    if (modern_)
    {
        if (auto snapshot = modern_->capture(clock_.now(), config_.visibilityRange))
        {
            classPrefix = std::string(toString(snapshot->player.playerClass));
            carried = std::move(snapshot->player.bagItems);
        }
    }
    else
    {
        Guid localGuid = 0;
        const auto objects = enumerateObjects(localGuid);

        for (const auto& obj : objects)
        {
            if (obj.guid == localGuid)
            {
                PlayerState p;
                readPlayerExtras(obj, p);
                readBags(objects, obj, p);
                classPrefix = std::string(toString(p.playerClass));
                carried = std::move(p.bagItems);
                break;
            }
        }
    }

    // A slot holding an item the bags do not have presses nothing, so the one
    // holding the most of what is carried claims Eat or Drink first; the others
    // stay as alternatives for when it runs out.
    const auto carriedCount = [&](std::uint32_t id)
    {
        int n = 0;

        for (const ItemStack& item : carried)
        {
            if (item.itemId == id)
            {
                n += item.count;
            }
        }

        return n;
    };
    std::stable_sort(bars.begin(), bars.end(),
                     [&](const ActionBarSlot& a, const ActionBarSlot& b)
                     {
                         const int left = a.item ? carriedCount(a.id) : std::numeric_limits<int>::max();
                         const int right = b.item ? carriedCount(b.id) : std::numeric_limits<int>::max();
                         return left > right;
                     });

    // What this pass bound, and the keys it bound them to.
    std::set<std::string, std::less<>> detected;
    std::vector<std::pair<KeyChord, std::string>> detectedKeys;

    // One slot per action: one with a key wins over one pressed through Lua, then the lowest slot.
    std::set<std::string, std::less<>> claimed;
    std::set<std::string, std::less<>> claimedWithKey;
    const auto claim = [&](const std::string& name, bool viaLua)
    {
        if (claimedWithKey.contains(name) || (viaLua && claimed.contains(name)))
        {
            return false;
        }

        claimed.insert(name);

        if (!viaLua)
        {
            claimedWithKey.insert(name);
        }

        return true;
    };

    for (const auto& slot : bars)
    {
        const bool viaLua = !slot.key;
        // Without a key the slot is pressed through Lua, or on a client whose
        // UI can be read, by a click on the button it shows on.
        const std::string button = viaLua && !luaCanUseActions_ && supportsUi() ? actionButtonFrame(slot.slot) : "";

        if (viaLua && !luaCanUseActions_ && button.empty())
        {
            if (warnedKeylessSlots_.insert(slot.slot).second)
            {
                log_.debug(logcat::Action, "action bar slot {} ({}) has no key binding, and neither Lua nor a click can press it here", slot.slot, slot.name.empty() ? std::to_string(slot.id) : slot.name);
            }

            continue;
        }

        const KeyChord chord = slot.key.value_or(KeyChord{});
        const std::string how = !viaLua ? "Key " + chord.toString() : !button.empty() ? "click " + button : "Lua";
        const std::string where = std::format("{} ({})", slot.slot > kStanceBarSlots ? std::format("Stance bar {}", slot.slot - kStanceBarSlots) : slot.slot > 0 ? std::format("Slot {}", slot.slot) : std::string("Stance bar"), how);
        const std::string* gameName = slot.item ? nullptr : spellNames_.find(slot.id);
        const std::string spellName = gameName ? *gameName : slot.name;
        const auto bind = [&](ActionBinding binding)
        {
            binding.actionSlot = slot.slot;
            detected.insert(binding.name);

            if (binding.chord.valid())
            {
                detectedKeys.emplace_back(binding.chord, binding.name);
            }

            actionMap.add(std::move(binding));
        };
        // Only what detection changes is logged: detecting again at the same bars stays quiet.
        const auto changes = [&](std::string_view name)
        {
            const ActionBinding* existing = actionMap.find(name);
            return !existing || existing->chord != chord || existing->actionSlot != slot.slot;
        };

        if (slot.item)
        {
            const std::string* iName = itemName(slot.id);
            const std::string_view nameOf = iName ? std::string_view(*iName) : std::string_view(slot.name);
            // A potion or healthstone on a bar is pressed there, never used from
            // the bags by name; potions share a two minute cooldown.
            const bool health = og::isHealthPotionItem(slot.id, nameOf);
            const bool mana = og::isManaPotionItem(slot.id, nameOf);
            const bool potion = health || mana;
            const char* action = health ? "Player.HealthPotion" : mana ? "Player.ManaPotion" : og::isWaterItem(slot.id, nameOf) ? "Player.Drink" : og::isFoodItem(slot.id, nameOf) ? "Player.Eat" : nullptr;

            if (!action)
            {
                continue;
            }

            ActionBinding* existing = detected.contains(action) ? actionMap.find(action) : nullptr;

            if (!claim(action, viaLua))
            {
                // Another slot has it already: this one is kept for when that one's stack runs out.
                if (existing)
                {
                    existing->itemAlternatives.push_back({chord, slot.slot, slot.id});
                }

                continue;
            }

            const bool changed = changes(action);
            ActionBinding binding{action, chord, potion ? Duration(120000) : Duration(2000), false};
            binding.itemId = slot.id;

            // A slot with a key takes over from one pressed through Lua, which stays an alternative.
            if (existing)
            {
                binding.itemAlternatives = std::move(existing->itemAlternatives);
                binding.itemAlternatives.push_back({existing->chord, existing->actionSlot, existing->itemId});
            }

            bind(std::move(binding));

            if (changed)
            {
                log_.info(logcat::Action, "Auto-detected item: {} ({}) -> {}", action, iName ? *iName : !slot.name.empty() ? slot.name : std::to_string(slot.id), where);
            }

            continue;
        }

        // The built-in English name first, so scripts keep working on a localized client.
        const std::string* known = spellNames_.find(slot.id);
        std::string cleanName = SpellNames::actionName(known ? *known : slot.name);

        // The one Judgement button, whatever the seal makes of it: WoW Forever
        // 1.60.1 puts it on the bar as "Judgement of Light", and the class's
        // rules press Paladin.Judgement.
        if (cleanName.starts_with("JudgementOf"))
        {
            cleanName = "Judgement";
        }

        // Skipped without a word, the rule using it only said "no usable
        // binding" - with the spell right there on the bar (Devouring Plague).
        // Where the button's tooltip can be read, that is tried first
        // (learnSpellNamesFromTooltips), and says so itself when it fails.
        if (cleanName.empty() && slot.id != 0 && !(supportsUi() && supportsUiBagSlots()) && warnedNamelessSpells_.insert(slot.id).second)
        {
            log_.warn(logcat::Action, "{} holds spell {}, which the bot has no name for - no rule can use it", where, slot.id);
        }

        if (cleanName.empty() || !claim(cleanName, viaLua))
        {
            continue;
        }

        const bool isAttack = (cleanName == "AutoAttack" || cleanName == "Attack");
        Duration cd = isAttack ? Duration(2000) : Duration(0);
        bool gcd = !isAttack;

        const std::string prefixed = (!classPrefix.empty() && classPrefix != "Unknown") ? classPrefix + "." + cleanName : "";

        // Inherit the timings configured in actions.yaml: auto-detection only
        // learns which key a spell sits on, not how it behaves. castTime has to
        // come along or the binding it replaces loses it, and the engine then
        // never stops the character for the cast - it walks off mid-Conjure and
        // breaks its own spell, over and over.
        // The range too: without it every rule counts as in reach, and the
        // rotation presses Frostbolt at 45 yards into "Out of range".
        Duration castTime{0};
        ActionRange range{};

        if (!prefixed.empty())
        {
            if (const auto* existing = actionMap.settingsFor(prefixed))
            {
                if (existing->cooldown > Duration(0))
                {
                    cd = existing->cooldown;
                }

                gcd = existing->onGcd;
                castTime = existing->castTime;
                range = existing->range;
            }
        }

        if (cd == Duration(0))
        {
            if (const auto* existing = actionMap.settingsFor(cleanName))
            {
                if (existing->cooldown > Duration(0))
                {
                    cd = existing->cooldown;
                }

                gcd = existing->onGcd;

                if (castTime == Duration(0))
                {
                    castTime = existing->castTime;
                }

                if (!range.limited())
                {
                    range = existing->range;
                }
            }
        }

        // What the client reports about the spell fills in whatever actions.yaml leaves out.
        if (castTime == Duration(0))
        {
            castTime = slot.castTime;
        }

        if (!range.limited())
        {
            range = slot.range;
        }

        // What the spell's tooltip said goes before both: it has the talents in.
        if (const auto told = tooltipSpellData_.find(slot.id); told != tooltipSpellData_.end())
        {
            castTime = told->second.castTime.value_or(castTime);
            range = told->second.range.value_or(range);
        }

        ActionBinding spell{cleanName, chord, cd, gcd, castTime, range};
        spell.spellId = slot.id;

        if (!prefixed.empty())
        {
            const bool changed = changes(prefixed);
            ActionBinding named = spell;
            named.name = prefixed;
            bind(std::move(named));

            if (changed)
            {
                log_.info(logcat::Action, "Auto-detected action: {} -> {} (cooldown: {:.1f}s, cast: {:.1f}s)", prefixed, where, toSeconds(cd), toSeconds(castTime));
            }
        }

        bind(std::move(spell));

        if (isAttack)
        {
            bind(ActionBinding{"AutoAttack", chord, cd, false});

            if (!classPrefix.empty())
            {
                bind(ActionBinding{classPrefix + ".AutoAttack", chord, cd, false});
            }
        }
    }

    // A key the action bar gives to one action must not stay bound to another
    // from actions.yaml: Player.Eat's "7" cast the Conjure Water sitting in
    // slot 7 every time the bot rested.
    std::vector<std::string> stale;

    for (const auto& [name, binding] : actionMap.bindings())
    {
        if (detected.contains(name) || !binding.chord.valid())
        {
            continue;
        }

        for (const auto& [chord, owner] : detectedKeys)
        {
            if (chord != binding.chord)
            {
                continue;
            }

            // actions.yaml carries every class; only what this character uses is worth a warning.
            const bool ours = name.find('.') == std::string::npos || name.starts_with("Player.") || name.starts_with("Target.") || (!classPrefix.empty() && name.starts_with(classPrefix + "."));

            // Expected, not a problem: actions.yaml's default keys are only
            // placeholders until the bar says otherwise.
            if (ours)
            {
                log_.debug(logcat::Action, "'{}' was on key {}, which the action bar uses for {} - unbound it", name, chord.toString(), owner);
            }

            stale.push_back(name);
            break;
        }
    }

    for (const auto& name : stale)
    {
        actionMap.remove(name);
    }

    // Without Lua, the bars read from memory are all the bot can know. A class
    // spell or consumable they do not show on a key would press actions.yaml's
    // default key, which on this character can be anything - T is the client's
    // Attack, and Conjure Water sat on it.
    if (modern_)
    {
        std::vector<std::string> absent;

        for (const auto& [name, binding] : actionMap.bindings())
        {
            if (detected.contains(name) || !binding.chord.valid())
            {
                continue;
            }

            const bool classSpell = !classPrefix.empty() && name.starts_with(classPrefix + ".");
            const bool consumable = name == "Player.Eat" || name == "Player.Drink" || name == "Player.HealthPotion" || name == "Player.ManaPotion";

            if (classSpell || consumable)
            {
                absent.push_back(name);
            }
        }

        for (const auto& name : absent)
        {
            log_.info(logcat::Action, "'{}' is not on an action bar key - unbound it", name);
            actionMap.remove(name);
        }
    }

    // Default pet bar command positions. Clients whose UI is readable click
    // these buttons; hidden or missing frames are rejected by the clicker.
    // Custom pet bars that rearrange commands are not supported here.
    if (supportsUi())
    {
        static constexpr std::pair<std::string_view, int> commands[] = {
            {"Pet.Attack", 1}, {"Pet.Follow", 2}, {"Pet.Stay", 3}, {"Pet.Aggressive", 8}, {"Pet.Defensive", 9}, {"Pet.Passive", 10},
        };

        for (const auto& [name, slot] : commands)
        {
            const auto* settings = actionMap.settingsFor(name);
            ActionBinding binding = settings ? *settings : ActionBinding{std::string(name), {}, Duration(1000), false};
            binding.chord = {};
            binding.chatCommand.clear();
            binding.actionSlot = kPetBarSlots + slot;
            actionMap.add(std::move(binding));
        }
    }

    // Releasing the spirit and taking the corpse back go through Lua on the
    // older clients; on 2.5.6 the bindings addon gives each a key of its own.
    // Set last, so no action bar key takes them away.
    if (modern_ && bindingsAddonUsable_)
    {
        for (const auto& [name, chord] : {std::pair{actions::ReleaseSpirit, bindingsaddon::releaseSpiritKey()}, std::pair{actions::RetrieveCorpse, bindingsaddon::retrieveCorpseKey()}})
        {
            const ActionBinding* existing = actionMap.find(name);

            if (existing && existing->chord == chord)
            {
                continue;
            }

            ActionBinding binding = existing ? *existing : ActionBinding{std::string(name), chord, Duration(1000), false};
            binding.chord = chord;
            actionMap.add(std::move(binding));
        }
    }
}

std::string WowMemoryStateProvider::describe()
{
    std::string out;

    if (!memory_->attached() && !attach())
    {
        const std::string problem = attachProblem();
        out += std::format("not attached: {}\n", !problem.empty() ? problem : memory_->lastError().empty() ? "no WoW process" : memory_->lastError());

        for (const auto& p : findClientProcesses())
        {
            out += std::format("  candidate pid {} comm='{}' cmd='{}'\n", p.pid, p.name, p.commandLine);
        }

        return out;
    }

    const auto version = readClientVersion(*memory_, imageBase_);
    out += std::format("attached to pid {}, image at 0x{:X}\n", memory_->pid(), imageBase_);
    out += std::format("WoW.exe version: {}  ->  layout {} (build {}){}\n", version ? version->toString() : "unknown", layout_->clientVersion.empty() ? std::string(layout_->name) : std::format("{} {}", layout_->name, layout_->clientVersion), layout_->build, layout_->readOnly ? ", read-only" : "");
    out += std::format("object manager: 0x{:X}  in world: {}\n", modern_ ? modern_->objectManager() : std::uint64_t{objectManager()}, isInGame() ? "yes" : "no");
    auto snap = capture();

    if (!snap)
    {
        out += "no snapshot (not in world, or offsets do not match this client)\n";
        return out;
    }

    const PlayerState& p = snap->player;
    out += std::format("player '{}' {} level {} class {} race {} faction template {} hp {}/{} power {}/{} ({})\n", p.name, formatGuid(p.guid), p.level, toString(p.playerClass), p.race, p.factionTemplate, p.health, p.maxHealth, p.power, p.maxPower, toString(p.powerType));

    if (p.knownTaxiNodes)
    {
        std::string nodes;

        for (const auto id : *p.knownTaxiNodes)
        {
            nodes += std::format("{}{}", nodes.empty() ? "" : ", ", id);
        }

        out += std::format("flight points learnt: {}\n", nodes.empty() ? std::string("none") : nodes);
    }
    out += std::format("position {} heading {:.2f} map {} zone {} \"{}\" combat={} autoAttack={} shooting={} dead={} ghost={} sitting={}\n", p.position, p.heading, snap->mapId ? std::to_string(*snap->mapId) : "?", snap->zoneId, snap->zoneText, p.inCombat, p.isAutoAttacking, p.isShooting, p.isDead, p.isGhost, p.isSitting);

    out += std::format("xp {}/{}  target {}  auras {}  broken items {}  money {}\n", p.xp, p.nextLevelXp, formatGuid(snap->targetGuid), p.auras.size(), p.brokenItems, p.money < 0 ? std::string("?") : std::format("{}g {}s {}c", p.money / 10000, p.money / 100 % 100, p.money % 100));

    if (const UnitState* pet = snap->pet())
    {
        out += std::format("pet '{}' {} level {} hp {}/{} dist {:.1f}{}\n", pet->name, formatGuid(pet->guid), pet->level, pet->health, pet->maxHealth, distance(p.position, pet->position), pet->isDead ? " dead" : "");
    }

    // The skills gathering and skinning go by; "unread" where the layout has none.
    out += p.skills.empty() ? std::string("skills unread\n") : std::format("skills: herbalism {} mining {} skinning {} fishing {} ({} lines)\n", p.herbalismSkill(), p.miningSkill(), p.skinningSkill(), p.fishingSkill(), p.skills.size());

    for (const auto& a : p.auras)
    {
        out += std::format("  {} {} ({}) x{}\n", a.harmful ? "debuff" : "buff  ", a.name.empty() ? "?" : a.name, a.spellId, a.stacks);
    }

    out += std::format("bags: {} free slots, {} kinds of items\n", p.freeBagSlots, p.bagItems.size());

    if (p.mainHandTempEnchant)
    {
        out += *p.mainHandTempEnchant ? std::format("main hand: temporary enchant {}, {} s left\n", *p.mainHandTempEnchant, p.mainHandTempEnchantSecondsLeft) : std::string("main hand: no temporary enchant\n");
    }

    for (const auto& item : p.bagItems)
    {
        out += std::format("  {} x{} (item {})\n", item.name.empty() ? "?" : item.name, item.count, item.itemId);
    }

    const auto known = readKnownSpells();

    if (!known.empty())
    {
        out += std::format("spellbook ({} spells):\n  ", known.size());

        for (std::size_t i = 0; i < known.size(); ++i)
        {
            const std::string* n = spellNames_.find(known[i]);
            out += std::format("{} ({}){}", n ? *n : "Unknown", known[i], i + 1 < known.size() ? ", " : "\n");
        }
    }

    if (layout_->actionBarFirstSlot || modern_)
    {
        const auto bars = readActionBars(false);
        out += modern_ ? "action bars (keys from the WTF bindings):\n" : lastActionBarSeq_.empty() ? "action bar (Bar 1 - no scan from a running bot yet):\n" : "action bars:\n";

        for (const auto& s : bars)
        {
            const std::string* builtIn = s.item ? itemName(s.id) : spellNames_.find(s.id);
            out += std::format("  Slot {:>3} ({:<9}): {}{} (id {})\n", s.slot, s.key ? s.key->toString() : "Lua", s.item ? "[Item] " : "", builtIn ? *builtIn : !s.name.empty() ? s.name : "Unknown", s.id);
        }
    }

    out += std::format("{} units, {} game objects in range\n", snap->units.size(), snap->objects.size());
    std::vector<const UnitState*> units;

    for (const auto& u : snap->units)
    {
        units.push_back(&u);
    }

    std::sort(units.begin(), units.end(), [&](const UnitState* a, const UnitState* b) { return distance(a->position, p.position) < distance(b->position, p.position); });

    for (std::size_t i = 0; i < std::min<std::size_t>(units.size(), 15); ++i)
    {
        const UnitState& u = *units[i];
        out += std::format("  {:<24} lvl {:>2} hp {:>5}/{:<5} dist {:>5.1f} {:<8} ft {:<5} {}{}{}{}{}{}{}{}{}\n", u.name.empty() ? "(player)" : u.name, u.level, u.health, u.maxHealth, distance(u.position, p.position), toString(u.reaction), u.factionTemplate, u.classification != Classification::Normal ? std::string(toString(u.classification)) + " " : std::string(), u.inCombat ? "combat " : "", u.isDead ? "dead " : "", u.isLootable ? "lootable " : "", u.isSkinnable ? "skinnable " : "", u.isVendor ? "vendor " : "", u.canRepair ? "repair " : "", u.isFlightMaster ? (u.newFlightPath ? "flight master (new flight path !) " : "flight master ") : "", u.questMarker == QuestMarker::Available ? "quest !" : u.questMarker == QuestMarker::Reward ? "quest ?" : u.questMarker == QuestMarker::Incomplete ? "quest ? (not done)" : "");
    }

    for (std::size_t i = 0; i < std::min<std::size_t>(snap->objects.size(), 10); ++i)
    {
        const GameObjectState& g = snap->objects[i];
        out += std::format("  [GO] {:<24} {} dist {:.1f}\n", g.name, toString(g.kind), distance(g.position, p.position));
    }

    return out;
}

bool WowMemoryStateProvider::setTarget(Guid guid)
{
    if (!memory_->attached() || layout_->readOnly || layout_->targetGuid == 0)
    {
        return false;
    }

    return memory_->write<std::uint64_t>(layout_->targetGuid, guid);
}

std::optional<Camera> WowMemoryStateProvider::readCamera() const
{
    if (layout_ && layout_->modern)
    {
        const auto& l = *layout_->modern;
        if (!memory_->attached() || !isInGame() || !l.cameraGlobal || !l.cameraPos || !l.cameraMatrix || !l.cameraFov)
        {
            return std::nullopt;
        }
        const auto owner = memory_->read<std::uint64_t>(imageBase_ + l.cameraGlobal).value_or(0);
        Camera c;
        float axes[3][3]{};
        if (!owner || !memory_->read(owner + l.cameraPos, &c.position, sizeof(c.position)) ||
            !memory_->read(owner + l.cameraMatrix, axes, sizeof(axes)) ||
            !memory_->read(owner + l.cameraFov, &c.fov, sizeof(c.fov)))
        {
            return std::nullopt;
        }
        if (!std::isfinite(c.position.x) || !std::isfinite(c.position.y) || !std::isfinite(c.position.z) ||
            !std::isfinite(c.fov) || c.fov < 0.3f || c.fov > 2.8f)
        {
            return std::nullopt;
        }
        for (int row = 0; row < 3; ++row)
        {
            float length = 0;
            for (int col = 0; col < 3; ++col)
            {
                if (!std::isfinite(axes[row][col]))
                {
                    return std::nullopt;
                }
                length += axes[row][col] * axes[row][col];
            }
            if (std::abs(length - 1.0f) > 0.02f)
            {
                return std::nullopt;
            }
        }
        for (int col = 0; col < 3; ++col)
        {
            c.matrix[0][col] = -axes[1][col];
            c.matrix[1][col] = axes[2][col];
            c.matrix[2][col] = axes[0][col];
        }
        c.aspect = 0; // Derive the aspect from the actual viewport.
        return c;
    }
    if (!memory_->attached() || !isInGame() || layout_->cameraManager == 0)
    {
        return std::nullopt;
    }

    const auto mgr = memory_->readPointer(layout_->cameraManager).value_or(0);

    if (mgr == 0)
    {
        return std::nullopt;
    }

    const auto cam = memory_->readPointer(mgr + layout_->cameraOffset).value_or(0);

    if (cam == 0)
    {
        return std::nullopt;
    }

    Camera c;
    c.position.x = memory_->read<float>(cam + layout_->cameraPos).value_or(0.0f);
    c.position.y = memory_->read<float>(cam + layout_->cameraPos + 4).value_or(0.0f);
    c.position.z = memory_->read<float>(cam + layout_->cameraPos + 8).value_or(0.0f);

    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            c.matrix[row][col] = memory_->read<float>(cam + layout_->cameraMatrix + (row * 3 + col) * 4).value_or(0.0f);
        }
    }

    if (layout_->cameraFov != 0)
    {
        c.fov = memory_->read<float>(cam + layout_->cameraFov).value_or(1.25f);
    }

    if (layout_->cameraAspect != 0)
    {
        c.aspect = memory_->read<float>(cam + layout_->cameraAspect).value_or(16.0f / 9.0f);
    }

    return c;
}

std::optional<UiFrame> WowMemoryStateProvider::uiFrame(std::string_view name)
{
    if (!modern_ || !memory_->attached())
    {
        return std::nullopt;
    }

    return modern_->uiFrame(name);
}

std::optional<Guid> WowMemoryStateProvider::mouseoverGuid()
{
    if (!modern_ || !memory_->attached())
    {
        return std::nullopt;
    }

    return modern_->mouseoverGuid();
}

bool WowMemoryStateProvider::supportsUi() const
{
    return modern_ && layout_ && layout_->modern && (layout_->modern->uiTop || layout_->modern->uiActionButtonTable);
}

bool WowMemoryStateProvider::supportsUiBagSlots() const
{
    return modern_ && modern_->supportsUiBagSlots();
}

std::vector<UiBagSlot> WowMemoryStateProvider::uiBagSlots()
{
    return modern_ && memory_->attached() ? modern_->uiBagSlots() : std::vector<UiBagSlot>{};
}

std::optional<UiItemTooltip> WowMemoryStateProvider::uiItemTooltip()
{
    return modern_ && memory_->attached() ? modern_->uiItemTooltip() : std::nullopt;
}

std::vector<UiItemTooltip> WowMemoryStateProvider::uiComparisonTooltips()
{
    return modern_ && memory_->attached() ? modern_->uiComparisonTooltips() : std::vector<UiItemTooltip>{};
}

std::optional<std::string> WowMemoryStateProvider::uiText(std::string_view frame, std::string_view fontString)
{
    if (!modern_ || !memory_->attached())
    {
        return std::nullopt;
    }

    return modern_->uiText(frame, fontString);
}

std::vector<UiFrame> WowMemoryStateProvider::uiFramesWithText(std::string_view under, std::string_view text)
{
    if (!modern_ || !memory_->attached())
    {
        return {};
    }

    return modern_->uiFramesWithText(under, text);
}

std::optional<UiFrame> WowMemoryStateProvider::uiFrameWithText(std::string_view under, std::string_view text)
{
    if (!modern_ || !memory_->attached())
    {
        return std::nullopt;
    }

    return modern_->uiFrameWithText(under, text);
}

std::optional<ActionCooldown> WowMemoryStateProvider::actionCooldown(int slot)
{
    if (!modern_ || !memory_->attached())
    {
        return std::nullopt;
    }

    const std::string button = actionButtonFrame(slot);

    if (button.empty())
    {
        return std::nullopt;
    }

    // The button of a bar not shown is not kept up to date.
    const auto shown = modern_->uiFrame(button);

    if (!shown || !shown->visible)
    {
        return std::nullopt;
    }

    const auto cooldown = modern_->uiCooldown(button + "Cooldown");
    const std::uint32_t clock = modern_->gameTime();

    if (!cooldown || clock == 0)
    {
        return std::nullopt;
    }

    ActionCooldown out;

    if (cooldown->duration == 0 || cooldown->start == 0)
    {
        return out;
    }

    // Wrapping arithmetic: the clock is a u32 of milliseconds.
    const std::uint32_t elapsed = clock - cooldown->start;
    out.duration = Duration(cooldown->duration);

    // Begun a moment "ahead" of the clock read just after it: all of it left.
    if (elapsed > 0x80000000u)
    {
        out.remaining = out.duration;
    }
    else if (elapsed < cooldown->duration)
    {
        out.remaining = Duration(cooldown->duration - elapsed);
    }

    // The global cooldown is on every spell's button at once, and so is the
    // gap between a wand's shots (1.8 s a shot, on Mind Blast's button too):
    // the same start and length on other buttons of the main bar. Taken for
    // the spell's own, nothing was ever ready while the wand fired.
    constexpr std::uint32_t kGlobalCooldown = 1500;
    int others = 0;

    for (int other = 1; other <= 12 && others < 2; ++other)
    {
        if (other == slot)
        {
            continue;
        }

        if (const auto same = modern_->uiCooldown(actionButtonFrame(other) + "Cooldown"); same && same->start == cooldown->start && same->duration == cooldown->duration)
        {
            ++others;
        }
    }

    out.shared = cooldown->duration <= kGlobalCooldown || others >= 2;
    return out;
}

std::optional<UiFrame> WowMemoryStateProvider::uiRegion(std::string_view frame, std::string_view region)
{
    if (!modern_ || !memory_->attached())
    {
        return std::nullopt;
    }

    return modern_->uiRegion(frame, region);
}

std::optional<ScreenPoint> WowMemoryStateProvider::worldToScreen(const Vec3& worldPos)
{
    const auto cam = readCamera();

    if (!cam)
    {
        return std::nullopt;
    }

    return cam->worldToScreen(worldPos);
}

bool WowMemoryStateProvider::interactUnit(Guid guid, const Vec3& position)
{
    if (!isInGame())
    {
        return false;
    }

    // The client's own right-click loots with autoloot on, so it needs neither
    // click-to-move nor Lua to empty the loot window.
    if (layout_->onRightClickUnit)
    {
        return rightClickUnit(guid, true);
    }

    ensureLootHook();
    return clickToMove(guid, position, 6); // Loot a corpse.
}

bool WowMemoryStateProvider::skinUnit(Guid guid)
{
    // CTM's Loot action does not skin. Vanilla has a real right-click;
    // other layouts use the interact key after selecting/facing the corpse.
    return !layout_->readOnly && isInGame() && layout_->onRightClickUnit && rightClickUnit(guid, true);
}

bool WowMemoryStateProvider::rightClickUnit(Guid guid, bool autoLoot)
{
    if (guid == 0 || !ensureExecutor() || !lua_->idle())
    {
        return false;
    }

    Guid localGuid = 0;

    for (const auto& obj : enumerateObjects(localGuid))
    {
        if (obj.guid != guid)
        {
            continue;
        }

        if (obj.type != c::TypeUnit)
        {
            return false;
        }

        if (!lua_->call(layout_->onRightClickUnit, obj.base, autoLoot ? 1 : 0))
        {
            log_.warn(logcat::Game, "cannot right-click {}: {}", formatGuid(guid), lua_->lastError());
            return false;
        }

        return true;
    }

    return false;
}

void WowMemoryStateProvider::cancelInteraction()
{
    if (layout_->readOnly || !memory_->attached() || !isInGame() || !layout_->ctmBase)
    {
        return;
    }

    const auto base = layout_->ctmBase;
    const auto action = memory_->read<std::uint32_t>(base + layout_->ctmAction).value_or(0);

    // Only clear the interact commands the bot issues, never the player's move.
    if (action == 5 || action == 6 || action == 7)
    {
        memory_->write<std::uint32_t>(base + layout_->ctmAction, 13); // None.
    }
}

bool WowMemoryStateProvider::interactNpc(Guid guid, const Vec3& position)
{
    if (!isInGame())
    {
        return false;
    }

    // The client's right-click is how a player talks to an NPC; without
    // autoloot it opens the vendor's window rather than looting anything.
    if (layout_->onRightClickUnit)
    {
        return rightClickUnit(guid, false);
    }

    // The client needs the NPC selected before the interact lands, or the
    // merchant frame opens against whatever was targeted before.
    setTarget(guid);
    return clickToMove(guid, position, 5); // Talk to an NPC.
}

bool WowMemoryStateProvider::supportsObjectInteraction() const
{
    return layout_->ctmBase != 0 && layout_->gameObjectNamePtr1 != 0;
}

void WowMemoryStateProvider::ensureLootHook()
{
    if (!supportsLua() || !memory_->attached() || !isInGame() || lootHookInstalled_)
    {
        return;
    }

    if (executeLua("if not _OG_LH then "
                   "_OG_LH=CreateFrame('Frame') "
                   "_OG_LH:RegisterEvent('LOOT_OPENED') "
                   "_OG_LH:RegisterEvent('LOOT_SLOT_CLEARED') "
                   "_OG_LH:SetScript('OnEvent',function() "
                   "if GetNumLootItems()>0 then "
                   "for i=GetNumLootItems(),1,-1 do LootSlot(i) end "
                   "end end) "
                   "end"))
    {
        lootHookInstalled_ = true;
    }
}

bool WowMemoryStateProvider::interactObject(Guid guid, const Vec3& position)
{
    if (!supportsObjectInteraction() || !isInGame())
    {
        return false;
    }

    ensureLootHook();

    if (!clickToMove(guid, position, 7))
    {
        return false; // Interact with a game object.
    }

    interactingObject_ = guid;
    return true;
}

void WowMemoryStateProvider::cancelObjectInteraction()
{
    const auto guid = std::exchange(interactingObject_, Guid{0});

    if (!guid || layout_->readOnly || !memory_->attached() || !isInGame() || !layout_->ctmBase)
    {
        return;
    }

    const auto base = layout_->ctmBase;

    // Do not cancel a newer command from the player or another behavior.
    if (memory_->read<Guid>(base + layout_->ctmGuid).value_or(0) == guid && memory_->read<std::uint32_t>(base + layout_->ctmAction).value_or(0) == 7)
    {
        memory_->write<std::uint32_t>(base + layout_->ctmAction, 13); // None.
    }
}

bool WowMemoryStateProvider::supportsLua() const
{
    return !layout_->readOnly && layout_->luaDoString != 0 && layout_->d3dDevicePointer != 0 && layout_->codeCave != 0;
}

bool WowMemoryStateProvider::ensureExecutor()
{
    if (layout_->readOnly || !memory_->attached() || !isInGame())
    {
        return false;
    }

    if (!lua_)
    {
        lua_ = std::make_unique<LuaExecutor>(*memory_, *layout_);
    }

    if (lua_->installed())
    {
        return true;
    }

    if (!lua_->install())
    {
        log_.warn(logcat::Game, "cannot install the Lua executor: {}", lua_->lastError());
        return false;
    }

    log_.info(logcat::Game, "Lua executor installed: cave 0x{:X}, stub 0x{:X}, EndScene 0x{:X}", lua_->caveAddress(), lua_->stubAddress(), lua_->originalEndScene());
    return true;
}

bool WowMemoryStateProvider::executeLua(std::string_view script)
{
    if (!supportsLua() || !ensureExecutor())
    {
        return false;
    }

    if (!lua_->execute(script))
    {
        log_.warn(logcat::Game, "Lua execution failed: {}", lua_->lastError());
        return false;
    }

    log_.debug(logcat::Game, "queued Lua: {}", script);
    return true;
}

bool WowMemoryStateProvider::luaIdle() const
{
    return !lua_ || !lua_->installed() || lua_->idle();
}

bool WowMemoryStateProvider::clickToMove(Guid guid, const Vec3& position, std::uint32_t action)
{
    const ClientLayout& L = *layout_;

    if (L.readOnly || !memory_->attached() || L.ctmBase == 0 || guid == 0 || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
    {
        return false;
    }

    // 1. Ensure CTM is enabled in memory
    const auto ptr = memory_->readPointer(L.ctmActivatePointer).value_or(0);

    if (ptr)
    {
        if (!memory_->write<std::uint32_t>(ptr + L.ctmActivateOffset, 1))
        {
            return false;
        }
    }

    // 2. Write CTM fields
    if (!memory_->write<float>(L.ctmBase + L.ctmDistance, 1.5f) || !memory_->write<std::uint64_t>(L.ctmBase + L.ctmGuid, guid) || !memory_->write<float>(L.ctmBase + L.ctmX, position.x) || !memory_->write<float>(L.ctmBase + L.ctmY, position.y) || !memory_->write<float>(L.ctmBase + L.ctmZ, position.z))
    {
        return false;
    }

    // Trigger only after every parameter was written successfully.
    return memory_->write<std::uint32_t>(L.ctmBase + L.ctmAction, action);
}

const FactionTemplate* WowMemoryStateProvider::getFactionTemplate(std::uint32_t templateId) const
{
    if (templateId == 0 || layout_->factionTemplateStore == 0)
    {
        return nullptr;
    }

    auto it = factionTemplateCache_.find(templateId);

    if (it != factionTemplateCache_.end())
    {
        return &it->second;
    }

    const auto minId = memory_->read<std::uint32_t>(layout_->factionTemplateStore + 0x10).value_or(1);
    const auto maxId = memory_->read<std::uint32_t>(layout_->factionTemplateStore + 0x0C).value_or(0);

    if (templateId < minId || templateId > maxId)
    {
        return nullptr;
    }

    const auto idIndexTable = memory_->read<std::uint32_t>(layout_->factionTemplateStore + 0x20).value_or(0);

    if (idIndexTable == 0)
    {
        return nullptr;
    }

    const auto rowPtr = memory_->read<std::uint32_t>(idIndexTable + (templateId - minId) * 4).value_or(0);

    if (rowPtr == 0)
    {
        return nullptr;
    }

    std::array<std::uint32_t, 14> fields{};

    if (!memory_->read(rowPtr, fields.data(), sizeof(fields)))
    {
        return nullptr;
    }

    FactionTemplate rec;
    rec.id = fields[0];
    rec.faction = fields[1];
    rec.flags = fields[2];
    rec.factionGroupMask = fields[3];
    rec.friendGroupMask = fields[4];
    rec.enemyGroupMask = fields[5];
    rec.enemyFaction = {fields[6], fields[7], fields[8], fields[9]};
    rec.friendFaction = {fields[10], fields[11], fields[12], fields[13]};

    auto [emplaced, _] = factionTemplateCache_.emplace(templateId, rec);
    return &emplaced->second;
}

Reaction WowMemoryStateProvider::calculateReaction(std::uint32_t playerTemplateId, std::uint32_t targetTemplateId) const
{
    return reactionBetween(playerTemplateId, getFactionTemplate(playerTemplateId), targetTemplateId, getFactionTemplate(targetTemplateId));
}

} // namespace og::wowclient
