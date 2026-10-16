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
 *  File: ScriptInterpreter.h
 *
 *  Desc:
 *  Declarations and interface for ScriptInterpreter (combat execution,
 *  rotation scripting, and class behaviors).
 *
 *
 */
#pragma once

#include "og/combat/script/ScriptAst.h"

#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace og::script
{

struct RuntimeError : std::runtime_error
{
    SourceLoc loc;
    explicit RuntimeError(const std::string& msg, SourceLoc l = {}) : std::runtime_error(msg), loc(l)
    {
    }
};

// What a script can see. The engine implementation (SnapshotEnvironment)
// exposes player/target/pet from the GameSnapshot; tests can provide fakes.
class IEnvironment
{
public:
    virtual ~IEnvironment() = default;

    // Bare identifier: objects (player/target/pet), globals (adds), settings,
    // global functions (ready). nullopt = unknown name.
    virtual std::optional<Value> name(std::string_view name) = 0;
    // object.member: a value, or a FunctionRef for methods. nullopt = unknown member.
    virtual std::optional<Value> member(const ObjectRef& object, std::string_view member) = 0;
    // Throws RuntimeError on bad arguments / unknown function.
    virtual Value call(const FunctionRef& fn, const std::vector<Value>& args) = 0;
    // Truthiness of an object reference ("if target").
    virtual bool exists(const ObjectRef& object) = 0;
};

Value evaluate(const Expr& expr, IEnvironment& env);
bool truthy(const Value& value, IEnvironment& env);

// Helpers for environment implementations.
double toNumber(const Value& v, SourceLoc loc = {});
std::string toStringArg(const Value& v, SourceLoc loc = {});

} // namespace og::script
