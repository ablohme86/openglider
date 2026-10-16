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
 *  File: ActionService.h
 *
 *  Desc:
 *  Declarations and interface for ActionService (semantic game actions,
 *  action configuration, and execution).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"
#include "og/actions/ActionCooldownTracker.h"
#include "og/actions/IActionService.h"
#include "og/common/EventBus.h"
#include "og/common/Log.h"
#include "og/input/IInputBackend.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace og
{

struct ActionExecutedEvent
{
    std::string action;
    TimePoint time;
};

class ActionService final : public IActionService
{
public:
    ActionService(ActionMap map, IInputBackend& input, const IClock& clock, Logger& log, EventBus* events = nullptr);

    ActionResult execute(std::string_view action, std::string_view reason = {}) override;
    // A chat line the action's key needs typed first (a binding the client
    // drops on a /reload): typed when the action is first used, not before,
    // and again once `again` has passed since.
    void setPreparation(std::string action, std::string chatLine, Duration again);
    // A loading screen took what was typed: typed again on the next use.
    void forgetPreparations();
    bool prepare(std::string_view action) override;
    void hold(std::string_view action) override;
    void release(std::string_view action) override;
    bool isHeld(std::string_view action) const override;
    void releaseAll() override;
    bool hasBinding(std::string_view action) const override;
    bool chat(std::string_view line) override;
    // The /script lines typed lately (the last within `within` of the one
    // before), in order: the first /script of a session makes the client ask
    // whether custom scripts may run, and the ones typed meanwhile are dropped.
    std::vector<std::string> recentScripts(Duration within) const;
    void clearRecentScripts()
    {
        recentScripts_.clear();
    }

    bool isReady(std::string_view action) const override;
    bool isReadyIgnoringGlobalCooldown(std::string_view action) const override;
    // The cooldown the client shows for an action bar slot, where it can be
    // read: an action's own cooldown is then the client's to tell, not a count
    // from the bot's key presses (which knows nothing of talents, of a spell
    // cast by hand, or of a press that did not go off). `shared`: the global
    // cooldown or the like, which stays the bot's own to count.
    struct ClientCooldown
    {
        Duration remaining{0};
        Duration duration{0};
        bool shared = false;
    };
    using ClientCooldowns = std::function<std::optional<ClientCooldown>(int slot)>;
    void setClientCooldowns(ClientCooldowns read)
    {
        clientCooldowns_ = std::move(read);
    }
    bool onGlobalCooldown() const override
    {
        return cooldowns_.gcdRemaining(clock_.now()) > Duration(0);
    }

    void clearCooldown(std::string_view action) override
    {
        cooldowns_.clear(action);
    }

    void holdBack(std::string_view action, Duration duration) override
    {
        cooldowns_.record(action, clock_.now(), duration, Duration(0));
    }

    Duration startGlobalCooldown() override
    {
        cooldowns_.startGcd(clock_.now(), map_.globalCooldown);
        return map_.globalCooldown;
    }

    void spellNotReady() override;
    void spellOutOfReach() override;
    std::optional<std::string> lastAction() const override
    {
        return last_;
    }

    Duration castTime(std::string_view action) const override;
    ActionRange range(std::string_view action) const override;
    std::uint32_t itemId(std::string_view action) const override;
    int chooseItemSlot(std::string_view action, const std::function<int(std::uint32_t)>& count) override;
    bool isCasting() const override;
    Duration castingTimeRemaining() const override;
    std::optional<std::string> castingAction() const override;
    void cancelCasting() override;

    // The mouse waits out a chat line too; a click would take the chat's focus.
    void mouseMove(int x, int y) override
    {
        if (!inputLocked())
        {
            input_.mouseMove(x, y);
        }
    }

    void click(MouseButton button = MouseButton::Left) override
    {
        if (!inputLocked())
        {
            input_.click(button);
        }
    }

    void mouseDown(MouseButton button = MouseButton::Left) override
    {
        if (!inputLocked())
        {
            input_.mouseDown(button);
        }
    }

    void mouseUp(MouseButton button = MouseButton::Left) override
    {
        input_.mouseUp(button);
    }

    void mouseScroll(int delta) override
    {
        if (!inputLocked())
        {
            input_.mouseScroll(delta);
        }
    }

    std::optional<std::pair<int, int>> windowSize() override
    {
        return input_.windowSize();
    }

    bool mouseFree() const override
    {
        return !inputLocked();
    }

    bool chatScript(std::string_view lua) override;
    // The chat lines chatScript types for `lua`: one /script when it fits
    // kChatLineLimit, else pieces gathered into a global and run from it.
    static std::vector<std::string> chatScriptLines(std::string_view lua);
    static constexpr std::size_t kChatLineLimit = 255;
    // Hold keys back while a chat command settles.
    bool inputLocked() const override
    {
        return clock_.now() < inputLockedUntil_;
    }

    static constexpr Duration kChatSettle{150};

    std::vector<SpellCost> costOf(std::string_view action) const override
    {
        const ActionBinding* binding = map_.find(action);
        return binding && binding->spellId && spellCosts_ ? spellCosts_->of(binding->spellId) : std::vector<SpellCost>{};
    }

    // The client's spell costs, read when the bot attaches (null: none known).
    void setSpellCosts(std::shared_ptr<const SpellCosts> costs)
    {
        spellCosts_ = std::move(costs);
    }

    bool isFriendly(std::string_view action) const override
    {
        return map_.isFriendly(action);
    }

    const ActionMap& map() const
    {
        return map_;
    }

    void setMap(ActionMap map)
    {
        map_ = std::move(map);
    }

    ActionCooldownTracker& cooldowns()
    {
        return cooldowns_;
    }

    // Runs a Lua script in the client; false if the client could not take it.
    // Bindings without a key (action bar slots the game has no key bound to)
    // are pressed through it.
    using LuaRunner = std::function<bool(std::string_view)>;
    void setLuaRunner(LuaRunner run)
    {
        lua_ = std::move(run);
    }

    // Clicks an action bar slot's button in the client's UI; false where it
    // cannot (no UI to read, the bar not shown). Where there is no Lua, the
    // slots without a key are pressed through it.
    using SlotClicker = std::function<bool(int slot)>;
    void setSlotClicker(SlotClicker click)
    {
        clickSlot_ = std::move(click);
    }

    // Let the client process movement key-up before starting a cast.
    static constexpr Duration kMovementStopSettle{50};
    // How long a refused spell keeps being retried before the bot gives up.
    static constexpr Duration kNotReadyRetryWindow{1000};
    // How long a spell that was out of range or sight is held back.
    static constexpr Duration kOutOfReachHold{1500};
    IInputBackend& input()
    {
        return input_;
    }

private:
    void warnUnbound(std::string_view action);
    // Enter, the line, Enter, with every held key let go first; Escape closes
    // the chat again if the line did not go out. Locks the keys for kChatSettle.
    bool typeLine(std::string_view line);
    TimePoint inputLockedUntil_{};
    std::optional<TimePoint> movementStoppedAt_;
    std::vector<std::string> recentScripts_;
    TimePoint recentScriptAt_{};
    struct Preparation
    {
        std::string line;
        Duration again{};
        std::optional<TimePoint> typedAt;
    };
    std::map<std::string, Preparation, std::less<>> preparations_;

    ActionMap map_;
    std::shared_ptr<const SpellCosts> spellCosts_;
    IInputBackend& input_;
    const IClock& clock_;
    Logger& log_;
    EventBus* events_;
    LuaRunner lua_;
    SlotClicker clickSlot_;
    ClientCooldowns clientCooldowns_;
    // Still cooling down by what the client shows on the action's button.
    bool coolingInClient(const ActionBinding& binding) const;
    // The client tells this action's cooldown (its button is shown).
    bool clientTells(const ActionBinding& binding) const;
    ActionCooldownTracker cooldowns_;
    std::set<std::string, std::less<>> held_;
    std::set<std::string, std::less<>> warnedUnbound_;
    std::optional<std::string> last_;
    std::optional<std::string> lastSpell_; // last press that can be refused as not ready
    TimePoint lastSpellAt_{};
    // The spell pressed before lastSpell_, for telling a GCD refusal apart in the log.
    std::optional<std::string> previousSpell_;
    TimePoint previousSpellAt_{};
    struct NotReadyRetry
    {
        std::string action;
        TimePoint firstRefused{};
        TimePoint lastRefused{};
    };
    std::optional<NotReadyRetry> notReady_;
    TimePoint castingUntil_{};
    std::optional<std::string> castingAction_;
};

} // namespace og
