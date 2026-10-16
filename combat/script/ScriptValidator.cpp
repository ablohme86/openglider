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
 *  File: ScriptValidator.cpp
 *
 *  Desc:
 *  Implementation of ScriptValidator (combat execution, rotation
 *  scripting, and class behaviors).
 *
 *
 */
#include "og/combat/script/ScriptValidator.h"

#include "og/combat/ICombatClass.h"

#include "og/combat/script/SnapshotEnvironment.h"

#include <algorithm>
#include <format>

namespace og::script
{

namespace
{

bool in(const std::vector<std::string_view>& list, std::string_view s)
{
    return std::find(list.begin(), list.end(), s) != list.end();
}

class Checker
{
public:
    Checker(const Program& p, std::vector<Diagnostic>& out) : out_(out)
    {
        for (const auto& s : p.settings)
        {
            settings_.insert(s.name);
        }
    }

    void checkExpr(const Expr& e)
    {
        switch (e.kind)
        {
        case Expr::Kind::Literal:
            return;
        case Expr::Kind::Name:
            if (!isKnownName(e.name))
            {
                error(e.loc, std::format("unknown name '{}'", e.name));
            }
            else if (in(SnapshotEnvironment::globalFunctions(), e.name))
            {
                error(e.loc, std::format("'{}' is a function; call it like {}(...)", e.name, e.name));
            }

            return;
        case Expr::Kind::Member:
            checkMember(e, false);
            return;
        case Expr::Kind::Call:
        {
            for (const auto& a : e.args)
            {
                checkExpr(*a);
            }

            if (e.args.size() != 1)
            {
                error(e.loc, std::format("functions take exactly 1 argument, got {}", e.args.size()));
            }

            const Expr& callee = *e.lhs;

            if (callee.kind == Expr::Kind::Name)
            {
                if (!in(SnapshotEnvironment::globalFunctions(), callee.name))
                {
                    error(callee.loc, std::format("unknown function '{}'", callee.name));
                }
                else if (callee.name == "ready" || callee.name == "bound" || callee.name == "cooldown")
                {
                    checkActionArg(e);
                }
            }
            else if (callee.kind == Expr::Kind::Member)
            {
                checkMember(callee, true);
            }
            else
            {
                error(e.loc, "expression is not callable");
            }

            return;
        }

        case Expr::Kind::Unary:
            checkExpr(*e.lhs);
            return;
        case Expr::Kind::Binary:
            checkExpr(*e.lhs);
            checkExpr(*e.rhs);
            return;
        }
    }

    std::vector<std::string> referencedActions;

private:
    bool isKnownName(const std::string& n) const
    {
        return in(SnapshotEnvironment::objectNames(), n) || in(SnapshotEnvironment::constantGroups(), n) || in(SnapshotEnvironment::globalValues(), n) || in(SnapshotEnvironment::globalFunctions(), n) || settings_.contains(n);
    }

    void checkActionArg(const Expr& call)
    {
        if (call.args.size() == 1 && call.args[0]->kind == Expr::Kind::Literal)
        {
            if (const auto* s = std::get_if<std::string>(&call.args[0]->literal))
            {
                referencedActions.push_back(*s);
            }
        }
    }

    void checkMember(const Expr& e, bool called)
    {
        const Expr& obj = *e.lhs;

        if (obj.kind == Expr::Kind::Name && !isKnownName(obj.name))
        {
            error(obj.loc, std::format("unknown name '{}' (did you mean player, target or pet?)", obj.name));
            return;
        }

        if (obj.kind == Expr::Kind::Name && in(SnapshotEnvironment::constantGroups(), obj.name))
        {
            const auto names = SnapshotEnvironment::constantsOf(obj.name);

            if (called)
            {
                error(e.loc, std::format("'{}.{}' is a value, not a method", obj.name, e.name));
            }
            else if (!in(names, e.name))
            {
                std::string known;

                for (const auto n : names)
                {
                    known += std::format("{}{}", known.empty() ? "" : ", ", n);
                }

                error(e.loc, std::format("'{}' has no value '{}' (known: {})", obj.name, e.name, known));
            }

            return;
        }

        if (obj.kind != Expr::Kind::Name || !in(SnapshotEnvironment::objectNames(), obj.name))
        {
            checkExpr(obj);
            error(e.loc, "members can only be accessed on player, target or pet");
            return;
        }

        const bool isMethod = in(SnapshotEnvironment::unitMethods(), e.name) || (obj.name == "player" && in(SnapshotEnvironment::playerMethods(), e.name));
        const bool isProp = in(SnapshotEnvironment::unitMembers(), e.name) || (obj.name == "player" && in(SnapshotEnvironment::playerMembers(), e.name)) || (obj.name == "target" && in(SnapshotEnvironment::targetMembers(), e.name));

        if (!isMethod && !isProp)
        {
            error(e.loc, std::format("'{}' has no member '{}'", obj.name, e.name));
        }
        else if (isMethod && !called)
        {
            error(e.loc, std::format("'{}.{}' is a method; call it like {}.{}(\"name\")", obj.name, e.name, obj.name, e.name));
        }
        else if (isProp && called)
        {
            error(e.loc, std::format("'{}.{}' is a property, not a method", obj.name, e.name));
        }
    }

    void error(SourceLoc loc, std::string msg)
    {
        out_.push_back({loc, std::move(msg), false});
    }

    std::vector<Diagnostic>& out_;
    std::set<std::string> settings_;
};

} // namespace

std::vector<Diagnostic> validate(const Program& program, const ActionMap* actions)
{
    std::vector<Diagnostic> out;
    Checker checker(program, out);

    if (program.className.empty())
    {
        out.push_back({{1, 1}, "missing 'class \"Name\"' declaration", true});
    }

    for (const auto& s : program.settings)
    {
        if (s.value)
        {
            checker.checkExpr(*s.value);
        }
    }

    std::set<std::string> seenBlocks;

    for (const auto& block : program.blocks)
    {
        if (!knownBlocks().contains(block.name))
        {
            out.push_back({block.loc, std::format("unknown block '{}' (known: emergency, pull, combat, rest, buffs)", block.name), true});
        }

        if (!seenBlocks.insert(block.name).second)
        {
            out.push_back({block.loc, std::format("duplicate block '{}'", block.name), false});
        }

        for (const auto& rule : block.rules)
        {
            if (rule.condition)
            {
                checker.checkExpr(*rule.condition);
            }

            // player.WalkBackward(10): a step, in a fight, of so many yards.
            if (scriptMoveWay(rule.action))
            {
                if (!rule.argument)
                {
                    out.push_back({rule.loc, std::format("'{}' needs how many yards, like {}(5)", rule.action, rule.action), false});
                }
                else
                {
                    checker.checkExpr(*rule.argument);

                    if (rule.argument->kind == Expr::Kind::Literal && std::holds_alternative<double>(rule.argument->literal))
                    {
                        const double yards = std::get<double>(rule.argument->literal);

                        if (yards <= 0 || yards > kScriptMoveLongest)
                        {
                            out.push_back({rule.loc, std::format("'{}': {} yards - from 1 to {:.0f}", rule.action, yards, kScriptMoveLongest), false});
                        }
                    }
                }

                if (block.name != "combat" && block.name != "emergency")
                {
                    out.push_back({rule.loc, std::format("'{}' is taken in a fight only: in combat or emergency", rule.action), false});
                }

                continue;
            }

            if (rule.argument)
            {
                out.push_back({rule.loc, std::format("'{}' takes no argument (only player.WalkForward, WalkBackward, StrafeLeft and StrafeRight do)", rule.action), false});
            }

            if (rule.action.starts_with("player."))
            {
                out.push_back({rule.loc, std::format("'{}' is no move: player.WalkForward, WalkBackward, StrafeLeft or StrafeRight", rule.action), false});
                continue;
            }

            // Known when actions.yaml lists it (a key comes from the action bar).
            if (actions && !actions->settingsFor(rule.action))
            {
                out.push_back({rule.loc, std::format("action '{}' has no built-in or configured metadata", rule.action), true});
            }
        }
    }

    if (actions)
    {
        for (const auto& a : checker.referencedActions)
        {
            if (!actions->settingsFor(a))
            {
                out.push_back({{0, 0}, std::format("action '{}' used in ready()/bound() has no built-in or configured metadata", a), true});
            }
        }
    }

    return out;
}

} // namespace og::script
