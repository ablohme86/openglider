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
 *  File: SpellNames.cpp
 *
 *  Desc:
 *  Implementation of SpellNames (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/SpellNames.h"
#include "og/common/FileUtil.h"
#include "og/common/Json.h"
#include "og/common/Sqlite.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <unordered_set>

namespace og::wowclient
{

namespace
{
std::string normalized(std::string_view name)
{
    std::string out;

    for (const char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    return out;
}

} // namespace

std::string SpellNames::actionName(std::string_view spellName)
{
    std::string out;
    bool wordStart = true;

    for (const char c : spellName)
    {
        const auto uc = static_cast<unsigned char>(c);

        if (std::isalnum(uc))
        {
            out += wordStart ? static_cast<char>(std::toupper(uc)) : c;
            wordStart = false;
        }
        else if (c != '\'')
        { // "Dragon's Breath" -> "DragonsBreath"
            wordStart = true;
        }
    }

    return out;
}

const std::vector<std::string>& SpellNames::spellsOf(std::string_view className) const
{
    static const std::vector<std::string> none;
    const auto it = byClass_.find(std::string(className));
    return it == byClass_.end() ? none : it->second;
}

std::vector<std::uint32_t> SpellNames::idsFor(std::string_view name) const
{
    const std::string wanted = normalized(name);
    std::vector<std::uint32_t> ids;

    if (wanted.empty())
    {
        return ids;
    }

    for (const auto& [id, spell] : names_)
    {
        if (normalized(spell) == wanted)
        {
            ids.push_back(id);
        }
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

SpellNames::SpellNames(const std::filesystem::path& path)
{
    std::string error;
    if (!path.empty())
    {
        if (path.extension() == ".db" || path.extension() == ".sqlite")
        {
            if (loadDatabase(path, &error) >= 0)
            {
                return;
            }
        }
        else
        {
            if (loadJson(path, &error) >= 0)
            {
                return;
            }
        }
    }
    else
    {
        const auto dbFile = og::resolveDataFile("world.db");
        // A database without spells in it is not the registry: the JSON then.
        if (std::filesystem::exists(dbFile) && loadDatabase(dbFile, &error) > 0)
        {
            return;
        }

        const auto jsonFile = og::resolveDataFile("spells.json");
        if (loadJson(jsonFile, &error) >= 0)
        {
            return;
        }
    }

    std::fprintf(stderr, "SpellNames: %s; spell names unavailable, numeric IDs still work\n", error.c_str());
}

int SpellNames::loadDatabase(const std::filesystem::path& path, std::string* error)
{
    if (error)
    {
        error->clear();
    }
    og::SqliteDb db;
    if (!db.openReadOnly(path, error))
    {
        return -1;
    }

    std::unordered_map<std::uint32_t, std::string> names;
    std::unordered_map<std::string, std::vector<std::string>> classes;

    try
    {
        auto stmt = db.prepare("SELECT id, name FROM spells");
        while (stmt.step())
        {
            names.emplace(stmt.columnUint32(0), stmt.columnString(1));
        }

        auto classStmt = db.prepare("SELECT class_name, action_name FROM spell_classes ORDER BY class_name");
        while (classStmt.step())
        {
            classes[classStmt.columnString(0)].push_back(classStmt.columnString(1));
        }

        names_ = std::move(names);
        byClass_ = std::move(classes);
        return static_cast<int>(names_.size());
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = e.what();
        }
        return -1;
    }
}

int SpellNames::loadJson(const std::filesystem::path& path, std::string* error)
{
    if (error)
    {
        error->clear();
    }
    try
    {
        const auto text = readTextFile(path);
        if (!text)
        {
            throw JsonError("cannot read file");
        }
        const Json root = Json::parse(*text);
        if (!root.isObject() || !root["version"].isInt() || root["version"].asInt() != 1)
        {
            throw JsonError("expected spell registry version 1");
        }
        std::unordered_map<std::uint32_t, std::string> names;
        std::unordered_map<std::string, std::vector<std::string>> classes;
        const auto readNames = [&](const Json& entries, bool overrideExisting)
        {
            if (!entries.isArray())
            {
                throw JsonError("spell entries must be arrays of [id, name]");
            }
            names.reserve(names.size() + entries.size());
            std::unordered_set<std::uint32_t> seen;
            for (const auto& entry : entries.asArray())
            {
                if (!entry.isArray() || entry.size() != 2 || !entry[0].isInt() ||
                    entry[0].asInt() <= 0 || entry[0].asInt() > std::numeric_limits<std::uint32_t>::max() ||
                    !entry[1].isString() || entry[1].asString().empty())
                {
                    throw JsonError("spell entries require a positive uint32 ID and a nonempty name");
                }
                const auto id = static_cast<std::uint32_t>(entry[0].asInt());
                if (!seen.insert(id).second)
                {
                    throw JsonError("duplicate spell ID " + std::to_string(id));
                }
                if (overrideExisting)
                {
                    names.insert_or_assign(id, entry[1].asString());
                }
                else
                {
                    names.emplace(id, entry[1].asString());
                }
            }
        };
        readNames(root["spells"], false);
        if (root.contains("overrides"))
        {
            readNames(root["overrides"], true);
        }
        if (root.contains("classes"))
        {
            if (!root["classes"].isObject())
            {
                throw JsonError("classes must be an object of action-name arrays");
            }
            for (const auto& [className, entries] : root["classes"].asObject())
            {
                if (className.empty() || !entries.isArray())
                {
                    throw JsonError("invalid class action list");
                }
                auto& spells = classes[className];
                for (const auto& name : entries.asArray())
                {
                    if (!name.isString() || name.asString().empty())
                    {
                        throw JsonError("class actions must be nonempty names");
                    }
                    spells.push_back(name.asString());
                }
            }
        }
        // Replace only after the entire document validates.
        names_ = std::move(names);
        byClass_ = std::move(classes);
        return static_cast<int>(names_.size());
    }
    catch (const JsonError& e)
    {
        if (error)
        {
            *error = path.string() + ": " + e.what();
        }
        return -1;
    }
}

const std::string* SpellNames::find(std::uint32_t spellId) const
{
    auto it = names_.find(spellId);
    return it == names_.end() ? nullptr : &it->second;
}

int SpellNames::loadYaml(const std::filesystem::path& path)
{
    try
    {
        const YAML::Node root = YAML::LoadFile(path.string());

        if (!root.IsMap())
        {
            return -1;
        }

        int count = 0;

        for (const auto& kv : root)
        {
            names_[kv.first.as<std::uint32_t>()] = kv.second.as<std::string>();
            ++count;
        }

        return count;
    }
    catch (const YAML::Exception&)
    {
        return -1;
    }
}

} // namespace og::wowclient
