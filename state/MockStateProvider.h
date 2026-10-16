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
 *  File: MockStateProvider.h
 *
 *  Desc:
 *  Declarations and interface for MockStateProvider (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include "og/state/IGameStateProvider.h"

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace og
{

// Hand-fed provider for unit tests. Either set a fixed snapshot, or queue a
// sequence of snapshots that are returned one per capture() (the last one
// repeats once the queue drains).
class MockStateProvider final : public IGameStateProvider
{
public:
    MockStateProvider() = default;
    explicit MockStateProvider(GameSnapshot snapshot) : current_(std::move(snapshot))
    {
    }

    std::string name() const override
    {
        return "mock";
    }

    std::optional<GameSnapshot> capture() override
    {
        if (!queue_.empty())
        {
            current_ = std::move(queue_.front());
            queue_.pop_front();
        }

        return current_;
    }

    void set(GameSnapshot snapshot)
    {
        current_ = std::move(snapshot);
    }

    void clear()
    {
        current_.reset();
        queue_.clear();
    }

    void push(GameSnapshot snapshot)
    {
        queue_.push_back(std::move(snapshot));
    }

    void mutate(const std::function<void(GameSnapshot&)>& fn)
    {
        if (!current_)
        {
            current_ = GameSnapshot{};
        }

        fn(*current_);
    }

    const std::optional<GameSnapshot>& current() const
    {
        return current_;
    }

    // Optional Lua, for behaviors that prefer the client's API to a keypress.
    // Off by default, so tests that do not opt in still see the key bindings.
    void setLuaSupported(bool on)
    {
        lua_ = on;
    }

    bool supportsLua() const override
    {
        return lua_;
    }

    bool executeLua(std::string_view script) override
    {
        scripts.emplace_back(script);
        return lua_;
    }

    std::vector<std::string> scripts;

    std::function<std::optional<ScreenPoint>(const Vec3&)> worldToScreenFunc;
    std::optional<ScreenPoint> worldToScreen(const Vec3& worldPos) override
    {
        if (worldToScreenFunc)
        {
            return worldToScreenFunc(worldPos);
        }

        return std::nullopt;
    }

    // Chat lines for takeChat, as a test says they came.
    std::vector<ChatMessage> chat;
    std::vector<ChatMessage> takeChat() override
    {
        return std::exchange(chat, {});
    }

    // The UI frames the client shows, by name; any other name is unknown.
    std::map<std::string, UiFrame, std::less<>> uiFrames;
    float tabReach = 20.0f;
    float tabTargetReach() const override
    {
        return tabReach;
    }

    bool supportsUi() const override
    {
        return !uiFrames.empty();
    }

    // A read-only 64-bit client: corpses are targeted by name through the chat.
    bool byName = false;
    bool targetsByName() const override
    {
        return byName;
    }

    // What is under the mouse; nullopt: the client does not say.
    std::optional<Guid> mouseover;
    std::optional<Guid> mouseoverGuid() override
    {
        return mouseover;
    }

    std::optional<UiFrame> uiFrame(std::string_view name) override
    {
        const auto it = uiFrames.find(name);

        if (it == uiFrames.end())
        {
            return std::nullopt;
        }

        return it->second;
    }

    // FontString texts by "frame/fontString".
    std::map<std::string, std::string, std::less<>> uiTexts;
    std::optional<std::string> uiText(std::string_view frame, std::string_view fontString) override
    {
        const auto it = uiTexts.find(std::string(frame) + "/" + std::string(fontString));

        if (it == uiTexts.end())
        {
            return std::nullopt;
        }

        return it->second;
    }

    std::function<std::optional<Camera>()> cameraFunc;
    std::optional<Camera> camera() const override
    {
        if (cameraFunc)
        {
            return cameraFunc();
        }

        return std::nullopt;
    }

private:
    std::optional<GameSnapshot> current_;
    std::deque<GameSnapshot> queue_;
    bool lua_ = false;
};

} // namespace og
