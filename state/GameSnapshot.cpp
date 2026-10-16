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
 *  File: GameSnapshot.cpp
 *
 *  Desc:
 *  Implementation of GameSnapshot (game state representations,
 *  snapshots, and state providers).
 *
 *
 */
#include "og/state/GameSnapshot.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace og
{

namespace
{

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

bool icontains(std::string_view text, std::string_view part)
{
    return !std::ranges::search(text, part, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); }).empty();
}

std::optional<std::uint32_t> parseSpellId(std::string_view s)
{
    std::uint32_t id = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), id);

    if (ec == std::errc() && ptr == s.data() + s.size())
    {
        return id;
    }

    return std::nullopt;
}

} // namespace

bool isConjuredWater(std::uint32_t id)
{
    switch (id)
    {
    // Conjured Water ranks 1-9
    case 5350:
    case 2288:
    case 2136:
    case 3772:
    case 8077:
    case 8078:
    case 8079:
    case 30703:
    case 22018:
    // Conjured Refreshment (food and drink)
    case 34062:
    case 43518:
    case 43523:
        return true;
    default:
        return false;
    }
}

bool isConjuredFood(std::uint32_t id)
{
    switch (id)
    {
    // Conjured Food ranks 1-8
    case 5349:
    case 1113:
    case 1114:
    case 1487:
    case 8075:
    case 8076:
    case 22895:
    case 22019:
    // Conjured Refreshment (food and drink)
    case 34062:
    case 43518:
    case 43523:
        return true;
    default:
        return false;
    }
}

bool isWaterItem(std::uint32_t id, std::string_view name)
{
    if (isConjuredWater(id))
    {
        return true;
    }

    switch (id)
    {
    // Common vendor waters
    case 159:
    case 1179:
    case 1205:
    case 1708:
    case 1645:
    case 8766:
        return true;
    default:
        break;
    }

    return name.find("Water") != std::string_view::npos || name.find("water") != std::string_view::npos;
}

bool isHealthPotionItem(std::uint32_t id, std::string_view name)
{
    switch (id)
    {
    // Minor, Lesser, (plain), Greater, Superior, Major Healing Potion; Combat,
    // Super, Runic and Resurgent Healing Potion
    case 118:
    case 858:
    case 929:
    case 1710:
    case 3928:
    case 13446:
    case 18839:
    case 22829:
    case 33447:
    case 33092:
    // Minor, Lesser, (plain), Greater and Major Healthstone
    case 5512:
    case 5511:
    case 5509:
    case 5510:
    case 9421:
        return true;
    default:
        break;
    }

    return name.find("Healing Potion") != std::string_view::npos || name.find("Healthstone") != std::string_view::npos;
}

bool isManaPotionItem(std::uint32_t id, std::string_view name)
{
    switch (id)
    {
    // Minor, Lesser, (plain), Greater, Superior, Major Mana Potion; Combat,
    // Super and Runic Mana Potion
    case 2455:
    case 3385:
    case 3827:
    case 6149:
    case 13443:
    case 13444:
    case 18841:
    case 22832:
    case 33448:
        return true;
    default:
        break;
    }

    return name.find("Mana Potion") != std::string_view::npos;
}

bool isFoodItem(std::uint32_t id, std::string_view name)
{
    if (isConjuredFood(id))
    {
        return true;
    }

    switch (id)
    {
    // Common vendor foods
    case 117:
    case 4540:
    case 4541:
    case 4542:
    case 4544:
    case 8952:
    // Vendor meat, bread, cheese, fruit, mushrooms and fish sold since 1.x
    case 2287:
    case 3770:
    case 3771:
    case 4599:
    case 4601:
    case 8950:
    case 414:
    case 422:
    case 1707:
    case 2070:
    case 3927:
    case 8932:
    case 4536:
    case 4537:
    case 4538:
    case 4539:
    case 4602:
    case 8953:
    case 4604:
    case 4605:
    case 4606:
    case 4607:
    case 4608:
    case 8948:
    case 787:
    case 4592:
    case 4593:
    case 4594:
    case 8957:
    case 21552:
    case 16166:
    case 16167:
    case 16168:
    case 16169:
    case 16170:
    case 16171:
    // Raw fish that can be eaten as caught: Brilliant Smallfish, Longjaw Mud
    // Snapper, Slitherskin Mackerel, Bristle Whisker Catfish, Loch Frenzy,
    // Rainbow Fin Albacore, Rockscale Cod, Spotted Yellowtail, Mithril Head
    // Trout, Glossy Mightfish, Summer Bass, Redgill, Nightfin Snapper, Sunscale
    // and Whitescale Salmon. Clients that give no item names only have the id.
    case 6291:
    case 6289:
    case 6303:
    case 6308:
    case 6317:
    case 6361:
    case 6362:
    case 4603:
    case 8365:
    case 13754:
    case 13756:
    case 13758:
    case 13759:
    case 13760:
    case 13889:
    // Cooked fish
    case 6290:
    case 5095:
    case 6316:
    case 6887:
    case 8364:
    case 13927:
    case 13928:
    case 13929:
    case 13930:
    case 13931:
    case 13932:
    case 13933:
    case 13934:
    case 13935:
    // Cooked meat, stews and the like
    case 724:
    case 733:
    case 1017:
    case 2680:
    case 2681:
    case 2683:
    case 2684:
    case 2687:
    case 2888:
    case 3220:
    case 3662:
    case 3663:
    case 3664:
    case 3665:
    case 3666:
    case 3726:
    case 3727:
    case 3728:
    case 3729:
    case 4457:
    case 5472:
    case 5474:
    case 5476:
    case 5477:
    case 5478:
    case 5479:
    case 5480:
    case 5525:
    case 5526:
    case 5527:
    case 6038:
    case 6888:
    case 6890:
    case 12209:
    case 12210:
    case 12212:
    case 12213:
    case 12214:
    case 12215:
    case 12216:
    case 12218:
    case 12224:
    case 13851:
    case 17222:
    case 18045:
    // Quest rewards, drops and the rest that rests a character, by name in
    // QuestieDB's items (Cactus Apple Surprise was not eaten from a bar)
    case 961:
    case 1082:
    case 1326:
    case 2679:
    case 2682:
    case 2685:
    case 4656:
    case 5057:
    case 5066:
    case 5473:
    case 6299:
    case 6657:
    case 6807:
    case 7097:
    case 7228:
    case 11415:
    case 11444:
    case 11584:
    case 11950:
    case 12238:
    case 13546:
    case 13724:
    case 13755:
    case 16766:
    case 16971:
    case 17119:
    case 17197:
    case 18000:
    case 18001:
    case 18254:
    case 19304:
    case 19696:
    case 20074:
    case 20452:
    case 20516:
    case 21030:
    case 21072:
    case 21215:
    case 21217:
        return true;
    default:
        break;
    }

    return name.find("Bread") != std::string_view::npos || name.find("Muffin") != std::string_view::npos || name.find("Rye") != std::string_view::npos || name.find("Pumpernickel") != std::string_view::npos || name.find("Sourdough") != std::string_view::npos || name.find("Sweet Roll") != std::string_view::npos || name.find("Cinnamon Roll") != std::string_view::npos || name.find("Croissant") != std::string_view::npos;
}

const Aura* UnitState::findAura(std::string_view nameOrId, std::optional<bool> harmful) const
{
    const auto id = parseSpellId(nameOrId);

    for (const auto& a : auras)
    {
        if (harmful && a.harmful != *harmful)
        {
            continue;
        }

        if (id ? a.spellId == *id : iequals(a.name, nameOrId))
        {
            return &a;
        }
    }

    return nullptr;
}

int PlayerState::waterCount() const
{
    int total = 0;

    for (const auto& item : bagItems)
    {
        if (isWaterItem(item.itemId, item.name))
        {
            total += item.count;
        }
    }

    return total;
}

int PlayerState::foodCount() const
{
    int total = 0;

    for (const auto& item : bagItems)
    {
        if (isFoodItem(item.itemId, item.name))
        {
            total += item.count;
        }
    }

    return total;
}

int PlayerState::conjuredWaterCount() const
{
    int total = 0;

    for (const auto& item : bagItems)
    {
        if (isConjuredWater(item.itemId))
        {
            total += item.count;
        }
    }

    return total;
}

int PlayerState::conjuredFoodCount() const
{
    int total = 0;

    for (const auto& item : bagItems)
    {
        if (isConjuredFood(item.itemId))
        {
            total += item.count;
        }
    }

    return total;
}

int PlayerState::itemCount(std::string_view nameOrId) const
{
    if (iequals(nameOrId, "water"))
    {
        return waterCount();
    }

    if (iequals(nameOrId, "food"))
    {
        return foodCount();
    }

    const auto id = parseSpellId(nameOrId);
    int total = 0;

    for (const auto& item : bagItems)
    {
        if (id ? item.itemId == *id : iequals(item.name, nameOrId))
        {
            total += item.count;
        }
    }

    return total;
}

int PlayerState::inventorySearch(std::string_view partOrId) const
{
    const auto id = parseSpellId(partOrId);
    int total = 0;

    for (const auto& item : bagItems)
    {
        if (id ? item.itemId == *id : !partOrId.empty() && icontains(item.name, partOrId))
        {
            total += item.count;
        }
    }

    return total;
}

const UnitState* GameSnapshot::findUnit(Guid guid) const
{
    if (guid == 0)
    {
        return nullptr;
    }

    if (guid == player.guid)
    {
        return &player;
    }

    for (const auto& u : units)
    {
        if (u.guid == guid)
        {
            return &u;
        }
    }

    return nullptr;
}

const GameObjectState* GameSnapshot::findObject(Guid guid) const
{
    for (const auto& o : objects)
    {
        if (o.guid == guid)
        {
            return &o;
        }
    }

    return nullptr;
}

const UnitState* GameSnapshot::pet() const
{
    if (player.guid == 0)
    {
        return nullptr;
    }

    for (const auto& u : units)
    {
        if (u.ownerGuid == player.guid && !u.isCritter)
        {
            return &u;
        }
    }

    return nullptr;
}

bool GameSnapshot::isAttacker(const UnitState& u) const
{
    // A critter hit by accident is no fight worth stopping for.
    if (!u.isAlive() || u.reaction == Reaction::Friendly || u.guid == player.guid || u.isCritter)
    {
        return false;
    }

    if (!u.inCombat || u.targetGuid == 0)
    {
        return false;
    }

    if (u.targetGuid == player.guid)
    {
        return true;
    }

    const UnitState* p = pet();
    return p && u.targetGuid == p->guid;
}

std::vector<const UnitState*> GameSnapshot::attackers() const
{
    std::vector<const UnitState*> out;

    for (const auto& u : units)
    {
        if (isAttacker(u))
        {
            out.push_back(&u);
        }
    }

    return out;
}

std::vector<const UnitState*> GameSnapshot::hostilesNear(const Vec3& point, float radius) const
{
    std::vector<const UnitState*> out;

    for (const auto& u : units)
    {
        if (!u.isAlive() || u.isCritter)
        {
            continue;
        }

        if (u.reaction != Reaction::Hostile)
        {
            continue;
        }

        if (distance(u.position, point) <= radius)
        {
            out.push_back(&u);
        }
    }

    return out;
}

} // namespace og
