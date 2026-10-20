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
 *  File: ScriptedCombatClass.h
 *
 *  Desc:
 *  Declarations and interface for ScriptedCombatClass (combat
 *  execution, rotation scripting, and class behaviors).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"
#include "og/combat/ICombatClass.h"
#include "og/combat/script/ScriptAst.h"
#include "og/combat/script/SnapshotEnvironment.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace og
{

// Combat class implemented in OGScript (.ogc file). See ScriptParser.h for the
// language and data/classes/*.ogc for examples.
class ScriptedCombatClass final : public ICombatClass
{
public:
    struct LoadResult
    {
        std::unique_ptr<ScriptedCombatClass> combatClass;
        std::vector<script::Diagnostic> diagnostics; // errors and warnings
        bool ok() const
        {
            return combatClass != nullptr;
        }

        bool hasErrors() const;
        std::string report(const std::string& file = {}) const;
    };

    // `actions` (optional) is used to warn about unbound actions.
    static LoadResult fromSource(std::string_view source, const ActionMap* actions = nullptr);
    static LoadResult fromFile(const std::filesystem::path& path, const ActionMap* actions = nullptr);

    std::string name() const override
    {
        return program_.className.empty() ? "Script" : program_.className;
    }

    const CombatClassSettings& settings() const override
    {
        return settings_;
    }

    float combatRange(CombatContext& ctx) override;
    float pullReach(CombatContext& ctx) override;
    bool isRanged() const override;
    bool wantsMeleeSwing(CombatContext& ctx) override;

    std::vector<std::string> blockActions(std::string_view block) const override;

    bool wantsEmergency(CombatContext& ctx) override
    {
        return select("emergency", ctx) != nullptr;
    }

    bool onEmergency(CombatContext& ctx) override
    {
        return run("emergency", ctx);
    }

    bool wantsPartyHeal(CombatContext& ctx, const std::function<bool(std::string_view)>& allowed) override
    {
        return select("emergency", ctx, nullptr, &allowed) != nullptr;
    }

    bool onPartyHeal(CombatContext& ctx, const std::function<bool(std::string_view)>& allowed) override
    {
        return executeRule(select("emergency", ctx, nullptr, &allowed), "emergency", ctx);
    }

    bool wantsPull(CombatContext& ctx) override;
    bool onPull(CombatContext& ctx) override;
    void resetPull() override
    {
        pullExecutedRules_.clear();
    }

    bool wantsCombat(CombatContext& ctx) override
    {
        return select("combat", ctx, nullptr, nullptr, true) != nullptr;
    }

    bool onCombat(CombatContext& ctx) override
    {
        return run("combat", ctx);
    }

    bool onRest(CombatContext& ctx) override
    {
        return run("rest", ctx);
    }

    bool wantsBuff(CombatContext& ctx) override
    {
        return select("buffs", ctx) != nullptr;
    }

    bool onBuff(CombatContext& ctx) override
    {
        return run("buffs", ctx);
    }

    bool wantsInstantBuff(CombatContext& ctx) override;
    bool onInstantBuff(CombatContext& ctx) override;
    bool wantsStationaryBuff(CombatContext& ctx) override;

    // Dry run: the rule that would fire right now, or nullptr; only rules
    // whose action `allowed` lets through, where it is given.
    const script::Rule* select(std::string_view block, CombatContext& ctx, std::size_t* ruleIndex = nullptr, const std::function<bool(std::string_view)>* allowed = nullptr, bool ignoreToggleCooldown = false);

    const script::Program& program() const
    {
        return program_;
    }

    const script::SettingsMap& settingValues() const
    {
        return settingValues_;
    }

private:
    explicit ScriptedCombatClass(script::Program program) : program_(std::move(program))
    {
    }

    bool run(std::string_view block, CombatContext& ctx);
    bool executeRule(const script::Rule* rule, std::string_view block, CombatContext& ctx);
    // False only when the character's spell list is known and does not contain
    // the rule's class spell (e.g. "Mage.Blizzard" before learning Blizzard).
    // True when the action has no configured range, or the current target is
    // inside it. Keeps the distance out of the script: a rule says what to cast
    // and why, actions.yaml says how far the spell reaches.
    bool inRange(const std::string& action, CombatContext& ctx) const;
    // Enough rage, energy or mana for what the spell costs (the client's
    // SpellPower table): a rule firing short of it pressed the spell again and
    // again into "Not enough rage". Said once, while it waits.
    bool affordable(const std::string& action, CombatContext& ctx);
    // Enough for it, and how much is wanted and of what, where it is short.
    struct Shortfall
    {
        int amount = 0;
        int have = 0;
        std::uint8_t power = 0;
    };
    std::optional<Shortfall> shortOf(const std::string& action, const CombatContext& ctx) const;
    std::set<std::string> waitingForPower_;
    bool characterKnows(const std::string& action, CombatContext& ctx);
    // The live game's spell lookup with every name looked up once; empty
    // without one.
    SpellLookup cachedSpellLookup(CombatContext& ctx);
    // Whether a melee swing is all the block has left to make right now: an
    // auto attack rule would fire, and no rule that reaches farther would (out
    // of mana with no wand). Rules are judged by their conditions, not their range.
    bool onlyMeleeLeft(const script::Block& block, CombatContext& ctx);
    void reportRuntimeError(Logger& log, const script::SourceLoc& loc, const std::string& message);
    // Turns the wand off so `action` can be cast.
    enum class WandStop
    {
        Clear,
        Pressed,
        Waiting
    };
    WandStop stopWand(const std::string& action, CombatContext& ctx);

    script::Program program_;
    script::SettingsMap settingValues_;
    CombatClassSettings settings_;
    std::set<std::pair<std::size_t, std::size_t>> reportedErrors_;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> spellIds_; // spell name -> spell ids (cache)
    std::set<std::string> reportedUnknown_;
    std::set<std::string> reportedUnbound_;
    std::optional<TimePoint> wandStopPressedAt_;
    std::set<std::size_t> pullExecutedRules_;
    std::map<std::string, float, std::less<>> actionMaxRanges_;
};

} // namespace og
