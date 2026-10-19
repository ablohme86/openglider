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

#include <algorithm>
#include <map>


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

std::optional<std::string> GameNames::localeOfNpcNames(const std::filesystem::path& worldDb, const std::vector<std::pair<std::uint32_t, std::string>>& seen, int fewest, std::string* error)
{
    SqliteDb db;

    if (!db.openReadOnly(worldDb, error))
    {
        return std::nullopt;
    }

    std::map<std::string, int> votes;
    int known = 0;

    try
    {
        auto english = db.prepare("SELECT 1 FROM npcs WHERE id = ? AND name = ?");
        auto other = db.prepare("SELECT locale FROM l10n_npcs WHERE id = ? AND name = ?");

        for (const auto& [id, name] : seen)
        {
            if (id == 0 || name.empty())
            {
                continue;
            }

            bool any = false;
            english.reset();
            english.bindUint32(1, id);
            english.bindText(2, name);

            // The English name, kept in some languages (Rejold Barleybrew is
            // that in German too): English.
            if (english.step())
            {
                ++votes["enUS"];
                ++known;
                continue;
            }

            // zhCN and zhTW, or esES and esMX, may share a name: each counts.
            other.reset();
            other.bindUint32(1, id);
            other.bindText(2, name);

            while (other.step())
            {
                ++votes[other.columnString(0)];
                any = true;
            }

            known += any ? 1 : 0;
        }
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = e.what();
        }

        return std::nullopt;
    }

    if (known < fewest)
    {
        return std::nullopt;
    }

    const auto best = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return best->first;
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
