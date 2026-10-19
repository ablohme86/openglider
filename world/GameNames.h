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
 *  File: GameNames.h
 *
 *  Desc:
 *  The game's names in the language of the client attached: NPCs, their
 *  titles, quests, quest items and objects, by the quest data's ids.
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace og
{

class GameNames
{
public:
    // One language's names from world.db's l10n tables (QuestieDB l10n/Forever).
    // False where the database has none for it: enUS, the quest data's own,
    // needs none.
    bool load(const std::filesystem::path& worldDb, const std::string& locale, std::string* error = nullptr);

    // The language of the names a client shows, where its Config.wtf does
    // not say (no textLocale there: a client installed in its language and
    // never switched). Each NPC seen (id, name as the client has it) is looked
    // up in world.db; the language most of them are in, "enUS" for the quest
    // data's own. nullopt with fewer than `fewest` known.
    static std::optional<std::string> localeOfNpcNames(const std::filesystem::path& worldDb, const std::vector<std::pair<std::uint32_t, std::string>>& seen, int fewest = 3, std::string* error = nullptr);

    const std::string& locale() const
    {
        return locale_;
    }

    bool empty() const
    {
        return npcs_.empty() && quests_.empty();
    }

    std::optional<std::string> npc(std::uint32_t id) const;
    std::optional<std::string> npcTitle(std::uint32_t id) const;
    std::optional<std::string> quest(std::uint32_t id) const;
    std::optional<std::string> item(std::uint32_t id) const;
    std::optional<std::string> object(std::uint32_t id) const;
    // An NPC's id by its name in this language.
    std::optional<std::uint32_t> npcId(std::string_view name) const;

private:
    std::string locale_;
    std::unordered_map<std::uint32_t, std::string> npcs_, titles_, quests_, items_, objects_;
    std::unordered_map<std::string, std::uint32_t> npcByName_;
};

} // namespace og
