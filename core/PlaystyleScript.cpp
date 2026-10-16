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
 *  File: PlaystyleScript.cpp
 *
 *  Desc:
 *  Implementation of PlaystyleScript (bot engine, behaviors, and core
 *  state machine).
 *
 *
 */
#include "og/core/PlaystyleScript.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <set>

namespace og
{

namespace
{

// A spell cast in fewer of the fights' casts than this is a one-off.
constexpr double kRotationShare = 0.04;
constexpr std::uint32_t kRotationLeast = 3;
// An opener: used to start at least this share of the fights.
constexpr double kPullShare = 0.1;
constexpr std::uint32_t kPullLeast = 2;
constexpr std::uint32_t kRestLeast = 3;

std::string_view trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }

    return text;
}

// The line without its comment; a # or // inside a quoted name is no comment.
std::string_view code(std::string_view line)
{
    bool quoted = false;

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '"')
        {
            quoted = !quoted;
        }
        else if (!quoted && (line[i] == '#' || (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/')))
        {
            return trim(line.substr(0, i));
        }
    }

    return trim(line);
}

int braces(std::string_view line)
{
    int depth = 0;
    bool quoted = false;

    for (const char c : code(line))
    {
        if (c == '"')
        {
            quoted = !quoted;
        }
        else if (!quoted)
        {
            depth += c == '{' ? 1 : c == '}' ? -1 : 0;
        }
    }

    return depth;
}

std::vector<std::string> linesOf(std::string_view text)
{
    std::vector<std::string> lines;

    while (!text.empty())
    {
        const auto end = text.find('\n');
        std::string_view one = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

        if (!one.empty() && one.back() == '\r')
        {
            one.remove_suffix(1);
        }

        lines.emplace_back(one);
    }

    return lines;
}

// A block written the usual way, "name {" on a line of its own: the lines of
// its body, [first, last). nullopt when the script has none written so.
struct Body
{
    std::size_t first = 0;
    std::size_t last = 0;
};

std::optional<Body> blockOf(const std::vector<std::string>& lines, std::string_view name)
{
    int depth = 0;

    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string_view line = code(lines[i]);

        if (depth == 0 && line.starts_with(name) && trim(line.substr(name.size())) == "{")
        {
            int inner = 1;

            for (std::size_t j = i + 1; j < lines.size(); ++j)
            {
                inner += braces(lines[j]);

                if (inner <= 0)
                {
                    return Body{i + 1, j};
                }
            }

            return std::nullopt;
        }

        depth += braces(lines[i]);
    }

    return std::nullopt;
}

// Whether the script has the block at all, however it is written.
bool hasBlock(const std::vector<std::string>& lines, std::string_view name)
{
    int depth = 0;

    for (const std::string& raw : lines)
    {
        const std::string_view line = code(raw);

        if (depth == 0 && line.starts_with(name) && trim(line.substr(name.size())).starts_with("{"))
        {
            return true;
        }

        depth += braces(raw);
    }

    return false;
}

// "Warlock.ShadowBolt if ..." -> "Warlock.ShadowBolt"; empty for a line that
// is no plain rule (a comment, an `if` block, a brace).
std::string_view actionOf(std::string_view line)
{
    const std::string_view rule = code(line);
    std::size_t end = 0;

    while (end < rule.size() && (std::isalnum(static_cast<unsigned char>(rule[end])) || rule[end] == '_' || rule[end] == '.'))
    {
        ++end;
    }

    const std::string_view action = rule.substr(0, end);
    return action.find('.') == std::string_view::npos || (end < rule.size() && !std::isspace(static_cast<unsigned char>(rule[end])) && rule[end] != '(') ? std::string_view{} : action;
}

// Swings and shots go on by themselves and never show as a cast.
bool unseeable(std::string_view action)
{
    return action == "AutoAttack" || action == "Attack" || action == "Shoot" || action == "AutoShot";
}

std::string indentOf(const std::vector<std::string>& lines, const Body& body)
{
    for (std::size_t i = body.first; i < body.last; ++i)
    {
        if (!trim(lines[i]).empty())
        {
            return lines[i].substr(0, lines[i].find_first_not_of(" \t"));
        }
    }

    return "    ";
}

std::string classOf(const std::vector<std::string>& lines)
{
    for (const std::string& raw : lines)
    {
        const std::string_view line = code(raw);

        if (line.starts_with("class"))
        {
            const auto open = line.find('"');
            const auto close = line.rfind('"');

            if (open != std::string_view::npos && close > open)
            {
                return std::string(line.substr(open + 1, close - open - 1));
            }
        }
    }

    return {};
}

std::optional<std::string> settingOf(const std::vector<std::string>& lines, std::string_view key)
{
    const auto body = blockOf(lines, "settings");

    for (std::size_t i = body ? body->first : 0; body && i < body->last; ++i)
    {
        const std::string_view line = code(lines[i]);
        const auto equals = line.find('=');

        if (equals != std::string_view::npos && trim(line.substr(0, equals)) == key)
        {
            return std::string(trim(line.substr(equals + 1)));
        }
    }

    return std::nullopt;
}

// Sets a setting where it stands, its name and spacing kept, or adds it.
void setSetting(std::vector<std::string>& lines, std::string_view key, int value)
{
    const auto body = blockOf(lines, "settings");

    if (!body)
    {
        return;
    }

    for (std::size_t i = body->first; i < body->last; ++i)
    {
        const auto equals = lines[i].find('=');

        if (equals != std::string::npos && trim(code(lines[i]).substr(0, code(lines[i]).find('='))) == key)
        {
            std::size_t from = equals + 1;

            while (from < lines[i].size() && lines[i][from] == ' ')
            {
                ++from;
            }

            std::size_t to = from;

            while (to < lines[i].size() && !std::isspace(static_cast<unsigned char>(lines[i][to])))
            {
                ++to;
            }

            lines[i].replace(from, to - from, std::to_string(value));
            return;
        }
    }

    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(body->last), std::format("{}{} = {}", indentOf(lines, *body), key, value));
}

// The lower and the upper end of the five-wide step a level falls in.
int stepFloor(double level)
{
    return static_cast<int>(std::floor(level / 5.0)) * 5;
}

int stepCeil(double level)
{
    return std::min(100, stepFloor(level) + 5);
}

struct Learned
{
    std::string action; // "Warlock.Corruption"
    std::string rule;   // the whole line, without its indent
    int rank = 0;       // what goes before what
    double order = 0.0;
    std::uint32_t weight = 0;
};

void sortLearned(std::vector<Learned>& rules)
{
    std::stable_sort(rules.begin(), rules.end(), [](const Learned& a, const Learned& b)
    {
        if (a.rank != b.rank)
        {
            return a.rank < b.rank;
        }

        if (a.order != b.order)
        {
            return a.order < b.order;
        }

        return a.weight > b.weight;
    });
}

std::string ruleOf(const std::string& action, const std::vector<std::string>& conditions)
{
    std::string rule = action;

    for (std::size_t i = 0; i < conditions.size(); ++i)
    {
        rule += i == 0 ? " if " : " and ";
        rule += conditions[i];
    }

    return rule;
}

// Rewrites a block's body: rules for spells the recorder has seen go, the
// learned ones come in where the first of them stood (or on top), and
// everything else stays as it is. Returns whether anything changed.
bool rewrite(std::vector<std::string>& lines, std::string_view block, const std::vector<Learned>& learned, const std::set<std::string>& seen, std::string_view onlyLearnedOf = {})
{
    auto body = blockOf(lines, block);

    if (!body)
    {
        // Written some other way: left alone. None at all: made, if there is something for it.
        if (hasBlock(lines, block) || learned.empty())
        {
            return false;
        }

        while (!lines.empty() && trim(lines.back()).empty())
        {
            lines.pop_back();
        }

        lines.emplace_back();
        lines.push_back(std::format("{} {{", block));
        lines.emplace_back("}");
        body = Body{lines.size() - 1, lines.size() - 1};
    }

    const std::string indent = indentOf(lines, *body);
    std::vector<std::string> out;
    std::optional<std::size_t> at;
    int depth = 0;
    bool dropped = false;

    for (std::size_t i = body->first; i < body->last; ++i)
    {
        const std::string_view action = actionOf(lines[i]);
        // With openers of the player's own, the class's other spells are not
        // opened with any more: they never did. What cannot be seen cast stays.
        const bool notTheirs = !onlyLearnedOf.empty() && !learned.empty() && action.starts_with(onlyLearnedOf) && !unseeable(action.substr(onlyLearnedOf.size()));

        // Inside an `if` block of the player's own: not ours to take apart.
        if (depth == 0 && (seen.contains(std::string(action)) || notTheirs))
        {
            at = at.value_or(out.size());
            dropped = true;
        }
        else
        {
            out.push_back(lines[i]);
        }

        depth += braces(lines[i]);
    }

    if (learned.empty() && !dropped)
    {
        return false;
    }

    std::vector<std::string> written;

    for (const Learned& one : learned)
    {
        written.push_back(indent + one.rule);
    }

    out.insert(out.begin() + static_cast<std::ptrdiff_t>(at.value_or(0)), written.begin(), written.end());

    if (std::equal(out.begin(), out.end(), lines.begin() + static_cast<std::ptrdiff_t>(body->first), lines.begin() + static_cast<std::ptrdiff_t>(body->last)))
    {
        return false;
    }

    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(body->first), lines.begin() + static_cast<std::ptrdiff_t>(body->last));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(body->first), out.begin(), out.end());
    return true;
}

void say(std::vector<std::string>& changes, std::string_view block, const std::vector<Learned>& learned)
{
    for (const Learned& one : learned)
    {
        changes.push_back(std::format("{}: {}", block, one.rule));
    }
}

} // namespace

std::string playstyleActionName(std::string_view spellName)
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
        {
            wordStart = true;
        }
    }

    return out;
}

TrainedScript writeTrainedRules(std::string_view script, const PlaystyleProfile& profile)
{
    TrainedScript result{std::string(script), {}};
    std::vector<std::string> lines = linesOf(script);
    const std::string className = classOf(lines);

    if (!profile.combatReady() || className.empty())
    {
        return result;
    }

    const bool usesMana = settingOf(lines, "usesMana").value_or("false") == "true";
    std::set<std::string> seen;
    std::uint32_t fightCasts = 0;

    for (const auto& [name, spell] : profile.spells)
    {
        seen.insert(className + "." + playstyleActionName(name));
        fightCasts += spell.inCombat;
    }

    std::vector<Learned> pull, combat, rest;
    const std::uint32_t pullLeast = std::max(kPullLeast, static_cast<std::uint32_t>(std::ceil(kPullShare * profile.fights)));
    const std::uint32_t rotationLeast = std::max(kRotationLeast, static_cast<std::uint32_t>(std::ceil(kRotationShare * fightCasts)));

    for (const auto& [name, spell] : profile.spells)
    {
        const std::string action = className + "." + playstyleActionName(name);

        if (spell.opened >= pullLeast)
        {
            // The one most often cast first goes first.
            pull.push_back({action, action, 0, -static_cast<double>(spell.openedFirst), spell.opened});
        }

        if (spell.inCombat >= rotationLeast)
        {
            std::vector<std::string> conditions;
            int rank = 2;

            // Only ever cast when hurt: a heal, a drain.
            if (spell.health.quantile(0.9) <= 70.0)
            {
                conditions.push_back(std::format("player.health < {}", stepCeil(spell.health.quantile(0.9))));
                rank = 0;
            }

            // Leaves its own debuff and is not cast onto it again: kept up, not spammed.
            const bool keptUp = spell.onTarget > 0 && spell.debuffed * 2 >= spell.onTarget && spell.refreshed * 4 <= spell.onTarget;

            if (keptUp)
            {
                conditions.push_back(std::format("not target.hasDebuff(\"{}\")", name));
                rank = std::min(rank, 1);

                // And not wasted on a mob about to die.
                if (const int floor = stepFloor(spell.targetHealth.quantile(0.1)); spell.onTarget >= 5 && floor >= 20)
                {
                    conditions.push_back(std::format("target.health > {}", floor));
                }
            }

            if (const int floor = stepFloor(spell.power.quantile(0.1)); usesMana && floor >= 15)
            {
                conditions.push_back(std::format("player.mana > {}", floor));
            }

            combat.push_back({action, ruleOf(action, conditions), rank, static_cast<double>(spell.orderSum) / static_cast<double>(spell.inCombat), spell.inCombat});
        }

        // Between fights, with mana to make up, and mana made up by it: Life
        // Tap, Evocation. A buff is cast at any mana and is the buffs block's;
        // a mount called at low mana gives none back.
        if (usesMana && spell.restored >= kRestLeast && spell.restored * 2 >= spell.outOfCombat && spell.power.quantile(0.9) <= 77.5)
        {
            std::vector<std::string> conditions{std::format("player.mana < {}", stepCeil(spell.power.quantile(0.9)))};

            if (const int floor = stepFloor(spell.health.quantile(0.1)); floor >= 20)
            {
                conditions.push_back(std::format("player.health > {}", floor));
            }

            rest.push_back({action, ruleOf(action, conditions), 0, 0.0, spell.outOfCombat});
        }
    }

    sortLearned(pull);
    sortLearned(combat);
    sortLearned(rest);

    if (rewrite(lines, "pull", pull, seen, className + "."))
    {
        say(result.changes, "pull", pull);

        if (pull.empty())
        {
            result.changes.emplace_back("pull: the spells you do not open with were taken out");
        }
    }

    if (rewrite(lines, "combat", combat, seen))
    {
        say(result.changes, "combat", combat);
    }

    if (rewrite(lines, "rest", rest, seen))
    {
        say(result.changes, "rest", rest);
    }

    // Sat down at this or less; up again at this or more.
    const auto level = [&](std::string_view key, const PlaystyleLevels& levels, bool upper)
    {
        if (levels.count() < kRestLeast)
        {
            return;
        }

        const int value = upper ? stepCeil(levels.quantile(0.5)) : stepFloor(levels.quantile(0.5));

        if (settingOf(lines, key) != std::to_string(value) && blockOf(lines, "settings"))
        {
            setSetting(lines, key, value);
            result.changes.push_back(std::format("settings: {} = {}", key, value));
        }
    };
    level("restHealth", profile.restHealth, true);
    level("restDoneHealth", profile.restDoneHealth, false);

    if (usesMana)
    {
        level("restMana", profile.restMana, true);
        level("restDoneMana", profile.restDoneMana, false);
    }

    if (result.changes.empty())
    {
        return result;
    }

    result.script.clear();

    for (const std::string& line : lines)
    {
        result.script += line;
        result.script += '\n';
    }

    return result;
}

} // namespace og
