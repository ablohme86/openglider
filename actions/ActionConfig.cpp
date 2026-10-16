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
 *  File: ActionConfig.cpp
 *
 *  Desc:
 *  Implementation of ActionConfig (semantic game actions, action
 *  configuration, and execution).
 *
 *
 */
#include "og/actions/ActionConfig.h"

#include "og/common/FileUtil.h"
#include "DefaultActions.h"

#include <yaml-cpp/yaml.h>

#include <format>

namespace og
{

ActionConfigResult parseActionConfig(const std::string& yamlText)
{
    ActionConfigResult result;
    YAML::Node root;

    try
    {
        root = YAML::Load(yamlText);
    }
    catch (const YAML::Exception& e)
    {
        result.errors.push_back(std::format("YAML error: {}", e.what()));
        return result;
    }

    if (!root || !root.IsMap())
    {
        result.errors.push_back("actions config must be a YAML mapping");
        return result;
    }

    try
    {
        if (auto gcd = root["globalCooldown"])
        {
            result.map.globalCooldown = seconds(gcd.as<double>());
        }

        const YAML::Node actionsNode = root["actions"];

        if (!actionsNode)
        {
            result.errors.push_back("missing 'actions' section");
            return result;
        }

        if (!actionsNode.IsMap())
        {
            result.errors.push_back("'actions' must be a mapping of action name -> binding");
            return result;
        }

        for (const auto& kv : actionsNode)
        {
            ActionBinding b;
            b.name = kv.first.as<std::string>();
            const YAML::Node& v = kv.second;
            std::string keyText;

            if (v.IsScalar())
            {
                keyText = v.as<std::string>();
            }
            else if (v.IsMap())
            {
                // `chat` alone: a slash command typed for it (/petattack), no key.
                if (auto c = v["chat"])
                {
                    b.chatCommand = c.as<std::string>();
                }

                keyText = v["key"] ? v["key"].as<std::string>() : std::string();

                if (auto c = v["cooldown"])
                {
                    b.cooldown = seconds(c.as<double>());
                }

                if (auto g = v["gcd"])
                {
                    b.onGcd = g.as<bool>();
                }

                if (auto ct = v["castTime"])
                {
                    b.castTime = seconds(ct.as<double>());
                }

                if (auto r = v["range"])
                {
                    b.range.max = r.as<float>();
                }

                if (auto r = v["minRange"])
                {
                    b.range.min = r.as<float>();
                }

                if (auto f = v["friendly"])
                {
                    b.friendly = f.as<bool>();
                }
            }
            else
            {
                result.errors.push_back(std::format("{}: binding must be a key string or a mapping", b.name));
                continue;
            }

            if (keyText.empty())
            {
                // Neither key nor chat: how the spell behaves, for the key the
                // action bar gives it.
                if (b.chatCommand.empty())
                {
                    result.map.addInfo(std::move(b));
                }
                else
                {
                    result.map.add(std::move(b));
                }

                continue;
            }

            auto chord = KeyChord::parse(keyText);

            if (!chord)
            {
                result.errors.push_back(std::format("{}: invalid key '{}'", b.name, keyText));
                continue;
            }

            b.chord = *chord;
            result.map.add(std::move(b));
        }
    }
    catch (const YAML::Exception& e)
    {
        result.errors.push_back(std::format("YAML error: {}", e.what()));
    }

    return result;
}

ActionConfigResult defaultActionConfig()
{
    return parseActionConfig(kDefaultActions);
}

ActionConfigResult loadActionConfig(const std::filesystem::path& path)
{
    auto text = readTextFile(path);

    if (!text)
    {
        ActionConfigResult r;
        r.errors.push_back(std::format("cannot read {}", path.string()));
        return r;
    }

    return parseActionConfig(*text);
}

} // namespace og
