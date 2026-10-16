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
 *  File: SpellNames.h
 *
 *  Desc:
 *  Declarations and interface for SpellNames (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace og::wowclient
{

// Loads spell ids, names and class action lists from data/spells.json for
// aura lookups ("target.hasDebuff('Rend')"). spells.yaml can extend it:
//
//   772: Rend
//   6546: Rend
class SpellNames
{
public:
    // Empty path resolves spells.json using the shared app data search path.
    explicit SpellNames(const std::filesystem::path& path = {});

    const std::string* find(std::uint32_t spellId) const;
    // All ids whose name matches, ignoring case, spaces and punctuation
    // ("FrostArmor" matches "Frost Armor").
    std::vector<std::uint32_t> idsFor(std::string_view name) const;
    // The action name a spell is bound under: every word capitalised, spaces and
    // punctuation dropped ("Cone of Cold" -> "ConeOfCold"). Action names are
    // case-sensitive, so this has to match how actions.yaml and scripts spell them.
    static std::string actionName(std::string_view spellName);
    void set(std::uint32_t spellId, std::string name)
    {
        names_[spellId] = std::move(name);
    }

    // Returns the number of entries loaded, or -1 on error.
    int loadYaml(const std::filesystem::path& path);
    // Replace the registry from JSON, atomically. On failure, keep existing
    // names and report the path and reason; return -1 (otherwise name count).
    int loadJson(const std::filesystem::path& path, std::string* error = nullptr);
    // Replace the registry from SQLite database (world.db), atomically.
    int loadDatabase(const std::filesystem::path& path, std::string* error = nullptr);
    std::size_t size() const
    {
        return names_.size();
    }

    // The configured spells of a class ("Warlock"), by their game names.
    const std::vector<std::string>& spellsOf(std::string_view className) const;

private:
    std::unordered_map<std::uint32_t, std::string> names_;
    std::unordered_map<std::string, std::vector<std::string>> byClass_;
};

} // namespace og::wowclient
