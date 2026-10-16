/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-04
 *  File: QuestCounts.cpp
 *
 *  Desc:
 *  Implementation of QuestCounts (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/QuestCounts.h"

#include "og/wowclient/ChatLog.h"

#include <cctype>
#include <charconv>
#include <regex>

namespace og::wowclient
{

namespace
{

std::string trimmed(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }

    text.erase(0, text.find_first_not_of(" \t"));
    return text;
}

std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

} // namespace

// A count of three digits at most; nullopt for anything else, rather than
// std::stoi's throw on "99999999999".
static std::optional<int> smallCount(const std::string& digits)
{
    int value = 0;

    if (digits.empty() || digits.size() > 3 || std::from_chars(digits.data(), digits.data() + digits.size(), value).ec != std::errc{})
    {
        return std::nullopt;
    }

    return value;
}

static std::string normalizeQuestTitle(std::string text)
{
    text = lower(trimmed(plainChatText(text)));
    if (constexpr std::string_view completeSuffix = " (complete)"; text.ends_with(completeSuffix))
    {
        text.resize(text.size() - completeSuffix.size());
    }
    else if (constexpr std::string_view completeSuffix2 = " (completed)"; text.ends_with(completeSuffix2))
    {
        text.resize(text.size() - completeSuffix2.size());
    }
    return trimmed(text);
}

std::optional<ObjectiveCount> parseObjectiveCount(std::string_view line)
{
    static const std::regex countFirst(R"(^\s*(\d+)\s*/\s*(\d+)\s+(.+)$)");
    static const std::regex countLast(R"(^(.+?):\s*(\d+)\s*/\s*(\d+)\s*$)");
    std::string text = plainChatText(line);
    bool lineCompleted = false;
    if (constexpr std::string_view comp = " (complete)"; lower(text).ends_with(comp))
    {
        text.resize(text.size() - comp.size());
        lineCompleted = true;
    }
    else if (constexpr std::string_view comp2 = " (completed)"; lower(text).ends_with(comp2))
    {
        text.resize(text.size() - comp2.size());
        lineCompleted = true;
    }

    std::smatch m;
    ObjectiveCount out;

    std::optional<int> have, want;

    if (std::regex_match(text, m, countFirst))
    {
        out.name = trimmed(m[3].str());
        have = smallCount(m[1].str());
        want = smallCount(m[2].str());
    }
    else if (std::regex_match(text, m, countLast))
    {
        out.name = trimmed(m[1].str());
        have = smallCount(m[2].str());
        want = smallCount(m[3].str());
    }

    if (!have || !want)
    {
        return std::nullopt;
    }

    out.have = lineCompleted ? std::max(*have, *want) : *have;
    out.want = *want;

    if (constexpr std::string_view slain = " slain"; out.name.size() > slain.size() && lower(out.name).ends_with(slain))
    {
        out.name.resize(out.name.size() - slain.size());
    }

    // "XP: 1225/3600" is no objective, nor a count of thousands.
    if (out.name.empty() || out.want <= 0 || out.want > 250 || lower(out.name) == "xp")
    {
        return std::nullopt;
    }

    return out;
}

std::map<std::string, int> objectiveCountsIn(const std::vector<PlacedText>& texts)
{
    std::map<std::string, int> out;

    for (const auto& [text, bottom, left] : texts)
    {
        if (const auto count = parseObjectiveCount(text))
        {
            // Two quests asking for the same thing in different numbers: no
            // telling which is whose, so 0 - each quest keeps its data's.
            if (auto [at, added] = out.try_emplace(lower(count->name), count->want); !added && at->second != count->want)
            {
                at->second = 0;
            }
        }
    }

    return out;
}

std::map<std::string, std::vector<ObjectiveCount>> trackedQuests(const std::vector<PlacedText>& texts)
{
    // Measured on 1.60.1 at 1080 lines: a title's bottom 0.0064 above its
    // first line's, the lines 0.0041 in from the title.
    constexpr float kIndent = 0.03f;
    constexpr float kBelow = 0.2f;
    std::vector<const PlacedText*> titles;

    for (const auto& t : texts)
    {
        if (t.bottom > 0 && t.text.starts_with("|cffffd100") && !parseObjectiveCount(t.text))
        {
            titles.push_back(&t);
        }
    }

    std::map<std::string, std::vector<ObjectiveCount>> out;

    for (const auto& t : texts)
    {
        const auto count = t.bottom > 0 ? parseObjectiveCount(t.text) : std::nullopt;

        if (!count)
        {
            continue;
        }

        const PlacedText* owner = nullptr;

        for (const PlacedText* title : titles)
        {
            const bool above = title->bottom > t.bottom && title->bottom - t.bottom < kBelow;
            const bool column = t.left >= title->left - 0.001f && t.left - title->left < kIndent;

            if (above && column && (!owner || title->bottom < owner->bottom))
            {
                owner = title;
            }
        }

        if (owner)
        {
            const std::string rawOwner = lower(trimmed(plainChatText(owner->text)));
            const bool isComplete = rawOwner.ends_with(" (complete)") || rawOwner.ends_with(" (completed)");
            ObjectiveCount item = *count;
            if (isComplete)
            {
                item.have = std::max(item.have, item.want);
            }
            out[normalizeQuestTitle(owner->text)].push_back(std::move(item));
        }
    }

    for (const PlacedText* title : titles)
    {
        const std::string rawTitle = lower(trimmed(plainChatText(title->text)));
        if (rawTitle.ends_with(" (complete)") || rawTitle.ends_with(" (completed)"))
        {
            const std::string norm = normalizeQuestTitle(title->text);
            auto& list = out[norm];
            if (list.empty())
            {
                list.push_back(ObjectiveCount{"Complete", 1, 1});
            }
            else
            {
                for (auto& obj : list)
                {
                    obj.have = std::max(obj.have, obj.want);
                }
            }
        }
    }

    return out;
}

} // namespace og::wowclient
