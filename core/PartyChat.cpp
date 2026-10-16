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
 *  File: PartyChat.cpp
 *
 *  Desc:
 *  Implementation of PartyChat (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/PartyChat.h"

#include "og/common/FileUtil.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <regex>
#include <sstream>

namespace og
{

namespace
{

constexpr std::string_view kNames[PartyChat::kKinds] = {"vendor", "vendorDone", "lost", "position", "ack"};

// Lower case, letters and digits only, one space between words: "Must
// vendor!" and "must vendor" are the same line.
std::string plain(std::string_view text)
{
    std::string out;

    for (const char c : text)
    {
        const auto u = static_cast<unsigned char>(c);

        if (std::isalnum(u))
        {
            out += static_cast<char>(std::tolower(u));
        }
        else if (!out.empty() && out.back() != ' ')
        {
            out += ' ';
        }
    }

    while (!out.empty() && out.back() == ' ')
    {
        out.pop_back();
    }

    return out;
}

// A position template as a pattern: {x} and {y} numbers, the rest as written,
// any case, spaces and punctuation round it loose.
std::regex positionPattern(std::string_view line)
{
    std::string pattern = "^\\W*?";

    for (std::size_t i = 0; i < line.size();)
    {
        if (line.substr(i, 3) == "{x}" || line.substr(i, 3) == "{y}")
        {
            pattern += "(-?\\d+(?:\\.\\d+)?)";
            i += 3;
            continue;
        }

        const char c = line[i++];

        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            pattern += c;
        }
        else
        {
            pattern += "\\W*?"; // lazy: a minus sign belongs to the number after it
        }
    }

    return std::regex(pattern + "\\W*$", std::regex::icase);
}

} // namespace

std::string_view PartyChat::nameOf(Kind kind)
{
    return kNames[static_cast<std::size_t>(kind)];
}

bool PartyChat::loadText(std::string_view text)
{
    std::array<std::vector<std::string>, kKinds> lines;
    std::optional<std::size_t> kind;
    std::istringstream in{std::string(text)};
    std::string line;

    while (std::getline(in, line))
    {
        if (const auto hash = line.find('#'); hash != std::string::npos)
        {
            line.erase(hash);
        }

        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
        {
            line.pop_back();
        }

        line.erase(0, line.find_first_not_of(" \t"));

        if (line.empty())
        {
            continue;
        }

        if (line.front() == '[' && line.back() == ']')
        {
            const std::string name = line.substr(1, line.size() - 2);
            kind.reset();

            for (std::size_t k = 0; k < kKinds; ++k)
            {
                if (kNames[k] == name)
                {
                    kind = k;
                }
            }

            continue;
        }

        if (kind)
        {
            lines[*kind].push_back(line);
        }
    }

    if (std::all_of(lines.begin(), lines.end(), [](const auto& l) { return l.empty(); }))
    {
        return false;
    }

    lines_ = std::move(lines);
    last_.fill(-1);
    return true;
}

bool PartyChat::loadFile(const std::string& path)
{
    const auto text = readTextFile(path);
    return text && loadText(*text);
}

std::optional<PartyChat::Heard> PartyChat::hear(std::string_view text) const
{
    for (const auto& line : lines_[static_cast<std::size_t>(Kind::Position)])
    {
        std::smatch match;
        const std::string said(text);

        if (std::regex_match(said, match, positionPattern(line)) && match.size() == 3)
        {
            // Which number is x: the order the template has them in.
            const bool xFirst = line.find("{x}") < line.find("{y}");
            const float a = std::stof(match[1].str()), b = std::stof(match[2].str());
            return Heard{Kind::Position, Vec3{xFirst ? a : b, xFirst ? b : a, 0}};
        }
    }

    const std::string heard = plain(text);

    for (std::size_t k = 0; k < kKinds; ++k)
    {
        if (static_cast<Kind>(k) == Kind::Position)
        {
            continue;
        }

        for (const auto& line : lines_[k])
        {
            if (plain(line) == heard)
            {
                return Heard{static_cast<Kind>(k), std::nullopt};
            }
        }
    }

    return std::nullopt;
}

std::string PartyChat::say(Kind kind, std::mt19937& random, const Vec3& place)
{
    const std::size_t k = static_cast<std::size_t>(kind);
    const auto& lines = lines_[k];

    if (lines.empty())
    {
        return {};
    }

    int pick = static_cast<int>(std::uniform_int_distribution<std::size_t>(0, lines.size() - 1)(random));

    // Not the same line twice running.
    if (lines.size() > 1 && pick == last_[k])
    {
        pick = (pick + 1) % static_cast<int>(lines.size());
    }

    last_[k] = pick;
    std::string line = lines[static_cast<std::size_t>(pick)];

    for (const auto& [field, value] : {std::pair{"{x}", place.x}, std::pair{"{y}", place.y}})
    {
        if (const auto at = line.find(field); at != std::string::npos)
        {
            line.replace(at, 3, std::format("{:.1f}", value));
        }
    }

    return line;
}

} // namespace og
