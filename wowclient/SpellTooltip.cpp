/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-13
 *  File: SpellTooltip.cpp
 *
 *  Desc:
 *  Implementation of SpellTooltip (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/SpellTooltip.h"

#include <charconv>
#include <string_view>

namespace og::wowclient
{

namespace
{

constexpr float kMeleeRange = 5.0f;

std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    {
        text.remove_suffix(1);
    }

    return text;
}

// The number a text starts with, and what follows it.
std::optional<double> leadingNumber(std::string_view& text)
{
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);

    if (ec != std::errc{} || end == text.data())
    {
        return std::nullopt;
    }

    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
    return value;
}

std::optional<Duration> cooldownOf(std::string_view line)
{
    if (!line.ends_with(" cooldown") && !line.ends_with(" cd"))
    {
        return std::nullopt;
    }

    const auto amount = leadingNumber(line);

    if (!amount || *amount < 0.0)
    {
        return std::nullopt;
    }

    if (line == " sec cooldown" || line == " sec cd")
    {
        return seconds(*amount);
    }

    if (line == " min cooldown" || line == " min cd")
    {
        return seconds(*amount * 60.0);
    }

    return std::nullopt;
}

std::optional<Duration> castTimeOf(std::string_view line)
{
    if (line == "Instant" || line == "Instant cast")
    {
        return Duration(0);
    }

    if (!line.ends_with(" cast"))
    {
        return std::nullopt;
    }

    const auto amount = leadingNumber(line);

    if (!amount || *amount < 0.0)
    {
        return std::nullopt;
    }

    if (line == " sec cast")
    {
        return seconds(*amount);
    }

    if (line == " min cast")
    {
        return seconds(*amount * 60.0);
    }

    return std::nullopt;
}

std::optional<ActionRange> rangeOf(std::string_view line)
{
    if (line == "Melee Range")
    {
        return ActionRange{0.0f, kMeleeRange};
    }

    if (!line.ends_with(" yd range"))
    {
        return std::nullopt;
    }

    const auto first = leadingNumber(line);

    if (!first || *first < 0.0)
    {
        return std::nullopt;
    }

    if (line == " yd range")
    {
        return ActionRange{0.0f, static_cast<float>(*first)};
    }

    if (!line.starts_with("-"))
    {
        return std::nullopt;
    }

    line.remove_prefix(1);
    const auto second = leadingNumber(line);

    if (!second || *second < *first || line != " yd range")
    {
        return std::nullopt;
    }

    return ActionRange{static_cast<float>(*first), static_cast<float>(*second)};
}

} // namespace

SpellTooltipData parseSpellTooltip(std::span<const std::string> lines)
{
    SpellTooltipData data;

    // The first of each: the description further down may say "30 yd range"
    // of something else.
    for (const std::string& raw : lines)
    {
        const std::string_view line = trimmed(raw);

        if (!data.cooldown)
        {
            data.cooldown = cooldownOf(line);
        }

        if (!data.castTime)
        {
            data.castTime = castTimeOf(line);
        }

        if (!data.range)
        {
            data.range = rangeOf(line);
        }
    }

    return data;
}

} // namespace og::wowclient
