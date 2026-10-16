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
 *  File: ActionService.cpp
 *
 *  Desc:
 *  Implementation of ActionService (semantic game actions, action
 *  configuration, and execution).
 *
 *
 */
#include "og/actions/ActionService.h"

#include <format>

namespace og
{

std::string_view toString(ActionResult r)
{
    switch (r)
    {
    case ActionResult::Executed:
        return "Executed";
    case ActionResult::NotBound:
        return "NotBound";
    case ActionResult::OnCooldown:
        return "OnCooldown";
    case ActionResult::InputUnavailable:
        return "InputUnavailable";
    }

    return "?";
}

ActionService::ActionService(ActionMap map, IInputBackend& input, const IClock& clock, Logger& log, EventBus* events) : map_(std::move(map)), input_(input), clock_(clock), log_(log), events_(events)
{
}

void ActionService::warnUnbound(std::string_view action)
{
    if (warnedUnbound_.insert(std::string(action)).second)
    {
        log_.warn(logcat::Action, "'{}' is not on an action bar", action);
    }
}

bool ActionService::typeLine(std::string_view line)
{
    if (!input_.canTypeText() || !input_.isConnected())
    {
        return false;
    }

    // A key held now would keep the character moving with the chat open, and
    // its release would be typed into the line; one that repeated just before
    // it was let go of would still reach the client after the Enter, into the
    // chat. Let everything go, and wait for the keyboard to be quiet.
    releaseAll();
    input_.settleBeforeTyping();
    input_.press(Key::Enter);
    const bool typed = input_.typeText(line);

    if (typed)
    {
        input_.press(Key::Enter);
    }
    else
    {
        input_.press(Key::Escape); // do not leave a half line open for the next key to land in
    }

    inputLockedUntil_ = clock_.now() + kChatSettle;

    if (typed && line.starts_with("/script "))
    {
        if (clock_.now() - recentScriptAt_ > Duration(10000))
        {
            recentScripts_.clear();
        }

        recentScripts_.emplace_back(line);
        recentScriptAt_ = clock_.now();
    }

    return typed;
}

std::vector<std::string> ActionService::recentScripts(Duration within) const
{
    if (clock_.now() - recentScriptAt_ > within)
    {
        return {};
    }

    return recentScripts_;
}

bool ActionService::chat(std::string_view line)
{
    const bool typed = typeLine(line);

    if (typed)
    {
        log_.info(logcat::Action, "chat: {}", line);
    }

    return typed;
}

void ActionService::setPreparation(std::string action, std::string chatLine, Duration again)
{
    preparations_[std::move(action)] = Preparation{std::move(chatLine), again, std::nullopt};
}

void ActionService::forgetPreparations()
{
    for (auto& [action, preparation] : preparations_)
    {
        preparation.typedAt.reset();
    }
}

bool ActionService::prepare(std::string_view action)
{
    const auto it = preparations_.find(action);

    if (it == preparations_.end())
    {
        return false;
    }

    Preparation& p = it->second;
    const TimePoint now = clock_.now();

    if ((p.typedAt && now - *p.typedAt < p.again) || inputLocked() || !typeLine(p.line))
    {
        return false;
    }

    p.typedAt = now;
    log_.info(logcat::Action, "chat: {} (for {})", p.line, action);
    return true;
}

std::vector<std::string> ActionService::chatScriptLines(std::string_view lua)
{
    const std::string whole = "/script " + std::string(lua);

    if (whole.size() <= kChatLineLimit)
    {
        return {whole};
    }

    // Long-bracket pieces cannot hold their own closing bracket; the scripts the
    // bot builds never contain one.
    constexpr std::string_view head = "/script _cs=(_cs or '')..[==[";
    constexpr std::string_view tail = "]==]";
    const std::size_t room = kChatLineLimit - head.size() - tail.size();
    std::vector<std::string> lines{"/script _cs=''"};

    for (std::size_t at = 0; at < lua.size(); at += room)
    {
        lines.push_back(std::string(head) + std::string(lua.substr(at, room)) + std::string(tail));
    }

    lines.emplace_back("/script local f=loadstring(_cs) _cs=nil if f then f() end");
    return lines;
}

bool ActionService::chatScript(std::string_view lua)
{
    if (lua.find("]==]") != std::string_view::npos || lua.find('|') != std::string_view::npos)
    {
        // "|" starts an escape sequence in the chat box.
        log_.warn(logcat::Action, "script not typed: it holds characters the chat would change");
        return false;
    }

    for (const auto& line : chatScriptLines(lua))
    {
        if (!typeLine(line))
        {
            return false;
        }
    }

    log_.debug(logcat::Action, "script typed into the chat: {}", lua);
    return true;
}

namespace
{
bool movesTheCharacter(std::string_view action)
{
    return action == actions::MoveForward || action == actions::MoveBackward || action == actions::StrafeLeft || action == actions::StrafeRight || action == actions::Jump;
}

} // namespace

ActionResult ActionService::execute(std::string_view action, std::string_view reason)
{
    const ActionBinding* b = map_.find(action);
    std::string chatLine;

    // No key and no action bar slot, but a slash command: typed (/petattack).
    if (b && !b->chord.valid() && b->actionSlot <= 0 && !b->chatCommand.empty() && input_.canTypeText())
    {
        chatLine = b->chatCommand;
    }

    const bool viaChat = !chatLine.empty();
    // No key: an action bar slot pressed through Lua, or else clicked.
    const bool viaLua = !viaChat && b && !b->chord.valid();

    if (!b || (viaLua && (b->actionSlot <= 0 || (!lua_ && !clickSlot_))))
    {
        warnUnbound(action);
        return ActionResult::NotBound;
    }

    const TimePoint now = clock_.now();

    if (!cooldowns_.isReady(action, now, !b->onGcd) || coolingInClient(*b))
    {
        return ActionResult::OnCooldown;
    }

    if (!viaChat && !viaLua && inputLocked())
    {
        log_.debug(logcat::Action, "{} held back: a chat line was just typed", action);
        return ActionResult::InputUnavailable;
    }

    if (movesTheCharacter(action) && isCasting())
    {
        log_.debug(logcat::Action, "ignoring {} while casting {}", action, *castingAction_);
        return ActionResult::InputUnavailable;
    }

    if (!viaLua && !input_.isConnected())
    {
        log_.warn(logcat::Action, "{} skipped: input backend '{}' not connected", action, input_.name());
        return ActionResult::InputUnavailable;
    }

    // What the key needs in the client is typed first; the press follows once
    // the chat has taken the line.
    if (!viaChat && !viaLua && prepare(action))
    {
        return ActionResult::InputUnavailable;
    }

    if (b->castTime > Duration(0))
    {
        release(actions::MoveForward);
        release(actions::MoveBackward);
        release(actions::StrafeLeft);
        release(actions::StrafeRight);

        // Range/approach code may already have released the keys this tick.
        // Defer without recording a cast or cooldown; the next tick retries.
        if (movementStoppedAt_ && now - *movementStoppedAt_ < kMovementStopSettle)
        {
            return ActionResult::InputUnavailable;
        }
    }

    if (viaChat)
    {
        if (!typeLine(chatLine))
        {
            log_.warn(logcat::Action, "{} skipped: '{}' could not be typed into the chat", action, chatLine);
            return ActionResult::InputUnavailable;
        }

        log_.debug(logcat::Action, "{} typed: {}", action, chatLine);
    }
    else if (viaLua)
    {
        // The clicker declines where the client has Lua, which is then used.
        const bool clicked = clickSlot_ && clickSlot_(b->actionSlot);

        if (!clicked && !(b->actionSlot <= 120 && lua_ && lua_(std::format("UseAction({})", b->actionSlot))))
        {
            log_.debug(logcat::Action, "{} skipped: slot {} could be neither clicked nor pressed through Lua", action, b->actionSlot);
            return ActionResult::InputUnavailable;
        }
    }
    else
    {
        input_.press(b->chord);
    }

    if (b->castTime > Duration(0))
    {
        castingUntil_ = now + b->castTime;
        castingAction_ = std::string(action);
        // An estimate: talents and haste shorten the real cast, and the engine
        // drops this timer as soon as the client reports the cast finished.
        log_.info(logcat::Action, "{} (cast time up to {:.1f}s)", action, toSeconds(b->castTime));
    }

    // Where the client tells the cooldown, the bot's own count only bridges
    // the moment until it shows on the button (the cast, and the way there and
    // back): a press that did not go off no longer keeps the spell back for
    // its whole cooldown.
    constexpr Duration kUntilShown{1500};
    Duration own = b->cooldown;

    if (own > b->castTime + kUntilShown && clientTells(*b))
    {
        own = b->castTime + kUntilShown;
    }

    cooldowns_.record(action, now, own, b->onGcd ? map_.globalCooldown : Duration(0));
    last_ = std::string(action);

    if (b->onGcd || b->cooldown > Duration(0) || b->castTime > Duration(0))
    {
        if (lastSpell_ != action)
        {
            previousSpell_ = lastSpell_;
            previousSpellAt_ = lastSpellAt_;
        }

        lastSpell_ = last_;
        lastSpellAt_ = now;
    }

    if (reason.empty())
    {
        log_.info(logcat::Action, "{}", action);
    }
    else
    {
        log_.info(logcat::Action, "{} ({})", action, reason);
    }

    if (events_)
    {
        events_->publish(ActionExecutedEvent{std::string(action), now});
    }

    return ActionResult::Executed;
}

void ActionService::spellNotReady()
{
    if (!lastSpell_)
    {
        return;
    }

    const std::string action = *lastSpell_;
    const TimePoint now = clock_.now();

    // A refusal long after the previous one is a new episode, not a retry.
    if (!notReady_ || notReady_->action != action || now - notReady_->lastRefused > kNotReadyRetryWindow)
    {
        notReady_ = NotReadyRetry{action, now, now};

        // Usually the client's GCD outlasting the bot's estimate: the key and
        // the server response arrive later than the bot started counting.
        if (previousSpell_)
        {
            log_.info(logcat::Action, "{} not ready yet ({:.2f}s after {}) - pressing it again", action, toSeconds(lastSpellAt_ - previousSpellAt_), *previousSpell_);
        }
        else
        {
            log_.info(logcat::Action, "{} not ready yet - pressing it again", action);
        }
    }

    notReady_->lastRefused = now;

    if (now - notReady_->firstRefused >= kNotReadyRetryWindow)
    {
        log_.info(logcat::Action, "{} still not ready after {:.1f}s - giving up", action, toSeconds(now - notReady_->firstRefused));
        notReady_.reset();
        return;
    }

    // The press never started a cooldown, a GCD or a cast.
    cooldowns_.clear(action);
    cooldowns_.clearGcd();

    if (castingAction_ == action)
    {
        cancelCasting();
    }

    log_.debug(logcat::Action, "{} still not ready - pressing it again", action);
}

void ActionService::spellOutOfReach()
{
    if (!lastSpell_)
    {
        return;
    }

    const std::string action = *lastSpell_;
    cooldowns_.clearGcd();

    if (castingAction_ == action)
    {
        cancelCasting();
    }

    cooldowns_.record(action, clock_.now(), kOutOfReachHold, Duration(0));
    notReady_.reset();
    log_.info(logcat::Action, "{} out of range or line of sight - holding it back for {:.1f}s", action, toSeconds(kOutOfReachHold));
}

void ActionService::hold(std::string_view action)
{
    if (held_.contains(action))
    {
        return;
    }

    if (isCasting() && movesTheCharacter(action))
    {
        log_.debug(logcat::Action, "ignoring {} while casting {}", action, *castingAction_);
        return;
    }

    if (inputLocked())
    {
        log_.debug(logcat::Action, "not holding {}: a chat line was just typed", action);
        return;
    }

    const ActionBinding* b = map_.find(action);

    if (!b || !b->chord.valid())
    {
        warnUnbound(action);
        return;
    }

    input_.keyDown(b->chord.key);
    held_.insert(std::string(action));
    log_.trace(logcat::Action, "hold {}", action);
}

void ActionService::release(std::string_view action)
{
    auto it = held_.find(action);

    if (it == held_.end())
    {
        return;
    }

    if (const ActionBinding* b = map_.find(action); b && b->chord.valid())
    {
        input_.keyUp(b->chord.key);
    }

    held_.erase(it);

    if (movesTheCharacter(action))
    {
        movementStoppedAt_ = clock_.now();
    }

    log_.trace(logcat::Action, "release {}", action);
}

bool ActionService::isHeld(std::string_view action) const
{
    return held_.contains(action);
}

void ActionService::releaseAll()
{
    const auto held = held_;

    for (const auto& a : held)
    {
        release(a);
    }
}

bool ActionService::hasBinding(std::string_view action) const
{
    return map_.contains(action);
}

bool ActionService::clientTells(const ActionBinding& binding) const
{
    return clientCooldowns_ && binding.actionSlot > 0 && clientCooldowns_(binding.actionSlot).has_value();
}

bool ActionService::coolingInClient(const ActionBinding& binding) const
{
    if (!clientCooldowns_ || binding.actionSlot <= 0)
    {
        return false;
    }

    const auto shown = clientCooldowns_(binding.actionSlot);
    return shown && !shown->shared && shown->remaining > Duration(0);
}

bool ActionService::isReady(std::string_view action) const
{
    const ActionBinding* b = map_.find(action);
    return b && cooldowns_.isReady(action, clock_.now(), !b->onGcd) && !coolingInClient(*b);
}

bool ActionService::isReadyIgnoringGlobalCooldown(std::string_view action) const
{
    const ActionBinding* b = map_.find(action);
    return b && hasBinding(action) && cooldowns_.isReady(action, clock_.now(), true) && !coolingInClient(*b);
}

Duration ActionService::castTime(std::string_view action) const
{
    const ActionBinding* b = map_.find(action);
    return b ? b->castTime : Duration(0);
}

std::uint32_t ActionService::itemId(std::string_view action) const
{
    const ActionBinding* b = map_.find(action);
    return b ? b->itemId : 0;
}

int ActionService::chooseItemSlot(std::string_view action, const std::function<int(std::uint32_t)>& count)
{
    ActionBinding* b = map_.find(action);

    if (!b || b->itemId == 0)
    {
        return 0;
    }

    const int current = count(b->itemId);
    auto best = b->itemAlternatives.end();
    int most = current;

    for (auto it = b->itemAlternatives.begin(); it != b->itemAlternatives.end(); ++it)
    {
        if (const int n = count(it->itemId); n > most)
        {
            most = n;
            best = it;
        }
    }

    if (best == b->itemAlternatives.end())
    {
        return current;
    }

    // The slot pressed so far stays among the alternatives.
    const ActionBinding::ItemSlot previous{b->chord, b->actionSlot, b->itemId};
    b->chord = best->chord;
    b->actionSlot = best->actionSlot;
    b->itemId = best->itemId;
    *best = previous;
    log_.info(logcat::Action, "{}: using action bar slot {} ({} left there, {} in the slot before)", action, b->actionSlot, most, current);
    return most;
}

ActionRange ActionService::range(std::string_view action) const
{
    const ActionBinding* b = map_.find(action);
    return b ? b->range : ActionRange{};
}

bool ActionService::isCasting() const
{
    return clock_.now() < castingUntil_;
}

Duration ActionService::castingTimeRemaining() const
{
    const TimePoint now = clock_.now();
    return now < castingUntil_ ? (castingUntil_ - now) : Duration(0);
}

std::optional<std::string> ActionService::castingAction() const
{
    if (isCasting())
    {
        return castingAction_;
    }

    return std::nullopt;
}

void ActionService::cancelCasting()
{
    castingUntil_ = TimePoint{};
    castingAction_.reset();
}

} // namespace og
