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
 *  File: SnapshotEnvironment.cpp
 *
 *  Desc:
 *  Implementation of SnapshotEnvironment (combat execution, rotation
 *  scripting, and class behaviors).
 *
 *
 */
#include "og/combat/script/SnapshotEnvironment.h"

#include "og/state/Difficulty.h"
#include "og/state/Shapeshift.h"

#include <algorithm>
#include <charconv>
#include <format>

namespace og::script
{

namespace
{

bool contains(const std::vector<std::string_view>& list, std::string_view s)
{
    return std::find(list.begin(), list.end(), s) != list.end();
}

void expectArgs(const FunctionRef& fn, const std::vector<Value>& args, std::size_t n)
{
    if (args.size() != n)
    {
        throw RuntimeError(std::format("{}() expects {} argument(s), got {}", fn.name, n, args.size()));
    }
}

} // namespace

SnapshotEnvironment::SnapshotEnvironment(const GameSnapshot& snapshot, const IActionService* actions, const SettingsMap* settings, double combatSeconds, SpellLookup spellLookup, const std::set<std::uint32_t>* instantSpells) : snapshot_(snapshot), actions_(actions), settings_(settings), combatSeconds_(combatSeconds), spellLookup_(std::move(spellLookup)), instantSpells_(instantSpells)
{
}

const std::vector<std::string_view>& SnapshotEnvironment::objectNames()
{
    static const std::vector<std::string_view> v{"player", "target", "pet"};
    return v;
}

const std::vector<std::string_view>& SnapshotEnvironment::globalValues()
{
    static const std::vector<std::string_view> v{"adds", "attackers", "combatTime"};
    return v;
}

const std::vector<std::string_view>& SnapshotEnvironment::globalFunctions()
{
    static const std::vector<std::string_view> v{"ready", "bound", "cooldown", "enemiesNear"};
    return v;
}

const std::vector<std::string_view>& SnapshotEnvironment::unitMembers()
{
    static const std::vector<std::string_view> v{"exists", "health", "hp", "maxHp", "power", "maxPower", "powerPercent", "mana", "level", "distance", "isElite", "isPlayer", "isCasting", "isDead", "isFleeing", "inCombat", "name", "targetingMe", "isLootable", "isSkinnable", "difficulty", "isAttacking", "hasAggro"};
    return v;
}

const std::vector<std::string_view>& SnapshotEnvironment::playerMembers()
{
    static const std::vector<std::string_view> v{"rage", "energy", "focus", "runicPower", "comboPoints", "isMoving", "speed", "isSitting", "autoAttacking", "isAutoAttacking", "shooting", "isShooting", "isGhost", "freeBagSlots", "class", "waterCount", "foodCount", "conjuredWaterCount", "conjuredFoodCount", "aggroedMobsCount", "stance", "form", "isStealthed", "hasPet"};
    return v;
}

const std::vector<std::string_view>& SnapshotEnvironment::targetMembers()
{
    static const std::vector<std::string_view> v{"comboPoints"};
    return v;
}

const std::vector<std::string_view>& SnapshotEnvironment::playerMethods()
{
    static const std::vector<std::string_view> v{"itemCount", "inventorySearch", "hasSpell"};
    return v;
}

const std::vector<std::string_view>& SnapshotEnvironment::constantGroups()
{
    static const std::vector<std::string_view> v{"Stance", "Form", "Difficulty"};
    return v;
}

std::vector<std::string_view> SnapshotEnvironment::constantsOf(std::string_view group)
{
    // What target.difficulty reads, easiest first: the level's colour, or Elite.
    static constexpr std::string_view difficulties[] = {"Gray", "Green", "Yellow", "Orange", "Red", "Elite"};
    const auto names = group == "Stance" ? stanceNames() : group == "Form" ? formNames() : group == "Difficulty" ? std::span<const std::string_view>{difficulties} : std::span<const std::string_view>{};
    return {names.begin(), names.end()};
}

const std::vector<std::string_view>& SnapshotEnvironment::unitMethods()
{
    static const std::vector<std::string_view> v{"hasBuff", "hasDebuff", "hasAura", "buffStacks", "debuffStacks", "hasMyDebuff", "auraRemaining"};
    return v;
}

const UnitState* SnapshotEnvironment::resolve(const ObjectRef& o) const
{
    if (o.name == "player")
    {
        return &snapshot_.player;
    }

    if (o.name == "target")
    {
        return snapshot_.target();
    }

    if (o.name == "pet")
    {
        return snapshot_.pet();
    }

    return nullptr;
}

bool SnapshotEnvironment::exists(const ObjectRef& o)
{
    const UnitState* u = resolve(o);
    return u != nullptr && u->guid != 0;
}

std::optional<Value> SnapshotEnvironment::name(std::string_view n)
{
    if (contains(objectNames(), n) || contains(constantGroups(), n))
    {
        return ObjectRef{std::string(n)};
    }

    if (n == "adds")
    {
        const auto attackers = snapshot_.attackers();
        const auto others = std::count_if(attackers.begin(), attackers.end(), [&](const UnitState* u) { return u->guid != snapshot_.targetGuid; });
        return static_cast<double>(others);
    }

    if (n == "attackers")
    {
        return static_cast<double>(snapshot_.attackers().size());
    }

    if (n == "combatTime")
    {
        return combatSeconds_;
    }

    if (contains(globalFunctions(), n))
    {
        return FunctionRef{"", std::string(n)};
    }

    if (settings_)
    {
        if (auto it = settings_->find(n); it != settings_->end())
        {
            return it->second;
        }
    }

    return std::nullopt;
}

std::optional<Value> SnapshotEnvironment::unitMember(const UnitState* u, std::string_view m) const
{
    // Missing unit (e.g. no target): every property has a neutral default.
    if (m == "exists")
    {
        return u != nullptr && u->guid != 0;
    }

    if (m == "name")
    {
        return u ? u->name : std::string();
    }

    // An elite reads Difficulty.Elite, anything else the colour the client
    // gives its level against the player's; compared as numbers, easiest first
    // (Gray 0, Green, Yellow, Orange, Red 4, Elite 5). No unit: -1.
    if (m == "difficulty")
    {
        if (!u)
        {
            return -1.0;
        }

        if (u->isElite())
        {
            return 5.0;
        }

        switch (difficultyOf(u->level, snapshot_.player.level))
        {
        case Difficulty::Gray:
            return 0.0;
        case Difficulty::Green:
            return 1.0;
        case Difficulty::Yellow:
            return 2.0;
        case Difficulty::Orange:
            return 3.0;
        case Difficulty::Red:
            return 4.0;
        }

        return 0.0;
    }

    if (!contains(unitMembers(), m))
    {
        return std::nullopt;
    }

    if (!u)
    {
        if (m == "isElite" || m == "isPlayer" || m == "isCasting" || m == "isDead" || m == "inCombat" || m == "targetingMe" || m == "isLootable" || m == "isSkinnable" || m == "isFleeing" || m == "isAttacking" || m == "hasAggro")
        {
            return false;
        }

        return 0.0;
    }

    if (m == "health")
    {
        return static_cast<double>(u->healthPercent());
    }

    if (m == "hp")
    {
        return static_cast<double>(u->health);
    }

    if (m == "maxHp")
    {
        return static_cast<double>(u->maxHealth);
    }

    if (m == "power")
    {
        return static_cast<double>(u->power);
    }

    if (m == "maxPower")
    {
        return static_cast<double>(u->maxPower);
    }

    if (m == "powerPercent")
    {
        return static_cast<double>(u->powerPercent());
    }

    if (m == "mana")
    {
        return u->powerType == PowerType::Mana ? static_cast<double>(u->powerPercent()) : 0.0;
    }

    if (m == "level")
    {
        return static_cast<double>(u->level);
    }

    if (m == "distance")
    {
        // Combat range is measured across the ground plane. Including Z made
        // a target directly beside the player appear out of melee range on
        // slopes or when the client reports different model heights.
        return static_cast<double>(distance2D(snapshot_.player.position, u->position));
    }

    if (m == "isElite")
    {
        return u->isElite();
    }

    if (m == "isPlayer")
    {
        return u->isPlayer;
    }

    if (m == "isCasting")
    {
        return u->isCasting && !(instantSpells_ && u->castingSpell && instantSpells_->contains(u->castingSpell));
    }

    if (m == "isSitting")
    {
        return u->isSitting;
    }

    if (m == "isDead")
    {
        return !u->isAlive();
    }

    if (m == "inCombat")
    {
        // The player: the client's combat flag, or aggro without it yet - a
        // mob that has the character (or its pet) as its target and is
        // coming, before the flag is up.
        if (u == &snapshot_.player)
        {
            return u->inCombat || std::any_of(snapshot_.units.begin(), snapshot_.units.end(), [&](const UnitState& o) {
                       return o.isAlive() && !o.isCritter && o.reaction != Reaction::Friendly && (snapshot_.isAttacker(o) || o.targetGuid == u->guid);
                   });
        }

        return u->inCombat;
    }

    // Fighting something: in combat, with a living enemy as its target - the
    // pet sent in, or a mob on the player or the pet.
    if (m == "isAttacking")
    {
        if (!u->inCombat || u->targetGuid == 0)
        {
            return false;
        }

        if (u->targetGuid == snapshot_.player.guid)
        {
            return true;
        }

        const UnitState* t = snapshot_.findUnit(u->targetGuid);
        return t && t->isAlive() && (t->reaction != Reaction::Friendly || t->ownerGuid == snapshot_.player.guid);
    }

    // A mob has it as its target: the pet holding aggro (a Voidwalker
    // tanking), or the character with one on it.
    if (m == "hasAggro")
    {
        return std::any_of(snapshot_.units.begin(), snapshot_.units.end(), [&](const UnitState& o) {
            return o.guid != u->guid && o.isAlive() && !o.isPlayer && !o.isCritter && o.ownerGuid == 0 && o.reaction != Reaction::Friendly && o.targetGuid != 0 && o.targetGuid == u->guid;
        });
    }

    if (m == "targetingMe")
    {
        return u->targetGuid != 0 && u->targetGuid == snapshot_.player.guid;
    }

    if (m == "isLootable")
    {
        return u->isLootable;
    }

    if (m == "isSkinnable")
    {
        return u->isSkinnable;
    }

    if (m == "isFleeing")
    {
        return u->isFleeing;
    }

    return std::nullopt;
}

std::optional<Value> SnapshotEnvironment::playerMember(std::string_view m) const
{
    const PlayerState& p = snapshot_.player;

    if (m == "mana")
    {
        return static_cast<double>(p.manaPercent());
    }

    if (m == "rage")
    {
        return static_cast<double>(p.rage());
    }

    if (m == "energy")
    {
        return static_cast<double>(p.energy());
    }

    if (m == "focus")
    {
        return static_cast<double>(p.focus());
    }

    if (m == "runicPower")
    {
        return static_cast<double>(p.runicPower());
    }

    if (m == "comboPoints")
    {
        return static_cast<double>(p.comboPoints);
    }

    if (m == "isMoving")
    {
        return p.isMoving;
    }

    if (m == "speed")
    {
        return static_cast<double>(p.speed);
    }

    if (m == "isSitting")
    {
        return p.isSitting;
    }

    if (m == "autoAttacking" || m == "isAutoAttacking")
    {
        return p.isAutoAttacking;
    }

    if (m == "shooting" || m == "isShooting")
    {
        return p.isShooting;
    }

    if (m == "isGhost")
    {
        return p.isGhost;
    }

    if (m == "freeBagSlots")
    {
        return static_cast<double>(p.freeBagSlots);
    }

    if (m == "class")
    {
        return std::string(toString(p.playerClass));
    }

    if (m == "waterCount")
    {
        return static_cast<double>(p.waterCount());
    }

    if (m == "foodCount")
    {
        return static_cast<double>(p.foodCount());
    }

    if (m == "conjuredWaterCount")
    {
        return static_cast<double>(p.conjuredWaterCount());
    }

    if (m == "conjuredFoodCount")
    {
        return static_cast<double>(p.conjuredFoodCount());
    }

    if (m == "stance")
    {
        return std::string(toString(stanceOf(p.auras)));
    }

    if (m == "form")
    {
        return std::string(toString(formOf(p.auras)));
    }

    if (m == "isStealthed")
    {
        return og::isStealthed(p.auras);
    }

    // The character's own pet is out (alive or dead): the same as `pet.exists`.
    if (m == "hasPet")
    {
        return snapshot_.pet() != nullptr;
    }

    if (m == "aggroedMobsCount")
    {
        // Mobs fighting the character itself; `attackers` also counts those on the pet.
        const auto attackers = snapshot_.attackers();
        return static_cast<double>(std::count_if(attackers.begin(), attackers.end(), [&](const UnitState* u) { return !u->isPlayer && u->targetGuid == p.guid; }));
    }

    return unitMember(&p, m);
}

// Whether the character has learned a spell, by name (any rank) or by spell
// id. Where that cannot be told (no spell list, or a name the spell table
// lacks) it counts as learned, as a rule for such a spell is not skipped either.
bool SnapshotEnvironment::knowsSpell(std::string_view nameOrId) const
{
    const PlayerState& p = snapshot_.player;

    if (p.knownSpells.empty())
    {
        return true;
    }

    std::uint32_t id = 0;

    if (const auto [end, ec] = std::from_chars(nameOrId.data(), nameOrId.data() + nameOrId.size(), id); ec == std::errc() && end == nameOrId.data() + nameOrId.size())
    {
        return p.hasSpell(id);
    }

    if (!spellLookup_)
    {
        return true;
    }

    const auto ids = spellLookup_(nameOrId);
    return ids.empty() || std::any_of(ids.begin(), ids.end(), [&](std::uint32_t spell) { return p.hasSpell(spell); });
}

std::optional<Value> SnapshotEnvironment::member(const ObjectRef& o, std::string_view m)
{
    if (contains(constantGroups(), o.name))
    {
        // Difficulties are ordered, so a rule can say "< Difficulty.Yellow":
        // their place in the list, easiest first.
        if (o.name == "Difficulty")
        {
            const auto names = constantsOf(o.name);
            const auto it = std::find(names.begin(), names.end(), m);

            if (it != names.end())
            {
                return static_cast<double>(it - names.begin());
            }

            return std::nullopt;
        }

        if (contains(constantsOf(o.name), m))
        {
            return std::string(m);
        }

        return std::nullopt;
    }

    if (!contains(objectNames(), o.name))
    {
        return std::nullopt;
    }

    if (contains(unitMethods(), m))
    {
        return FunctionRef{o.name, std::string(m)};
    }

    if (o.name == "player" && contains(playerMethods(), m))
    {
        return FunctionRef{o.name, std::string(m)};
    }

    if (o.name == "player")
    {
        return playerMember(m);
    }

    // Combo points are built up on a target, and gone with it: the ones on
    // the target the character has now. (player.comboPoints reads the same,
    // for the scripts written with it.)
    if (o.name == "target" && m == "comboPoints")
    {
        return snapshot_.target() ? static_cast<double>(snapshot_.player.comboPoints) : 0.0;
    }

    return unitMember(resolve(o), m);
}

Value SnapshotEnvironment::call(const FunctionRef& fn, const std::vector<Value>& args)
{
    if (fn.object.empty())
    {
        if (fn.name == "ready")
        {
            expectArgs(fn, args, 1);
            return actions_ != nullptr && actions_->isReady(toStringArg(args[0]));
        }

        if (fn.name == "bound")
        {
            expectArgs(fn, args, 1);
            return actions_ != nullptr && actions_->hasBinding(toStringArg(args[0]));
        }

        if (fn.name == "cooldown")
        {
            // Coarse: 0 when ready, 1 otherwise (exact remaining time is not exposed by IActionService).
            expectArgs(fn, args, 1);
            return (actions_ != nullptr && actions_->isReady(toStringArg(args[0]))) ? 0.0 : 1.0;
        }

        if (fn.name == "enemiesNear")
        {
            expectArgs(fn, args, 1);
            const float radius = static_cast<float>(toNumber(args[0]));
            return static_cast<double>(snapshot_.hostilesNear(snapshot_.player.position, radius).size());
        }

        throw RuntimeError(std::format("unknown function '{}'", fn.name));
    }

    expectArgs(fn, args, 1);

    if (fn.object == "player" && fn.name == "itemCount")
    {
        return static_cast<double>(snapshot_.player.itemCount(toStringArg(args[0])));
    }

    if (fn.object == "player" && fn.name == "inventorySearch")
    {
        return static_cast<double>(snapshot_.player.inventorySearch(toStringArg(args[0])));
    }

    if (fn.object == "player" && fn.name == "hasSpell")
    {
        return knowsSpell(toStringArg(args[0]));
    }

    const UnitState* u = resolve(ObjectRef{fn.object});
    const std::string aura = toStringArg(args[0]);

    if (fn.name == "hasBuff")
    {
        return u != nullptr && u->hasBuff(aura);
    }

    if (fn.name == "hasDebuff")
    {
        return u != nullptr && u->hasDebuff(aura);
    }

    if (fn.name == "hasAura")
    {
        return u != nullptr && u->hasAura(aura);
    }

    if (fn.name == "buffStacks")
    {
        const Aura* a = u ? u->findAura(aura, false) : nullptr;
        return a ? static_cast<double>(a->stacks) : 0.0;
    }

    if (fn.name == "debuffStacks")
    {
        const Aura* a = u ? u->findAura(aura, true) : nullptr;
        return a ? static_cast<double>(a->stacks) : 0.0;
    }

    if (fn.name == "hasMyDebuff")
    {
        if (!u)
        {
            return false;
        }

        for (const auto& a : u->auras)
        {
            if (a.harmful && a.caster == snapshot_.player.guid && (a.name == aura || std::to_string(a.spellId) == aura))
            {
                return true;
            }
        }

        // Providers that cannot tell the caster leave it 0: fall back to any matching debuff.
        const Aura* any = u->findAura(aura, true);
        return any != nullptr && any->caster == 0;
    }

    if (fn.name == "auraRemaining")
    {
        const Aura* a = u ? u->findAura(aura) : nullptr;
        return a ? static_cast<double>(std::max(0.0f, a->remaining)) : 0.0;
    }

    throw RuntimeError(std::format("unknown method '{}.{}'", fn.object, fn.name));
}

} // namespace og::script
