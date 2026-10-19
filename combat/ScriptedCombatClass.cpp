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
 *  File: ScriptedCombatClass.cpp
 *
 *  Desc:
 *  Implementation of ScriptedCombatClass (combat execution, rotation
 *  scripting, and class behaviors).
 *
 *
 */
#include "og/combat/ScriptedCombatClass.h"

#include "og/combat/script/ScriptInterpreter.h"
#include "og/combat/script/ScriptParser.h"
#include "og/combat/script/ScriptValidator.h"
#include "og/common/FileUtil.h"
#include "og/state/Shapeshift.h"

#include <algorithm>
#include <format>

namespace og
{

namespace
{

// Settings may only reference earlier settings (constants), so they are
// evaluated with an environment that knows nothing about the game.
class ConstantEnvironment final : public script::IEnvironment
{
public:
    explicit ConstantEnvironment(const script::SettingsMap& s) : settings_(s)
    {
    }

    std::optional<script::Value> name(std::string_view n) override
    {
        if (auto it = settings_.find(n); it != settings_.end())
        {
            return it->second;
        }

        return std::nullopt;
    }

    std::optional<script::Value> member(const script::ObjectRef&, std::string_view) override
    {
        return std::nullopt;
    }

    script::Value call(const script::FunctionRef& fn, const std::vector<script::Value>&) override
    {
        throw script::RuntimeError(std::format("'{}' cannot be used in settings", fn.name));
    }

    bool exists(const script::ObjectRef&) override
    {
        return false;
    }

private:
    const script::SettingsMap& settings_;
};

void applySetting(CombatClassSettings& s, const std::string& name, const script::Value& v)
{
    auto num = [&](float& field)
    {
        if (const auto* d = std::get_if<double>(&v))
        {
            field = static_cast<float>(*d);
        }
    };

    // pullRange is gone: the pull spells' ranges say it (pullReach). Older
    // scripts that still set it load as before.
    if (name == "combatRange")
    {
        num(s.combatRange);
    }
    else if (name == "restHealth")
    {
        num(s.restHealth);
    }
    else if (name == "restMana")
    {
        num(s.restMana);
    }
    else if (name == "restDoneHealth")
    {
        num(s.restDoneHealth);
    }
    else if (name == "restDoneMana")
    {
        num(s.restDoneMana);
    }
    else if (name == "usesMana")
    {
        if (const auto* b = std::get_if<bool>(&v))
        {
            s.usesMana = *b;
        }
    }
}

bool isAutoAttackAction(std::string_view action)
{
    if (action == "AutoAttack" || action == "Attack")
    {
        return true;
    }

    if (action.ends_with(".AutoAttack") || action.ends_with(".Attack"))
    {
        return true;
    }

    return false;
}

// Auto Shot and wand Shoot repeat on a swing timer: their "cooldown" is the
// gap between shots, not a sign that the ranged option is gone.
bool isAutoRepeatAction(std::string_view action)
{
    return action.ends_with(".AutoShot") || action.ends_with(".Shoot");
}

// The client reports the wand stopped a moment after the press. Pressing Shoot
// again before then would turn it back on.
constexpr Duration kWandStopSettle{1000};

} // namespace

bool ScriptedCombatClass::LoadResult::hasErrors() const
{
    for (const auto& d : diagnostics)
    {
        if (!d.warning)
        {
            return true;
        }
    }

    return false;
}

std::string ScriptedCombatClass::LoadResult::report(const std::string& file) const
{
    std::string out;

    for (const auto& d : diagnostics)
    {
        out += d.toString(file);
        out += '\n';
    }

    return out;
}

ScriptedCombatClass::LoadResult ScriptedCombatClass::fromSource(std::string_view source, const ActionMap* actions)
{
    LoadResult result;
    auto parsed = script::parse(source);
    result.diagnostics = std::move(parsed.errors);

    if (!parsed.program)
    {
        return result;
    }

    auto validation = script::validate(*parsed.program, actions);
    result.diagnostics.insert(result.diagnostics.end(), validation.begin(), validation.end());

    if (result.hasErrors())
    {
        return result;
    }

    std::unique_ptr<ScriptedCombatClass> cls(new ScriptedCombatClass(std::move(*parsed.program)));

    for (const auto& s : cls->program_.settings)
    {
        try
        {
            ConstantEnvironment env(cls->settingValues_);
            script::Value v = script::evaluate(*s.value, env);
            applySetting(cls->settings_, s.name, v);
            cls->settingValues_[s.name] = std::move(v);
        }
        catch (const script::RuntimeError& e)
        {
            result.diagnostics.push_back({s.loc, std::format("setting '{}': {}", s.name, e.what()), false});
        }
    }

    if (result.hasErrors())
    {
        return result;
    }

    result.combatClass = std::move(cls);
    return result;
}

ScriptedCombatClass::LoadResult ScriptedCombatClass::fromFile(const std::filesystem::path& path, const ActionMap* actions)
{
    auto text = readTextFile(path);

    if (!text)
    {
        LoadResult r;
        r.diagnostics.push_back({{0, 0}, std::format("cannot read {}", path.string()), false});
        return r;
    }

    return fromSource(*text, actions);
}

void ScriptedCombatClass::reportRuntimeError(Logger& log, const script::SourceLoc& loc, const std::string& message)
{
    if (reportedErrors_.insert({loc.line, loc.column}).second)
    {
        log.warn(logcat::Script, "{} line {}:{}: {} (rule skipped)", name(), loc.line, loc.column, message);
    }
}

const script::Rule* ScriptedCombatClass::select(std::string_view blockName, CombatContext& ctx, std::size_t* ruleIndex, const std::function<bool(std::string_view)>* allowed, bool ignoreToggleCooldown)
{
    const script::Block* block = program_.block(std::string(blockName));

    if (!block)
    {
        return nullptr;
    }

    script::SnapshotEnvironment env(ctx.snapshot, &ctx.actions, &settingValues_, ctx.combatSeconds, cachedSpellLookup(ctx), ctx.instantSpells);
    const bool isPull = blockName == "pull";

    for (std::size_t i = 0; i < block->rules.size(); ++i)
    {
        if (isPull && pullExecutedRules_.contains(i))
        {
            continue;
        }

        const auto& rule = block->rules[i];

        if (allowed && !(*allowed)(rule.action))
        {
            continue;
        }

        // A step (player.WalkBackward(10)): no key, no cooldown - taken where
        // the fight can take it and the condition holds.
        if (scriptMoveWay(rule.action))
        {
            if (!ctx.moveOrder || ctx.snapshot.player.isCasting || ctx.actions.isCasting())
            {
                continue;
            }

            try
            {
                if (!rule.condition || script::truthy(script::evaluate(*rule.condition, env), env))
                {
                    if (ruleIndex)
                    {
                        *ruleIndex = i;
                    }

                    return &rule;
                }
            }
            catch (const script::RuntimeError& e)
            {
                reportRuntimeError(ctx.log, e.loc.line ? e.loc : rule.loc, e.what());
            }

            continue;
        }

        // Report missing actions once so a skipped rule is visible.
        if (!ctx.actions.hasBinding(rule.action))
        {
            if (reportedUnbound_.insert(rule.action).second)
            {
                // A potion, food or drink is pressed where it sits on an action bar.
                if (rule.action == "Player.HealthPotion" || rule.action == "Player.ManaPotion" || rule.action == "Player.Eat" || rule.action == "Player.Drink")
                {
                    ctx.log.info(logcat::Action, "{}: no {} on an action bar - put one there for '{}' to use it", blockName, rule.action == "Player.HealthPotion" ? "healing potion or healthstone" : rule.action == "Player.ManaPotion" ? "mana potion" : rule.action == "Player.Eat" ? "food" : "drink", rule.action);
                }
                else
                {
                    ctx.log.warn(logcat::Action, "{}: '{}' is not on an action bar (ignoring until spell is learned)", blockName, rule.action);
                }
            }

            continue;
        }

        if (!ctx.actions.isReady(rule.action) && !(ignoreToggleCooldown && (isAutoRepeatAction(rule.action) || isAutoAttackAction(rule.action))))
        {
            continue;
        }

        // A bar slot holding an item the bags no longer have presses nothing:
        // the slot with the most left, of those holding something for it.
        if (ctx.actions.itemId(rule.action) != 0 && ctx.actions.chooseItemSlot(rule.action, [&](std::uint32_t item) { return ctx.snapshot.player.itemCount(std::to_string(item)); }) == 0)
        {
            continue;
        }

        if (!characterKnows(rule.action, ctx))
        {
            continue;
        }

        if (!inRange(rule.action, ctx))
        {
            continue;
        }

        if (ctx.snapshot.player.isAutoAttacking && isAutoAttackAction(rule.action))
        {
            continue;
        }

        // Pressing Shoot again while the wand fires turns it off.
        if (ctx.snapshot.player.isShooting && isAutoRepeatAction(rule.action))
        {
            continue;
        }

        // So does a form or Stealth the character is in; its stance does nothing.
        if (const ShapeshiftSpell* shift = shapeshiftFor(rule.action); shift && alreadyIn(*shift, ctx.snapshot.player.auras))
        {
            continue;
        }

        if (!rule.condition)
        {
            if (!affordable(rule.action, ctx))
            {
                continue;
            }

            if (ruleIndex)
            {
                *ruleIndex = i;
            }

            return &rule;
        }

        try
        {
            if (script::truthy(script::evaluate(*rule.condition, env), env))
            {
                // Waited for, not pressed, until there is enough for it: the
                // rules after it may fire meanwhile.
                if (!affordable(rule.action, ctx))
                {
                    continue;
                }

                if (ruleIndex)
                {
                    *ruleIndex = i;
                }

                return &rule;
            }
        }
        catch (const script::RuntimeError& e)
        {
            reportRuntimeError(ctx.log, e.loc.line ? e.loc : rule.loc, e.what());
        }
    }

    return nullptr;
}

std::optional<ScriptedCombatClass::Shortfall> ScriptedCombatClass::shortOf(const std::string& action, const CombatContext& ctx) const
{
    const PlayerState& p = ctx.snapshot.player;

    for (const SpellCost& cost : ctx.actions.costOf(action))
    {
        // Mana, rage, focus, energy; combo points the rules see to themselves.
        if (cost.power > static_cast<std::uint8_t>(PowerType::Energy))
        {
            continue;
        }

        const auto type = static_cast<PowerType>(cost.power);
        int have = 0;

        if (p.maxPowerOf(type) > 0)
        {
            have = p.powerOf(type);
        }
        else if (p.powerType == type)
        {
            have = p.power;
        }
        else
        {
            continue; // a power the character's state does not show
        }

        if (have < cost.amount)
        {
            return Shortfall{cost.amount, have, cost.power};
        }
    }

    return std::nullopt;
}

bool ScriptedCombatClass::affordable(const std::string& action, CombatContext& ctx)
{
    if (const auto shortfall = shortOf(action, ctx))
    {
        if (waitingForPower_.insert(action).second)
        {
            static constexpr const char* kNames[] = {"mana", "rage", "focus", "energy"};
            ctx.log.debug(logcat::Combat, "{}: waiting for {} {} ({} now)", action, shortfall->amount, kNames[shortfall->power], shortfall->have);
        }

        return false;
    }

    waitingForPower_.erase(action);
    return true;
}

bool ScriptedCombatClass::inRange(const std::string& action, CombatContext& ctx) const
{
    ActionRange range = ctx.actions.range(action);

    // The client gives its Attack no range. Turned on from afar it swings at
    // nothing: a warrior with Charge on cooldown switched it on at 20 yards
    // and stood waiting there, as no pull rule was left to fire closer.
    if (!range.limited() && isAutoAttackAction(action))
    {
        range = ActionRange{0.0f, 5.0f};
    }

    if (!range.limited())
    {
        return true; // no range configured: the rule decides
    }

    // Nothing to be in range of. Self-casts and buffs are never given a range,
    // so this only affects rules that need a target.
    const UnitState* target = ctx.snapshot.target();

    if (!target)
    {
        return true;
    }

    // The server keeps a minimum range between the two bodies, not their
    // middles: each one's combat reach (some 1.5 yd for a player or an orc)
    // comes on top. A hunter 9 yd from a Blackrock Outrunner was too close
    // for Auto Shot and Serpent Sting, and pressed them for 9 s.
    if (range.min > 0.0f)
    {
        range.min += kBodiesReach;
    }

    return range.covers(distance2D(ctx.snapshot.player.position, target->position));
}

bool ScriptedCombatClass::onlyMeleeLeft(const script::Block& block, CombatContext& ctx)
{
    // Judged as before the first swing, so "if not player.autoAttacking" keeps
    // the bot in melee once it has started.
    GameSnapshot snapshot = ctx.snapshot;
    snapshot.player.isAutoAttacking = false;
    script::SnapshotEnvironment env(snapshot, &ctx.actions, &settingValues_, ctx.combatSeconds, cachedSpellLookup(ctx), ctx.instantSpells);
    const auto wanted = [&](const script::Rule& rule)
    {
        if (!rule.condition)
        {
            return true;
        }

        try
        {
            return script::truthy(script::evaluate(*rule.condition, env), env);
        }
        catch (const script::RuntimeError&)
        {
            return false; // select() reports it
        }
    };
    bool melee = false;

    for (const auto& rule : block.rules)
    {
        if (!ctx.actions.hasBinding(rule.action) || !characterKnows(rule.action, ctx))
        {
            continue;
        }

        if (isAutoAttackAction(rule.action))
        {
            melee = melee || wanted(rule);
            continue;
        }

        const ActionRange range = ctx.actions.range(rule.action);
        const bool reachesFarther = range.limited() ? range.max > 5.0f : isAutoRepeatAction(rule.action) || ctx.actions.castTime(rule.action) > Duration(0);

        if (!reachesFarther)
        {
            continue;
        }

        if (!isAutoRepeatAction(rule.action) && !ctx.actions.isReadyIgnoringGlobalCooldown(rule.action))
        {
            continue;
        }

        // Out of mana for it, where its cost is known: it reaches nowhere.
        if (shortOf(rule.action, ctx))
        {
            continue;
        }

        if (wanted(rule))
        {
            return false;
        }
    }

    return melee;
}

bool ScriptedCombatClass::wantsMeleeSwing(CombatContext& ctx)
{
    const auto* block = program_.block("combat");
    return block && onlyMeleeLeft(*block, ctx);
}

float ScriptedCombatClass::pullReach(CombatContext& ctx)
{
    // A binding that carries no range: the common reach of a ranged spell. A
    // melee opener is found anyway, as the pull approach walks in while a rule
    // fires closer.
    constexpr float kUnrangedReach = 30.0f;
    const auto* block = program_.block("pull");

    if (!block)
    {
        return 5.0f;
    }

    float reach = 0;

    for (std::size_t i = 0; i < block->rules.size(); ++i)
    {
        const auto& rule = block->rules[i];

        if (!ctx.actions.hasBinding(rule.action) || !characterKnows(rule.action, ctx))
        {
            continue;
        }

        const ActionRange range = ctx.actions.range(rule.action);

        if (range.limited())
        {
            reach = std::max(reach, range.max);
        }
        else
        {
            reach = std::max(reach, isAutoAttackAction(rule.action) ? 5.0f : kUnrangedReach);
        }
    }

    return reach > 0 ? reach : 5.0f;
}

bool ScriptedCombatClass::isRanged() const
{
    if (settings_.combatRange > 5.0f)
    {
        return true;
    }

    if (const auto* block = program_.block("pull"))
    {
        for (const auto& rule : block->rules)
        {
            if (!isAutoAttackAction(rule.action))
            {
                return true;
            }
        }
    }

    return false;
}

float ScriptedCombatClass::combatRange(CombatContext& ctx)
{
    const auto* block = program_.block("combat");

    if (!block)
    {
        return settings_.combatRange;
    }

    // A caster out of mana with no wand walks in to hit with its weapon.
    if (onlyMeleeLeft(*block, ctx))
    {
        return std::min(settings_.combatRange, 4.5f);
    }

    float reach = 0;
    float availableReach = 0;
    bool hasMelee = false;
    bool unrangedCast = false;

    for (const auto& rule : block->rules)
    {
        if (!ctx.actions.hasBinding(rule.action) || !characterKnows(rule.action, ctx))
        {
            continue;
        }

        const ActionRange range = ctx.actions.range(rule.action);

        if (range.limited())
        {
            reach = std::max(reach, range.max);
            const bool melee = range.max <= 5.0f;
            hasMelee = hasMelee || melee;

            // A ranged spell counts while it is ready and paid for: one
            // short of rage or mana keeps no warrior waiting for a runner.
            if (melee || isAutoRepeatAction(rule.action) || (ctx.actions.isReadyIgnoringGlobalCooldown(rule.action) && !shortOf(rule.action, ctx)))
            {
                availableReach = std::max(availableReach, range.max);
            }
        }
        else if (isAutoAttackAction(rule.action))
        {
            reach = std::max(reach, 5.0f);
            availableReach = std::max(availableReach, 5.0f);
            hasMelee = true;
        }
        else if (ctx.actions.castTime(rule.action) > Duration(0))
        {
            // Older/custom bindings may not carry a cast spell's range, and
            // heals on ourselves have none. Such a spell never makes it melee.
            unrangedCast = true;
        }

        // Unranged instant actions include self-buffs; they add no reach.
    }

    // A ranged ability on its own cooldown must not keep a melee rotation
    // waiting for a runner. Ignore the GCD so repeatable ranged spells retain
    // their reach between casts; melee abilities and auto-repeat shots retain
    // reach while recharging.
    if (hasMelee)
    {
        reach = availableReach;
    }

    if (reach <= 0 || (reach <= 5.0f && unrangedCast))
    {
        return settings_.combatRange;
    }

    if (reach <= 5.0f)
    {
        return std::min({settings_.combatRange, reach, 4.5f});
    }

    // A ranged rotation fights from as far as its spells reach, past
    // combatRange too: with combatRange 25, Mind Blast's 30 yards walked the
    // priest in for nothing. A melee class (combatRange 5 or less) casts its
    // ranged spells from melee, so combatRange still holds it there.
    return settings_.combatRange <= 5.0f ? settings_.combatRange : reach;
}

bool ScriptedCombatClass::characterKnows(const std::string& action, CombatContext& ctx)
{
    const auto& known = ctx.snapshot.player.knownSpells;

    if (!ctx.spellLookup || known.empty())
    {
        return true; // spell list not available
    }

    const auto dot = action.find('.');

    if (dot == std::string::npos || parsePlayerClass(action.substr(0, dot)) == PlayerClass::Unknown)
    {
        return true; // Player.*, Target.*, ...: not a class spell
    }

    const auto ids = cachedSpellLookup(ctx)(action.substr(dot + 1));

    if (ids.empty())
    {
        return true; // name not in the spell table: cannot tell
    }

    for (const std::uint32_t id : ids)
    {
        if (std::find(known.begin(), known.end(), id) != known.end())
        {
            return true;
        }
    }

    if (reportedUnknown_.insert(action).second)
    {
        ctx.log.debug(logcat::Combat, "skipping '{}': the character has not learned it", action);
    }

    return false;
}

SpellLookup ScriptedCombatClass::cachedSpellLookup(CombatContext& ctx)
{
    if (!ctx.spellLookup)
    {
        return {};
    }

    return [this, lookup = ctx.spellLookup](std::string_view spell)
    {
        auto it = spellIds_.find(spell);

        if (it == spellIds_.end())
        {
            it = spellIds_.emplace(std::string(spell), (*lookup)(spell)).first;
        }

        return it->second;
    };
}

ScriptedCombatClass::WandStop ScriptedCombatClass::stopWand(const std::string& action, CombatContext& ctx)
{
    const TimePoint now = ctx.snapshot.timestamp;

    if (wandStopPressedAt_ && now - *wandStopPressedAt_ < kWandStopSettle)
    {
        return WandStop::Waiting;
    }

    std::string shoot;

    for (const auto& block : program_.blocks)
    {
        for (const auto& rule : block.rules)
        {
            if (shoot.empty() && rule.action.ends_with(".Shoot") && ctx.actions.hasBinding(rule.action))
            {
                shoot = rule.action;
            }
        }
    }

    if (shoot.empty())
    {
        return WandStop::Clear; // nothing to stop it with: cast over it, as before
    }

    // Shoot's cooldown is the gap between shots, not a reason to keep firing.
    ctx.actions.clearCooldown(shoot);

    if (ctx.actions.execute(shoot, "stop the wand for " + action) != ActionResult::Executed)
    {
        return WandStop::Clear;
    }

    // The last shot leaves a global cooldown: the spell pressed straight after
    // got "Spell is not ready yet", which 1.60.1 does not let the bot see, and
    // it went back to the wand thinking the spell cast. Wait it out, and keep
    // the Shoot rule from starting the wand again in the meantime.
    const Duration gcd = ctx.actions.startGlobalCooldown();
    // Held back a while past it too: let go with the global cooldown, the
    // Shoot rule - above the spell in the block - went first again, the
    // wand was stopped for the spell again, and the spell never went off
    // ("Priest.Shoot if not player.isShooting" before Devouring Plague).
    constexpr Duration kForTheSpell{1500};
    ctx.actions.holdBack(shoot, gcd + kForTheSpell);
    wandStopPressedAt_ = now;
    return WandStop::Pressed;
}

bool ScriptedCombatClass::wantsPull(CombatContext& ctx)
{
    return select("pull", ctx) != nullptr;
}

bool ScriptedCombatClass::onPull(CombatContext& ctx)
{
    std::size_t index = 0;
    const script::Rule* rule = select("pull", ctx, &index);

    if (!rule)
    {
        return false;
    }

    if (executeRule(rule, "pull", ctx))
    {
        pullExecutedRules_.insert(index);
        return true;
    }

    return false;
}

std::vector<std::string> ScriptedCombatClass::blockActions(std::string_view blockName) const
{
    std::vector<std::string> out;

    if (const script::Block* block = program_.block(std::string(blockName)))
    {
        for (const auto& rule : block->rules)
        {
            // A step (player.WalkBackward) is no button.
            if (!scriptMoveWay(rule.action) && std::find(out.begin(), out.end(), rule.action) == out.end())
            {
                out.push_back(rule.action);
            }
        }
    }

    return out;
}

bool ScriptedCombatClass::run(std::string_view block, CombatContext& ctx)
{
    const script::Rule* rule = select(block, ctx);

    if (!rule)
    {
        return false;
    }

    return executeRule(rule, block, ctx);
}

bool ScriptedCombatClass::executeRule(const script::Rule* rule, std::string_view block, CombatContext& ctx)
{
    if (!rule)
    {
        return false;
    }

    // A spell pressed while the wand fires clips against its shots and the
    // global cooldown, and often does not go off. Stop the wand first; the
    // block's own Shoot rule starts it again once its condition still holds.
    if (ctx.snapshot.player.isShooting && !isAutoRepeatAction(rule->action) && !isAutoAttackAction(rule->action) && !rule->action.starts_with("Player."))
    {
        const WandStop stop = stopWand(rule->action, ctx);

        if (stop != WandStop::Clear)
        {
            return stop == WandStop::Pressed;
        }
    }

    ctx.log.debug(logcat::Combat, "{}: rule '{}' (line {})", block, rule->action, rule->loc.line);
    const std::string reason = rule->conditionText.empty() ? std::string() : "if " + rule->conditionText;

    // A step: handed to the fight, which walks it.
    if (const auto way = scriptMoveWay(rule->action))
    {
        if (!ctx.moveOrder || !rule->argument)
        {
            return false;
        }

        script::SnapshotEnvironment env(ctx.snapshot, &ctx.actions, &settingValues_, ctx.combatSeconds, cachedSpellLookup(ctx), ctx.instantSpells);
        float yards = 0;

        try
        {
            yards = static_cast<float>(script::toNumber(script::evaluate(*rule->argument, env), rule->loc));
        }
        catch (const script::RuntimeError& e)
        {
            reportRuntimeError(ctx.log, e.loc.line ? e.loc : rule->loc, e.what());
            return false;
        }

        if (!(yards > 0))
        {
            return false;
        }

        *ctx.moveOrder = ScriptMove{*way, std::min(yards, kScriptMoveLongest), std::format("{}({:g}){}", rule->action, yards, reason.empty() ? "" : " " + reason)};
        return true;
    }

    const Duration castTime = ctx.actions.castTime(rule->action);
    if (castTime > Duration(0))
    {
        const bool moving = ctx.snapshot.player.isMoving || ctx.snapshot.player.speed > 0.0f ||
                            ctx.actions.isHeld(actions::MoveForward) || ctx.actions.isHeld(actions::MoveBackward) ||
                            ctx.actions.isHeld(actions::StrafeLeft) || ctx.actions.isHeld(actions::StrafeRight);
        if (moving)
        {
            ctx.actions.release(actions::MoveForward);
            ctx.actions.release(actions::MoveBackward);
            ctx.actions.release(actions::StrafeLeft);
            ctx.actions.release(actions::StrafeRight);
            ctx.log.trace(logcat::Combat, "{}: waiting for 0 movement speed before casting '{}' (speed={:.2f}, moving={})",
                          block, rule->action, ctx.snapshot.player.speed, ctx.snapshot.player.isMoving);
            return false;
        }
    }

    return ctx.actions.execute(rule->action, reason) == ActionResult::Executed;
}

bool ScriptedCombatClass::wantsInstantBuff(CombatContext& ctx)
{
    const std::function<bool(std::string_view)> instantOnly = [&](std::string_view action)
    {
        return action != actions::Eat && action != actions::Drink && ctx.actions.castTime(action) == Duration(0);
    };
    return select("buffs", ctx, nullptr, &instantOnly) != nullptr;
}

bool ScriptedCombatClass::onInstantBuff(CombatContext& ctx)
{
    const std::function<bool(std::string_view)> instantOnly = [&](std::string_view action)
    {
        return action != actions::Eat && action != actions::Drink && ctx.actions.castTime(action) == Duration(0);
    };
    const script::Rule* rule = select("buffs", ctx, nullptr, &instantOnly);
    if (!rule)
    {
        return false;
    }
    return executeRule(rule, "buffs", ctx);
}

bool ScriptedCombatClass::wantsStationaryBuff(CombatContext& ctx)
{
    const std::function<bool(std::string_view)> stationaryOnly = [&](std::string_view action)
    {
        return action == actions::Eat || action == actions::Drink || ctx.actions.castTime(action) > Duration(0);
    };
    return select("buffs", ctx, nullptr, &stationaryOnly) != nullptr;
}

} // namespace og
