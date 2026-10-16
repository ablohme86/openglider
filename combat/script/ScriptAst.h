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
 *  File: ScriptAst.h
 *
 *  Desc:
 *  Declarations and interface for ScriptAst (combat execution, rotation
 *  scripting, and class behaviors).
 *
 *
 */
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace og::script
{

struct SourceLoc
{
    std::size_t line = 0;
    std::size_t column = 0;
};

struct Diagnostic
{
    SourceLoc loc;
    std::string message;
    bool warning = false;
    std::string toString(const std::string& file = {}) const;
};

// Runtime values.
struct ObjectRef
{
    std::string name; // "player", "target", "pet"
    bool operator==(const ObjectRef&) const = default;
};
struct FunctionRef
{
    std::string object; // empty for global functions
    std::string name;
    bool operator==(const FunctionRef&) const = default;
};
using Value = std::variant<std::monostate, bool, double, std::string, ObjectRef, FunctionRef>;

std::string valueToString(const Value& v);

enum class Op
{
    Or,
    And,
    Not,
    Neg,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr
{
    enum class Kind
    {
        Literal,
        Name,
        Member,
        Call,
        Unary,
        Binary
    };

    Kind kind = Kind::Literal;
    SourceLoc loc;
    Value literal;             // Literal
    std::string name;          // Name, Member (member name)
    Op op = Op::Add;           // Unary, Binary
    ExprPtr lhs;               // Member object, Call callee, Unary operand, Binary left
    ExprPtr rhs;               // Binary right
    std::vector<ExprPtr> args; // Call
};

struct Rule
{
    std::string action;
    ExprPtr argument;          // player.WalkBackward(10): the yards; null for an action
    ExprPtr condition;         // null = always
    std::string conditionText; // the condition as written, for the log
    SourceLoc loc;
};

struct Block
{
    std::string name;
    std::vector<Rule> rules;
    SourceLoc loc;
};

struct Setting
{
    std::string name;
    ExprPtr value;
    SourceLoc loc;
};

struct Program
{
    std::string className;
    std::vector<Setting> settings;
    std::vector<Block> blocks;

    const Block* block(const std::string& name) const
    {
        for (const auto& b : blocks)
        {
            if (b.name == name)
            {
                return &b;
            }
        }

        return nullptr;
    }
};

} // namespace og::script
