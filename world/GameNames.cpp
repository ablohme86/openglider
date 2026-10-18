/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-18
 *  File: GameNames.cpp
 *
 *  Desc:
 *  Implementation of GameNames.
 *
 */
#include "og/world/GameNames.h"

#include "og/common/Sqlite.h"


namespace og
{

bool GameNames::load(const std::filesystem::path& worldDb, const std::string& locale, std::string* error)
{
    locale_ = locale;
    npcs_.clear();
    titles_.clear();
    quests_.clear();
    items_.clear();
    objects_.clear();
    npcByName_.clear();
    SqliteDb db;

    if (!db.openReadOnly(worldDb, error))
    {
        return false;
    }

    try
    {
        const auto each = [&](const char* sql, auto&& take)
        {
            auto stmt = db.prepare(sql);
            stmt.bindText(1, locale);

            while (stmt.step())
            {
                take(stmt);
            }
        };
        each("SELECT id, name, sub_name FROM l10n_npcs WHERE locale = ?",
             [&](SqliteStmt& s)
             {
                 const auto id = s.columnUint32(0);

                 if (!s.isNull(1))
                 {
                     npcs_[id] = s.columnString(1);
                     npcByName_.emplace(s.columnString(1), id);
                 }

                 if (!s.isNull(2))
                 {
                     titles_[id] = s.columnString(2);
                 }
             });
        const auto into = [](auto& map) { return [&map](SqliteStmt& s) { if (!s.isNull(1)) { map[s.columnUint32(0)] = s.columnString(1); } }; };
        each("SELECT id, title FROM l10n_quests WHERE locale = ?", into(quests_));
        each("SELECT id, name FROM l10n_items WHERE locale = ?", into(items_));
        each("SELECT id, name FROM l10n_objects WHERE locale = ?", into(objects_));
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = e.what();
        }

        return false;
    }

    return !empty();
}

namespace
{
template <class Map> std::optional<std::string> find(const Map& map, std::uint32_t id)
{
    const auto it = map.find(id);
    return it == map.end() ? std::nullopt : std::optional<std::string>{it->second};
}
} // namespace

std::optional<std::string> GameNames::npc(std::uint32_t id) const
{
    return find(npcs_, id);
}

std::optional<std::string> GameNames::npcTitle(std::uint32_t id) const
{
    return find(titles_, id);
}

std::optional<std::string> GameNames::quest(std::uint32_t id) const
{
    return find(quests_, id);
}

std::optional<std::string> GameNames::item(std::uint32_t id) const
{
    return find(items_, id);
}

std::optional<std::string> GameNames::object(std::uint32_t id) const
{
    return find(objects_, id);
}

std::optional<std::uint32_t> GameNames::npcId(std::string_view name) const
{
    const auto it = npcByName_.find(std::string(name));
    return it == npcByName_.end() ? std::nullopt : std::optional<std::uint32_t>{it->second};
}

} // namespace og
