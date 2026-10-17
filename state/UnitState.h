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
 *  File: UnitState.h
 *
 *  Desc:
 *  Declarations and interface for UnitState (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include "og/common/Vec3.h"
#include "og/state/Types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

// An item with `durability` of `maxDurability` left shows yellow on the armour
// figure: at a fifth or less. At 0 it is broken (red).
inline bool durabilityLow(std::uint32_t durability, std::uint32_t maxDurability)
{
    return maxDurability > 0 && durability * 5 <= maxDurability;
}

struct Aura
{
    std::uint32_t spellId = 0;
    std::string name;     // empty when the provider cannot resolve the spell name
    bool harmful = false; // debuff
    int stacks = 1;
    Guid caster = 0;
    float remaining = -1.0f; // seconds; < 0 = unknown / permanent
};

// The marker over a quest giver's head: a yellow ? (a quest to hand in), a
// yellow ! (a quest to take), a grey ? (a quest of ours it takes, not done).
enum class QuestMarker
{
    None,
    Incomplete,
    Available,
    Reward
};

struct UnitState
{
    Guid guid = 0;
    std::uint32_t entry = 0;
    std::string name;
    int level = 0;
    std::uint32_t factionTemplate = 0;
    Faction factionOverride = Faction::Unknown;

    int health = 0;
    int maxHealth = 0;
    PowerType powerType = PowerType::Mana;
    int power = 0; // current value of the unit's primary power
    int maxPower = 0;

    Vec3 position;
    float heading = 0.0f;

    Reaction reaction = Reaction::Unknown;
    Classification classification = Classification::Normal;
    bool isPlayer = false;
    PlayerClass playerClass = PlayerClass::Unknown; // players only
    int race = 0;                                   // players only: the race id, 0 unknown (see factionOfRace)
    bool isDead = false;
    bool inCombat = false;
    bool isCasting = false;
    std::uint32_t castingSpell = 0; // the spell being cast, where the client says (1.6x); 0 otherwise
    bool isSitting = false;
    bool isLootable = false;
    bool isSkinnable = false;
    bool isTappedByOther = false;
    bool isFleeing = false; // running away: feared, or a mob fleeing at low health
    bool isCritter = false;
    bool isAttackable = true; // false for vendors, quest givers, guards and other NPCs you cannot attack
    bool isVendor = false;    // sells goods (NPC flag), so the bot can unload at it
    bool canRepair = false;   // also repairs (NPC flag)
    bool isFlightMaster = false; // flies players about (NPC flag)
    // A flight master whose flight path the character has not learnt: the
    // green "!" over its head.
    bool newFlightPath = false;
    QuestMarker questMarker = QuestMarker::None;

    Guid targetGuid = 0;
    Guid ownerGuid = 0; // summoner/creator (pets, totems)

    std::vector<Aura> auras;

    float healthPercent() const
    {
        return maxHealth > 0 ? 100.0f * static_cast<float>(health) / static_cast<float>(maxHealth) : 0.0f;
    }

    float powerPercent() const
    {
        return maxPower > 0 ? 100.0f * static_cast<float>(power) / static_cast<float>(maxPower) : 0.0f;
    }

    bool isElite() const
    {
        return classification == Classification::Elite || classification == Classification::RareElite || classification == Classification::WorldBoss;
    }

    bool isAlive() const
    {
        return !isDead && health > 0;
    }

    Faction faction() const
    {
        if (!isPlayer)
        {
            return Faction::Unknown;
        }

        if (factionOverride != Faction::Unknown)
        {
            return factionOverride;
        }

        if (const auto f = factionOfTemplateId(factionTemplate); f != Faction::Unknown)
        {
            return f;
        }

        return factionOfRace(race);
    }

    // Aura lookup by name (case-insensitive) or numeric spell id ("772").
    const Aura* findAura(std::string_view nameOrId, std::optional<bool> harmful = std::nullopt) const;
    bool hasAura(std::string_view nameOrId) const
    {
        return findAura(nameOrId) != nullptr;
    }

    bool hasBuff(std::string_view nameOrId) const
    {
        return findAura(nameOrId, false) != nullptr;
    }

    bool hasDebuff(std::string_view nameOrId) const
    {
        return findAura(nameOrId, true) != nullptr;
    }

    int auraStacks(std::string_view nameOrId) const
    {
        const Aura* a = findAura(nameOrId);
        return a ? a->stacks : 0;
    }
};

// Rabbits, deer, cows, frogs and the like: faction templates 31 (Prey), 188 and
// 190 (Ambient). The ids are the same in every client version.
inline bool isCritterFaction(std::uint32_t factionTemplate)
{
    return factionTemplate == 31 || factionTemplate == 188 || factionTemplate == 190;
}

// All stacks of one item in the player's bags, summed.
struct ItemStack
{
    std::uint32_t itemId = 0;
    std::string name; // empty when the provider does not know the item's name
    int count = 0;
};

// A character skill (e.g. Herbalism, Mining, Weapon skills).
inline constexpr std::uint32_t kSkillHerbalism = 182;
inline constexpr std::uint32_t kSkillMining = 186;
inline constexpr std::uint32_t kSkillSkinning = 393;
inline constexpr std::uint32_t kSkillFishing = 356;

struct PlayerSkill
{
    std::uint16_t id = 0;
    std::uint16_t rank = 0;
    std::uint16_t maxRank = 0;
    std::int16_t tempBonus = 0;
    std::int16_t permBonus = 0;

    int total() const
    {
        return static_cast<int>(rank) + tempBonus + permBonus;
    }
};

// Drinks and food the bot recognises: by item id, or by name for what it has no id for.
bool isWaterItem(std::uint32_t itemId, std::string_view name = {});
bool isFoodItem(std::uint32_t itemId, std::string_view name = {});
// Healing potions and healthstones; mana potions. What Player.HealthPotion and
// Player.ManaPotion press when one sits on an action bar.
bool isHealthPotionItem(std::uint32_t itemId, std::string_view name = {});
bool isManaPotionItem(std::uint32_t itemId, std::string_view name = {});
// What a mage conjures: every rank, and Conjured Refreshment, which is both.
bool isConjuredWater(std::uint32_t itemId);
bool isConjuredFood(std::uint32_t itemId);

struct PlayerState : UnitState
{
    PlayerState()
    {
        isPlayer = true;
    }

    std::array<int, kPowerTypeCount> powers{};
    std::array<int, kPowerTypeCount> maxPowers{};

    bool isGhost = false;
    std::optional<Vec3> corpsePosition;

    bool isMoving = false;
    float speed = 0.0f; // yards/s (observed 2D movement speed)
    bool isAutoAttacking = false;
    bool isShooting = false; // an auto-repeat spell (wand Shoot, Auto Shot) is on
    int comboPoints = 0;
    int freeBagSlots = -1; // -1 = unknown
    // Bag slots holding something, in every bag (a quiver's too), -1 = unknown.
    int usedBagSlots = -1;
    int brokenItems = -1; // equipped items at zero durability, -1 = unknown
    // Equipped items low enough to show yellow on the armour figure (or red,
    // broken ones count too), -1 = unknown.
    int damagedItems = -1;
    std::int64_t money = -1; // carried copper, -1 = unknown
    int xp = 0;
    int nextLevelXp = 0;
    std::vector<ItemStack> bagItems; // backpack + bags
    // The ids of the quests in the quest log; empty when the client does not tell.
    std::vector<std::uint32_t> questLog;
    // The flight points (TaxiNodes ids) the character has learnt, lowest
    // first; nullopt when the client does not tell.
    std::optional<std::vector<std::uint32_t>> knownTaxiNodes;
    // What has been killed for each quest in the log, objective by objective
    // (the order the quest lists its kill objectives in); items are counted in the bags.
    struct QuestProgress
    {
        std::uint32_t questId = 0;
        std::array<std::uint16_t, 8> kills{};
    };
    std::vector<QuestProgress> questProgress;
    // How many of a quest objective the game asks for, by lower-case name
    // ("half-eaten fish" 7), as its UI showed it: where the client tells,
    // over the quest data's count.
    std::map<std::string, int> objectiveWanted;
    // The breath bar is shown: the character is under water and its breath
    // runs out. nullopt where the client's UI cannot be read.
    std::optional<bool> breathDraining;
    // The quest tracker's lines, by lower-case quest title: where the client
    // tells, a quest with a line not yet done is not done, whatever the
    // quest data has for it.
    struct TrackedObjective
    {
        std::string name;
        int have = 0;
        int want = 0;
        // In the tracker now; false for the last lines of a quest no longer
        // shown there, whose counts may be old.
        bool shown = true;
    };
    std::map<std::string, std::vector<TrackedObjective>> trackedQuests;
    // The main hand's temporary enchant (a lure, a poison): nullopt when the
    // client does not tell, 0 when there is none.
    std::optional<std::uint32_t> mainHandTempEnchant;
    int mainHandTempEnchantSecondsLeft = 0;

    // Items in the bags by name (case-insensitive) or numeric item id ("5350").
    int itemCount(std::string_view nameOrId) const;
    // Items whose name holds the text anywhere ("Water" counts Conjured Water and Refreshing Spring Water), or by item id.
    int inventorySearch(std::string_view partOrId) const;
    int waterCount() const;
    int foodCount() const;
    // Only what a mage conjures, so bought drinks do not stop Conjure Water.
    int conjuredWaterCount() const;
    int conjuredFoodCount() const;

    int powerOf(PowerType t) const
    {
        return powers[static_cast<std::size_t>(t)];
    }

    int maxPowerOf(PowerType t) const
    {
        return maxPowers[static_cast<std::size_t>(t)];
    }

    float powerPercentOf(PowerType t) const
    {
        const int m = maxPowerOf(t);
        return m > 0 ? 100.0f * static_cast<float>(powerOf(t)) / static_cast<float>(m) : 0.0f;
    }

    void setPower(PowerType t, int value, int maxValue)
    {
        powers[static_cast<std::size_t>(t)] = value;
        maxPowers[static_cast<std::size_t>(t)] = maxValue;
    }

    int mana() const
    {
        return powerOf(PowerType::Mana);
    }

    float manaPercent() const
    {
        return powerPercentOf(PowerType::Mana);
    }

    int rage() const
    {
        return powerOf(PowerType::Rage);
    }

    int energy() const
    {
        return powerOf(PowerType::Energy);
    }

    int focus() const
    {
        return powerOf(PowerType::Focus);
    }

    int runicPower() const
    {
        return powerOf(PowerType::RunicPower);
    }

    bool isDeadOrGhost() const
    {
        return isDead || isGhost;
    }

    std::vector<std::uint32_t> knownSpells;
    bool hasSpell(std::uint32_t spellId) const
    {
        return std::find(knownSpells.begin(), knownSpells.end(), spellId) != knownSpells.end();
    }

    std::vector<PlayerSkill> skills;
    int skillRank(std::uint32_t skillId) const
    {
        for (const auto& s : skills)
        {
            if (s.id == skillId)
            {
                return s.total();
            }
        }

        return 0;
    }

    bool hasSkill(std::uint32_t skillId) const
    {
        for (const auto& s : skills)
        {
            if (s.id == skillId)
            {
                return true;
            }
        }

        return false;
    }

    void setSkill(std::uint32_t skillId, int rank, int maxRank = 0, int bonus = 0)
    {
        for (auto& s : skills)
        {
            if (s.id == skillId)
            {
                s.rank = static_cast<std::uint16_t>(rank);
                s.maxRank = static_cast<std::uint16_t>(maxRank);
                s.tempBonus = static_cast<std::int16_t>(bonus);
                return;
            }
        }

        PlayerSkill s;
        s.id = static_cast<std::uint16_t>(skillId);
        s.rank = static_cast<std::uint16_t>(rank);
        s.maxRank = static_cast<std::uint16_t>(maxRank);
        s.tempBonus = static_cast<std::int16_t>(bonus);
        skills.push_back(s);
    }

    int herbalismSkill() const
    {
        return skillRank(182);
    }

    int miningSkill() const
    {
        return skillRank(186);
    }

    int skinningSkill() const
    {
        return skillRank(kSkillSkinning);
    }

    int fishingSkill() const
    {
        return skillRank(kSkillFishing);
    }
};

struct GameObjectState
{
    Guid guid = 0;
    std::uint32_t entry = 0;
    std::string name;
    GameObjectKind kind = GameObjectKind::Unknown;
    Vec3 position;
    Guid createdBy = 0;     // e.g. owner of a fishing bobber
    bool isBobbing = false; // fishing bobber "bite" animation
    bool canInteract = true;
};

struct FishingState
{
    // The client layout can tell a bite. Without it the bot cannot fish.
    bool biteKnown = false;
    bool hasBobber = false;
    bool biteDetected = false;
    Guid bobberGuid = 0;
};

} // namespace og
