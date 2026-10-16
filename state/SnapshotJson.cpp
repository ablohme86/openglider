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
 *  File: SnapshotJson.cpp
 *
 *  Desc:
 *  Implementation of SnapshotJson (game state representations,
 *  snapshots, and state providers).
 *
 *
 */
#include "og/state/SnapshotJson.h"

#include <charconv>

namespace og
{

namespace
{

// Shortest decimal that converts back to exactly the same float, stored as a
// double. Keeps replay files compact *and* bit-exact, so replays make the
// same decisions as the live run.
double exactFloat(float v)
{
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    double d = 0.0;
    std::from_chars(buf, res.ptr, d);
    return d;
}

void writeUnitFields(Json& j, const UnitState& u)
{
    j["guid"] = guidToJson(u.guid);

    if (u.entry)
    {
        j["entry"] = u.entry;
    }

    j["name"] = u.name;
    j["level"] = u.level;
    j["health"] = u.health;
    j["maxHealth"] = u.maxHealth;
    j["powerType"] = static_cast<int>(u.powerType);
    j["power"] = u.power;
    j["maxPower"] = u.maxPower;
    j["pos"] = toJson(u.position);
    j["heading"] = exactFloat(u.heading);
    j["reaction"] = std::string(toString(u.reaction));

    if (u.classification != Classification::Normal)
    {
        j["class"] = std::string(toString(u.classification));
    }

    if (u.isPlayer)
    {
        j["isPlayer"] = true;
    }

    if (u.race)
    {
        j["race"] = static_cast<std::int64_t>(u.race);
    }

    if (u.factionTemplate)
    {
        j["factionTemplate"] = static_cast<std::int64_t>(u.factionTemplate);
    }

    if (u.factionOverride != Faction::Unknown)
    {
        j["factionOverride"] = static_cast<int>(u.factionOverride);
    }

    if (u.isDead)
    {
        j["dead"] = true;
    }

    if (u.inCombat)
    {
        j["combat"] = true;
    }

    if (u.isCasting)
    {
        j["casting"] = true;
    }

    if (u.isLootable)
    {
        j["lootable"] = true;
    }

    if (u.isSkinnable)
    {
        j["skinnable"] = true;
    }

    if (u.isTappedByOther)
    {
        j["tappedByOther"] = true;
    }

    if (u.isCritter)
    {
        j["critter"] = true;
    }

    if (!u.isAttackable)
    {
        j["notAttackable"] = true;
    }

    if (u.targetGuid)
    {
        j["target"] = guidToJson(u.targetGuid);
    }

    if (u.ownerGuid)
    {
        j["owner"] = guidToJson(u.ownerGuid);
    }

    if (!u.auras.empty())
    {
        Json auras = Json::array();

        for (const auto& a : u.auras)
        {
            auras.push_back(toJson(a));
        }

        j["auras"] = std::move(auras);
    }
}

void readUnitFields(const Json& j, UnitState& u)
{
    u.guid = guidFromJson(j["guid"]);
    u.entry = static_cast<std::uint32_t>(j["entry"].asInt());
    u.name = j["name"].asString();
    u.level = static_cast<int>(j["level"].asInt());
    u.health = static_cast<int>(j["health"].asInt());
    u.maxHealth = static_cast<int>(j["maxHealth"].asInt());
    u.powerType = static_cast<PowerType>(j["powerType"].asInt());
    u.power = static_cast<int>(j["power"].asInt());
    u.maxPower = static_cast<int>(j["maxPower"].asInt());
    u.position = vec3FromJson(j["pos"]);
    u.heading = j["heading"].asFloat();
    u.reaction = parseReaction(j["reaction"].asString());
    u.classification = parseClassification(j["class"].asString());
    u.isPlayer = j["isPlayer"].asBool();
    u.race = static_cast<int>(j["race"].asInt());
    u.factionTemplate = static_cast<std::uint32_t>(j["factionTemplate"].asInt());
    u.factionOverride = static_cast<Faction>(j["factionOverride"].asInt());
    u.isDead = j["dead"].asBool();
    u.inCombat = j["combat"].asBool();
    u.isCasting = j["casting"].asBool();
    u.isLootable = j["lootable"].asBool();
    u.isSkinnable = j["skinnable"].asBool();
    u.isTappedByOther = j["tappedByOther"].asBool();
    u.isCritter = j["critter"].asBool();
    u.isAttackable = !j["notAttackable"].asBool();
    u.targetGuid = guidFromJson(j["target"]);
    u.ownerGuid = guidFromJson(j["owner"]);

    for (const auto& a : j["auras"].asArray())
    {
        u.auras.push_back(auraFromJson(a));
    }
}

} // namespace

Json guidToJson(Guid guid)
{
    return Json(formatGuid(guid));
}

Guid guidFromJson(const Json& j)
{
    if (j.isInt())
    {
        return static_cast<Guid>(j.asInt());
    }

    const std::string& s = j.asString();

    if (s.empty())
    {
        return 0;
    }

    std::string_view v = s;
    int base = 10;

    if (v.starts_with("0x") || v.starts_with("0X"))
    {
        v.remove_prefix(2);
        base = 16;
    }

    Guid g = 0;
    std::from_chars(v.data(), v.data() + v.size(), g, base);
    return g;
}

Json toJson(const Vec3& v)
{
    return Json(Json::Array{exactFloat(v.x), exactFloat(v.y), exactFloat(v.z)});
}

Vec3 vec3FromJson(const Json& j)
{
    if (j.isObject())
    {
        return {j["x"].asFloat(), j["y"].asFloat(), j["z"].asFloat()};
    }

    return {j[0].asFloat(), j[1].asFloat(), j[2].asFloat()};
}

Json toJson(const Aura& a)
{
    Json j = Json::object();
    j["id"] = a.spellId;

    if (!a.name.empty())
    {
        j["name"] = a.name;
    }

    if (a.harmful)
    {
        j["harmful"] = true;
    }

    if (a.stacks != 1)
    {
        j["stacks"] = a.stacks;
    }

    if (a.caster)
    {
        j["caster"] = guidToJson(a.caster);
    }

    if (a.remaining >= 0.0f)
    {
        j["remaining"] = exactFloat(a.remaining);
    }

    return j;
}

Aura auraFromJson(const Json& j)
{
    Aura a;
    a.spellId = static_cast<std::uint32_t>(j["id"].asInt());
    a.name = j["name"].asString();
    a.harmful = j["harmful"].asBool();
    a.stacks = static_cast<int>(j["stacks"].asInt(1));
    a.caster = guidFromJson(j["caster"]);
    a.remaining = j["remaining"].asFloat(-1.0f);
    return a;
}

Json toJson(const UnitState& u)
{
    Json j = Json::object();
    writeUnitFields(j, u);
    return j;
}

UnitState unitFromJson(const Json& j)
{
    UnitState u;
    readUnitFields(j, u);
    return u;
}

Json toJson(const PlayerState& p)
{
    Json j = Json::object();
    writeUnitFields(j, p);
    j["playerClass"] = std::string(toString(p.playerClass));
    Json powers = Json::array();
    Json maxPowers = Json::array();

    for (std::size_t i = 0; i < kPowerTypeCount; ++i)
    {
        powers.push_back(p.powers[i]);
        maxPowers.push_back(p.maxPowers[i]);
    }

    j["powers"] = std::move(powers);
    j["maxPowers"] = std::move(maxPowers);

    if (p.isGhost)
    {
        j["ghost"] = true;
    }

    if (p.corpsePosition)
    {
        j["corpse"] = toJson(*p.corpsePosition);
    }

    if (p.isMoving)
    {
        j["moving"] = true;
    }

    if (p.isMoving || p.speed > 0.0f)
    {
        j["speed"] = p.speed;
    }

    if (p.isSitting)
    {
        j["sitting"] = true;
    }

    if (p.isAutoAttacking)
    {
        j["autoAttack"] = true;
    }

    if (p.isShooting)
    {
        j["shooting"] = true;
    }

    if (p.comboPoints)
    {
        j["comboPoints"] = p.comboPoints;
    }

    j["freeBagSlots"] = p.freeBagSlots;
    j["brokenItems"] = p.brokenItems;
    j["damagedItems"] = p.damagedItems;
    j["usedBagSlots"] = p.usedBagSlots;
    j["xp"] = p.xp;
    j["nextLevelXp"] = p.nextLevelXp;

    if (!p.bagItems.empty())
    {
        Json items = Json::array();

        for (const auto& item : p.bagItems)
        {
            Json ij = Json::object();
            ij["id"] = static_cast<std::int64_t>(item.itemId);

            if (!item.name.empty())
            {
                ij["name"] = item.name;
            }

            ij["count"] = item.count;
            items.push_back(std::move(ij));
        }

        j["bagItems"] = std::move(items);
    }

    return j;
}

PlayerState playerFromJson(const Json& j)
{
    PlayerState p;
    readUnitFields(j, p);
    p.playerClass = parsePlayerClass(j["playerClass"].asString());

    for (std::size_t i = 0; i < kPowerTypeCount; ++i)
    {
        p.powers[i] = static_cast<int>(j["powers"][i].asInt());
        p.maxPowers[i] = static_cast<int>(j["maxPowers"][i].asInt());
    }

    p.isGhost = j["ghost"].asBool();

    if (j.contains("corpse"))
    {
        p.corpsePosition = vec3FromJson(j["corpse"]);
    }

    p.isMoving = j["moving"].asBool();
    p.speed = j.contains("speed") ? static_cast<float>(j["speed"].asDouble()) : (p.isMoving ? 7.0f : 0.0f);
    p.isSitting = j["sitting"].asBool();
    p.isAutoAttacking = j["autoAttack"].asBool();
    p.isShooting = j["shooting"].asBool();
    p.comboPoints = static_cast<int>(j["comboPoints"].asInt());
    p.freeBagSlots = static_cast<int>(j["freeBagSlots"].asInt(-1));
    p.brokenItems = static_cast<int>(j["brokenItems"].asInt(-1));
    p.damagedItems = static_cast<int>(j["damagedItems"].asInt(-1));
    p.usedBagSlots = static_cast<int>(j["usedBagSlots"].asInt(-1));
    p.xp = static_cast<int>(j["xp"].asInt());
    p.nextLevelXp = static_cast<int>(j["nextLevelXp"].asInt());
    const Json& items = j["bagItems"];

    for (std::size_t i = 0; i < items.size(); ++i)
    {
        ItemStack item;
        item.itemId = static_cast<std::uint32_t>(items[i]["id"].asInt());
        item.name = items[i]["name"].asString();
        item.count = static_cast<int>(items[i]["count"].asInt());
        p.bagItems.push_back(std::move(item));
    }

    return p;
}

Json toJson(const GameObjectState& o)
{
    Json j = Json::object();
    j["guid"] = guidToJson(o.guid);

    if (o.entry)
    {
        j["entry"] = o.entry;
    }

    j["name"] = o.name;
    j["kind"] = std::string(toString(o.kind));
    j["pos"] = toJson(o.position);

    if (o.createdBy)
    {
        j["createdBy"] = guidToJson(o.createdBy);
    }

    if (o.isBobbing)
    {
        j["bobbing"] = true;
    }

    if (!o.canInteract)
    {
        j["canInteract"] = false;
    }

    return j;
}

GameObjectState objectFromJson(const Json& j)
{
    GameObjectState o;
    o.guid = guidFromJson(j["guid"]);
    o.entry = static_cast<std::uint32_t>(j["entry"].asInt());
    o.name = j["name"].asString();
    o.kind = parseGameObjectKind(j["kind"].asString());
    o.position = vec3FromJson(j["pos"]);
    o.createdBy = guidFromJson(j["createdBy"]);
    o.isBobbing = j["bobbing"].asBool();
    o.canInteract = j["canInteract"].asBool(true);
    return o;
}

Json toJson(const GameSnapshot& s)
{
    Json j = Json::object();
    j["t"] = toMillis(s.timestamp);

    if (s.mapId)
    {
        j["map"] = *s.mapId;
    }

    j["zone"] = s.zoneId;
    j["player"] = toJson(s.player);

    if (s.targetGuid)
    {
        j["target"] = guidToJson(s.targetGuid);
    }

    Json units = Json::array();

    for (const auto& u : s.units)
    {
        units.push_back(toJson(u));
    }

    j["units"] = std::move(units);
    Json objects = Json::array();

    for (const auto& o : s.objects)
    {
        objects.push_back(toJson(o));
    }

    j["objects"] = std::move(objects);

    if (s.fishing.hasBobber || s.fishing.biteDetected || s.fishing.biteKnown)
    {
        Json f = Json::object();
        f["biteKnown"] = s.fishing.biteKnown;
        f["hasBobber"] = s.fishing.hasBobber;
        f["bite"] = s.fishing.biteDetected;
        f["bobber"] = guidToJson(s.fishing.bobberGuid);
        j["fishing"] = std::move(f);
    }

    return j;
}

GameSnapshot snapshotFromJson(const Json& j)
{
    GameSnapshot s;
    s.timestamp = fromMillis(j["t"].asInt());

    if (const Json* m = j.find("map"))
    {
        s.mapId = static_cast<std::uint32_t>(m->asInt());
    }

    s.zoneId = static_cast<std::uint32_t>(j["zone"].asInt());
    s.player = playerFromJson(j["player"]);

    s.targetGuid = guidFromJson(j["target"]);

    for (const auto& u : j["units"].asArray())
    {
        s.units.push_back(unitFromJson(u));
    }

    for (const auto& o : j["objects"].asArray())
    {
        s.objects.push_back(objectFromJson(o));
    }

    if (const Json* f = j.find("fishing"))
    {
        s.fishing.biteKnown = (*f)["biteKnown"].asBool();
        s.fishing.hasBobber = (*f)["hasBobber"].asBool();
        s.fishing.biteDetected = (*f)["bite"].asBool();
        s.fishing.bobberGuid = guidFromJson((*f)["bobber"]);
    }

    return s;
}

} // namespace og
