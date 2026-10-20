/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-15
 *  File: LayoutJson.cpp
 *
 *  Desc:
 *  Implementation of LayoutJson (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/LayoutJson.h"

#include "og/wowclient/ClientLayout.h"
#include "og/wowclient/FactionTemplate.h"

#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <variant>
#include <vector>

namespace og::wowclient
{

namespace
{

using M = ModernLayout;
using Member = std::variant<std::uint64_t M::*, std::uint32_t M::*, int M::*, bool M::*>;

struct Field
{
    std::string_view name;
    Member member;
    // Fields added after the server first stored its layouts are optional and
    // stay 0 when missing, which the reader treats as "not known".
    bool required = true;
};

// Every field of ModernLayout, in the order it declares them.
const std::vector<Field>& fields()
{
    static const std::vector<Field> list{
        {"cameraGlobal", &M::cameraGlobal, false}, {"cameraPos", &M::cameraPos, false},
        {"cameraMatrix", &M::cameraMatrix, false}, {"cameraFov", &M::cameraFov, false},
        {"objectManager", &M::objectManager}, {"objectManagerFallback", &M::objectManagerFallback}, {"targetGuid", &M::targetGuid}, {"playerNameGuid", &M::playerNameGuid}, {"playerNameCacheBuckets", &M::playerNameCacheBuckets}, {"nameNodeNext", &M::nameNodeNext}, {"nameNodeGuid", &M::nameNodeGuid}, {"nameNodeName", &M::nameNodeName}, {"gameTime", &M::gameTime}, {"mapId", &M::mapId}, {"zoneId", &M::zoneId}, {"realmName", &M::realmName}, {"actionBars", &M::actionBars}, {"mgrSlots", &M::mgrSlots}, {"mgrSlotCount", &M::mgrSlotCount}, {"slotSize", &M::slotSize}, {"slotEntry", &M::slotEntry}, {"entryObject", &M::entryObject}, {"entryItemObject", &M::entryItemObject}, {"maxSlots", &M::maxSlots}, {"objEntry", &M::objEntry}, {"objGuid", &M::objGuid}, {"objTypeId", &M::objTypeId}, {"unitPosition", &M::unitPosition}, {"unitFacing", &M::unitFacing}, {"unitMovement", &M::unitMovement, false}, {"unitCastingSpell", &M::unitCastingSpell}, {"unitNameCache", &M::unitNameCache}, {"nameCacheName", &M::nameCacheName}, {"creatureCacheType", &M::creatureCacheType, false}, {"hasCreatureCacheType", &M::hasCreatureCacheType, false}, {"unitAuraData", &M::unitAuraData}, {"unitAuraCount", &M::unitAuraCount}, {"auraSize", &M::auraSize}, {"auraCaster", &M::auraCaster}, {"auraSpellId", &M::auraSpellId}, {"auraFlags", &M::auraFlags}, {"auraDuration", &M::auraDuration}, {"auraExpiration", &M::auraExpiration}, {"auraStacks", &M::auraStacks}, {"unitDynamicFlags", &M::unitDynamicFlags}, {"unitPower", &M::unitPower}, {"unitHealth", &M::unitHealth}, {"unitMaxHealth", &M::unitMaxHealth}, {"unitTarget", &M::unitTarget}, {"unitTapper", &M::unitTapper}, {"unitNpcFlags", &M::unitNpcFlags}, {"unitQuestGiverStatus", &M::unitQuestGiverStatus, false}, {"unitInteractMarker", &M::unitInteractMarker, false}, {"unitBytes0", &M::unitBytes0}, {"unitBytes1", &M::unitBytes1, false}, {"unitLevel", &M::unitLevel}, {"unitFactionTemplate", &M::unitFactionTemplate}, {"unitFlags", &M::unitFlags}, {"unitMaxPower", &M::unitMaxPower}, {"playerInventory", &M::playerInventory}, {"playerCoinage", &M::playerCoinage}, {"playerAutoAttackTarget", &M::playerAutoAttackTarget}, {"playerComboPoints", &M::playerComboPoints}, {"playerMaxComboPoints", &M::playerMaxComboPoints}, {"playerXp", &M::playerXp}, {"playerNextLevelXp", &M::playerNextLevelXp}, {"inventoryBagFirst", &M::inventoryBagFirst}, {"inventoryBagCount", &M::inventoryBagCount}, {"inventoryBackpackFirst", &M::inventoryBackpackFirst}, {"inventoryBackpackExtraCount", &M::inventoryBackpackExtraCount}, {"inventoryEquipmentCount", &M::inventoryEquipmentCount}, {"inventoryBackpackCount", &M::inventoryBackpackCount}, {"itemEntry", &M::itemEntry}, {"itemStackCount", &M::itemStackCount}, {"itemContainedIn", &M::itemContainedIn}, {"itemDurability", &M::itemDurability}, {"itemMaxDurability", &M::itemMaxDurability}, {"containerSlots", &M::containerSlots}, {"gameObjectPosition", &M::gameObjectPosition}, {"gameObjectNameCache", &M::gameObjectNameCache}, {"gameObjectCacheName", &M::gameObjectCacheName}, {"gameObjectCacheType", &M::gameObjectCacheType}, {"hasGameObjectCacheType", &M::hasGameObjectCacheType}, {"gameObjectCacheCaption", &M::gameObjectCacheCaption}, {"unitZoneId", &M::unitZoneId}, {"gameObjectCreatedBy", &M::gameObjectCreatedBy, false}, {"gameObjectCacheDisplayId", &M::gameObjectCacheDisplayId, false}, {"gameObjectScale", &M::gameObjectScale, false}, {"gameObjectBobbing", &M::gameObjectBobbing, false}, {"gameObjectState", &M::gameObjectState, false}, {"gameObjectBobbingValue", &M::gameObjectBobbingValue, false}, {"gameObjectBobbingMask", &M::gameObjectBobbingMask, false}, {"itemEnchantments", &M::itemEnchantments, false}, {"itemEnchantmentSize", &M::itemEnchantmentSize, false}, {"lootFlagTrusted", &M::lootFlagTrusted, false}, {"autoRepeatSpell", &M::autoRepeatSpell, false}, {"playerBackpackSlots", &M::playerBackpackSlots, false}, {"lastNameAfterFirst", &M::lastNameAfterFirst, false}, {"playerSkillIds", &M::playerSkillIds, false}, {"playerSkillStride", &M::playerSkillStride, false}, {"playerQuestLog", &M::playerQuestLog, false}, {"playerQuestLogStride", &M::playerQuestLogStride, false}, {"playerQuestLogProgress", &M::playerQuestLogProgress, false}, {"playerKnownTaxiNodes", &M::playerKnownTaxiNodes, false}, {"playerKnownTaxiNodesInline", &M::playerKnownTaxiNodesInline, false}, {"mouseoverGuid", &M::mouseoverGuid, false}, {"corpsePosition", &M::corpsePosition, false}, {"unitSummonedBy", &M::unitSummonedBy, false}, {"unitCreatedBy", &M::unitCreatedBy, false}, {"uiTop", &M::uiTop, false}, {"uiTopObjects", &M::uiTopObjects, false}, {"uiObjectLink", &M::uiObjectLink, false}, {"uiObjectName", &M::uiObjectName, false}, {"uiObjectRect", &M::uiObjectRect, false}, {"uiObjectFlags", &M::uiObjectFlags, false}, {"uiVisibleMask", &M::uiVisibleMask, false}, {"uiFrameRegions", &M::uiFrameRegions, false}, {"uiRegionLink", &M::uiRegionLink, false}, {"uiFontStringText", &M::uiFontStringText, false}, {"uiFontStringColor", &M::uiFontStringColor, false}, {"uiDirectFontString", &M::uiDirectFontString, false}, {"uiObjectParent", &M::uiObjectParent, false}, {"uiTooltipOwner", &M::uiTooltipOwner, false}, {"uiBagButtonVtable", &M::uiBagButtonVtable, false}, {"uiCooldownStart", &M::uiCooldownStart, false}, {"uiCooldownDuration", &M::uiCooldownDuration, false}, {"uiActionButtonTable", &M::uiActionButtonTable, false},
    };
    return list;
}

// "0x1234560", "30" or 30.
std::optional<std::uint64_t> number(const Json& value)
{
    if (value.isInt())
    {
        const std::int64_t n = value.asInt();

        if (n < 0)
        {
            return std::nullopt;
        }

        return static_cast<std::uint64_t>(n);
    }

    if (!value.isString())
    {
        return std::nullopt;
    }

    std::string_view text = value.asString();
    int base = 10;

    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
        base = 16;
    }

    if (text.empty())
    {
        return std::nullopt;
    }

    std::uint64_t out = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out, base);

    if (ec != std::errc() || end != text.data() + text.size())
    {
        return std::nullopt;
    }

    return out;
}

bool assign(M& layout, const Member& member, const Json& value)
{
    if (const auto* flag = std::get_if<bool M::*>(&member))
    {
        if (!value.isBool())
        {
            return false;
        }

        layout.*(*flag) = value.asBool();
        return true;
    }

    const auto n = number(value);

    if (!n)
    {
        return false;
    }

    if (const auto* wide = std::get_if<std::uint64_t M::*>(&member))
    {
        layout.*(*wide) = *n;
        return true;
    }

    if (const auto* offset = std::get_if<std::uint32_t M::*>(&member))
    {
        if (*n > std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }

        layout.*(*offset) = static_cast<std::uint32_t>(*n);
        return true;
    }

    const auto* count = std::get_if<int M::*>(&member);

    if (!count || *n > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    layout.*(*count) = static_cast<int>(*n);
    return true;
}

Json toJson(const M& layout, const Member& member)
{
    if (const auto* flag = std::get_if<bool M::*>(&member))
    {
        return Json(layout.*(*flag));
    }

    if (const auto* count = std::get_if<int M::*>(&member))
    {
        return Json(layout.*(*count));
    }

    if (const auto* wide = std::get_if<std::uint64_t M::*>(&member))
    {
        return Json(std::format("0x{:X}", layout.*(*wide)));
    }

    return Json(std::format("0x{:X}", layout.*std::get<std::uint32_t M::*>(member)));
}

// The table a build uses when its document does not say.
std::string_view defaultFactionTable(std::uint32_t build)
{
    return build == 69795 ? "2.5.6" : "";
}

} // namespace

std::string productName(int major, int minor)
{
    if (major == 1 && minor >= 60)
    {
        return "WoW Forever";
    }

    if (major == 2 && minor == 5)
    {
        return "TBC Anniversary";
    }

    if (major == 1 && minor >= 13)
    {
        return "Classic Era";
    }

    return {};
}

std::string layoutFileName(const LayoutDocument& document)
{
    std::string slug;

    for (const char c : document.name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    return "layout-" + (slug.empty() ? std::to_string(document.build) : slug) + ".json";
}

FactionTable factionTableByName(std::string_view name)
{
    if (name == "2.5.6")
    {
        return &factionTemplate256;
    }

    return nullptr;
}

std::optional<LayoutDocument> layoutFromJson(const Json& json, std::string* error)
{
    const auto fail = [error](std::string message) -> std::optional<LayoutDocument>
    {
        if (error)
        {
            *error = std::move(message);
        }

        return std::nullopt;
    };

    if (!json.isObject())
    {
        return fail("a layout must be a JSON object");
    }

    LayoutDocument document;
    const Json* name = json.find("name");

    if (!name || !name->isString() || name->asString().empty())
    {
        return fail("the layout has no name");
    }

    document.name = name->asString();

    if (const Json* version = json.find("clientVersion"); version && version->isString())
    {
        document.clientVersion = version->asString();
    }

    const Json* build = json.find("build");
    const auto buildNumber = build ? number(*build) : std::nullopt;

    if (!buildNumber || *buildNumber == 0 || *buildNumber > std::numeric_limits<std::uint32_t>::max())
    {
        return fail(std::format("layout {} has no valid build number", document.name));
    }

    document.build = static_cast<std::uint32_t>(*buildNumber);

    if (const Json* table = json.find("factionTable"); table && table->isString())
    {
        document.factionTable = table->asString();
    }

    if (document.factionTable.empty())
    {
        document.factionTable = defaultFactionTable(document.build);
    }

    const Json* modern = json.find("modern");

    if (!modern || !modern->isObject())
    {
        return fail(std::format("layout {} has no \"modern\" offsets", document.name));
    }

    for (const Field& field : fields())
    {
        const Json* value = modern->find(field.name);

        if (!value)
        {
            if (field.required)
            {
                return fail(std::format("layout {} is missing {}", document.name, field.name));
            }

            continue;
        }

        if (!assign(document.modern, field.member, *value))
        {
            return fail(std::format("layout {}: {} is not a valid value", document.name, field.name));
        }
    }

    document.modern.factionTemplate = factionTableByName(document.factionTable);
    return document;
}

Json layoutToJson(const LayoutDocument& document)
{
    Json modern = Json::object();

    for (const Field& field : fields())
    {
        modern[field.name] = toJson(document.modern, field.member);
    }

    Json out = Json::object();
    out["name"] = document.name;

    if (!document.clientVersion.empty())
    {
        out["clientVersion"] = document.clientVersion;
    }

    out["build"] = static_cast<std::int64_t>(document.build);
    out["readOnly"] = true;

    if (!document.factionTable.empty())
    {
        out["factionTable"] = document.factionTable;
    }

    out["modern"] = std::move(modern);
    return out;
}

std::optional<LayoutDocument> documentFor(const ClientLayout& layout)
{
    if (!layout.modern)
    {
        return std::nullopt;
    }

    LayoutDocument document;
    document.name = std::string(layout.name);
    document.clientVersion = std::string(layout.clientVersion);
    document.build = layout.build;
    document.modern = *layout.modern;

    if (layout.modern->factionTemplate == factionTableByName("2.5.6"))
    {
        document.factionTable = "2.5.6";
    }

    return document;
}

} // namespace og::wowclient
