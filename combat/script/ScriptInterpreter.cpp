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
 *  File: ScriptInterpreter.cpp
 *
 *  Desc:
 *  Implementation of ScriptInterpreter (combat execution, rotation
 *  scripting, and class behaviors).
 *
 *
 */
#include "og/combat/script/ScriptInterpreter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace og::script
{

namespace
{

std::string typeName(const Value& v)
{
    switch (v.index())
    {
    case 0:
        return "nil";
    case 1:
        return "bool";
    case 2:
        return "number";
    case 3:
        return "string";
    case 4:
        return "object";
    case 5:
        return "function";
    }

    return "?";
}

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

bool valuesEqual(const Value& a, const Value& b)
{
    if (a.index() != b.index())
    {
        return false;
    }

    if (const auto* s = std::get_if<std::string>(&a))
    {
        return iequals(*s, std::get<std::string>(b));
    }

    return a == b;
}

Value evalBinary(const Expr& e, IEnvironment& env)
{
    if (e.op == Op::And)
    {
        if (!truthy(evaluate(*e.lhs, env), env))
        {
            return false;
        }

        return truthy(evaluate(*e.rhs, env), env);
    }

    if (e.op == Op::Or)
    {
        if (truthy(evaluate(*e.lhs, env), env))
        {
            return true;
        }

        return truthy(evaluate(*e.rhs, env), env);
    }

    const Value l = evaluate(*e.lhs, env);
    const Value r = evaluate(*e.rhs, env);

    switch (e.op)
    {
    case Op::Eq:
        return valuesEqual(l, r);
    case Op::Ne:
        return !valuesEqual(l, r);
    case Op::Add:
        if (std::holds_alternative<std::string>(l) || std::holds_alternative<std::string>(r))
        {
            auto str = [](const Value& v)
            {
                if (const auto* s = std::get_if<std::string>(&v))
                {
                    return *s;
                }

                return valueToString(v);
            };
            return str(l) + str(r);
        }

        return toNumber(l, e.lhs->loc) + toNumber(r, e.rhs->loc);
    default:
        break;
    }

    if ((e.op == Op::Lt || e.op == Op::Le || e.op == Op::Gt || e.op == Op::Ge) && std::holds_alternative<std::string>(l) && std::holds_alternative<std::string>(r))
    {
        const auto& a = std::get<std::string>(l);
        const auto& b = std::get<std::string>(r);

        switch (e.op)
        {
        case Op::Lt:
            return a < b;
        case Op::Le:
            return a <= b;
        case Op::Gt:
            return a > b;
        default:
            return a >= b;
        }
    }

    const double a = toNumber(l, e.lhs->loc);
    const double b = toNumber(r, e.rhs->loc);

    switch (e.op)
    {
    case Op::Lt:
        return a < b;
    case Op::Le:
        return a <= b;
    case Op::Gt:
        return a > b;
    case Op::Ge:
        return a >= b;
    case Op::Sub:
        return a - b;
    case Op::Mul:
        return a * b;
    case Op::Div:
        if (b == 0.0)
        {
            throw RuntimeError("division by zero", e.loc);
        }

        return a / b;
    case Op::Mod:
        if (b == 0.0)
        {
            throw RuntimeError("modulo by zero", e.loc);
        }

        return std::fmod(a, b);
    default:
        break;
    }

    throw RuntimeError("invalid binary operator", e.loc);
}

} // namespace

double toNumber(const Value& v, SourceLoc loc)
{
    if (const auto* d = std::get_if<double>(&v))
    {
        return *d;
    }

    if (const auto* b = std::get_if<bool>(&v))
    {
        return *b ? 1.0 : 0.0;
    }

    if (std::holds_alternative<std::monostate>(v))
    {
        return 0.0;
    }

    throw RuntimeError(std::format("expected a number, got {} ({})", typeName(v), valueToString(v)), loc);
}

std::string toStringArg(const Value& v, SourceLoc loc)
{
    if (const auto* s = std::get_if<std::string>(&v))
    {
        return *s;
    }

    if (std::holds_alternative<double>(v))
    {
        return valueToString(v);
    }

    throw RuntimeError(std::format("expected a string, got {}", typeName(v)), loc);
}

bool truthy(const Value& v, IEnvironment& env)
{
    struct Visitor
    {
        IEnvironment& env;
        bool operator()(std::monostate) const
        {
            return false;
        }

        bool operator()(bool b) const
        {
            return b;
        }

        bool operator()(double d) const
        {
            return d != 0.0;
        }

        bool operator()(const std::string& s) const
        {
            return !s.empty();
        }

        bool operator()(const ObjectRef& o) const
        {
            return env.exists(o);
        }

        bool operator()(const FunctionRef&) const
        {
            return true;
        }
    };
    return std::visit(Visitor{env}, v);
}

Value evaluate(const Expr& e, IEnvironment& env)
{
    switch (e.kind)
    {
    case Expr::Kind::Literal:
        return e.literal;

    case Expr::Kind::Name:
    {
        auto v = env.name(e.name);

        if (!v)
        {
            throw RuntimeError(std::format("unknown name '{}'", e.name), e.loc);
        }

        return *v;
    }

    case Expr::Kind::Member:
    {
        const Value obj = evaluate(*e.lhs, env);
        const auto* ref = std::get_if<ObjectRef>(&obj);

        if (!ref)
        {
            throw RuntimeError(std::format("'{}' is not an object", valueToString(obj)), e.loc);
        }

        auto v = env.member(*ref, e.name);

        if (!v)
        {
            throw RuntimeError(std::format("'{}' has no member '{}'", ref->name, e.name), e.loc);
        }

        return *v;
    }

    case Expr::Kind::Call:
    {
        const Value callee = evaluate(*e.lhs, env);
        const auto* fn = std::get_if<FunctionRef>(&callee);

        if (!fn)
        {
            throw RuntimeError(std::format("'{}' is not callable", valueToString(callee)), e.loc);
        }

        std::vector<Value> args;
        args.reserve(e.args.size());

        for (const auto& a : e.args)
        {
            args.push_back(evaluate(*a, env));
        }

        try
        {
            return env.call(*fn, args);
        }
        catch (RuntimeError& err)
        {
            if (err.loc.line == 0)
            {
                err.loc = e.loc;
            }

            throw;
        }
    }

    case Expr::Kind::Unary:
    {
        const Value v = evaluate(*e.lhs, env);

        if (e.op == Op::Not)
        {
            return !truthy(v, env);
        }

        return -toNumber(v, e.lhs->loc);
    }

    case Expr::Kind::Binary:
        return evalBinary(e, env);
    }

    throw RuntimeError("invalid expression", e.loc);
}

} // namespace og::script
