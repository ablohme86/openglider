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
 *  File: VirtualInputBackend.h
 *
 *  Desc:
 *  Declarations and interface for VirtualInputBackend (low-level OS and
 *  Wine input simulation backends).
 *
 *
 */
#pragma once

#include "og/input/IInputBackend.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace og
{

struct InputEvent
{
    enum class Type : std::uint8_t
    {
        KeyDown,
        KeyUp,
        MouseMove,
        Click,
        MouseDown,
        MouseUp,
        MouseScroll,
        Text
    };
    Type type = Type::KeyDown;
    Key key = Key::None;
    int x = 0;
    int y = 0;
    MouseButton button = MouseButton::Left;
    int scrollDelta = 0;
    std::string text; // Type::Text

    std::string toString() const;
};

// Records all input instead of sending it anywhere. Used by unit tests and by
// the simulator (which listens to the events to move the simulated player).
class VirtualInputBackend final : public IInputBackend
{
public:
    using Listener = std::function<void(const InputEvent&)>;

    std::string name() const override
    {
        return "virtual";
    }

    // Nothing repeats here: no need to wait before typing.
    std::chrono::milliseconds typingSettle() const override
    {
        return settle_;
    }

    void setTypingSettle(std::chrono::milliseconds settle)
    {
        settle_ = settle;
    }

    bool canTypeText() const override
    {
        return true;
    }

    // No window unless a test gives it one, for clicks on UI frames.
    void setWindowSize(int width, int height)
    {
        windowSize_ = std::pair{width, height};
    }

    std::optional<std::pair<int, int>> windowSize() override
    {
        return windowSize_;
    }

    void setListener(Listener listener)
    {
        listener_ = std::move(listener);
    }

    const std::vector<InputEvent>& events() const
    {
        return events_;
    }

    void clearEvents()
    {
        events_.clear();
    }

    void setRecording(bool enabled)
    {
        recording_ = enabled;
    }

    // Keys that received a KeyDown since the last clearEvents().
    std::vector<Key> pressedKeys() const;

protected:
    void doKeyDown(Key key) override
    {
        dispatchInput({InputEvent::Type::KeyDown, key});
    }

    void doKeyUp(Key key) override
    {
        dispatchInput({InputEvent::Type::KeyUp, key});
    }

    void doMouseMove(int x, int y) override
    {
        dispatchInput({InputEvent::Type::MouseMove, Key::None, x, y});
    }

    void doClick(MouseButton b) override
    {
        dispatchInput({InputEvent::Type::Click, Key::None, 0, 0, b});
    }

    void doMouseDown(MouseButton b) override
    {
        dispatchInput({InputEvent::Type::MouseDown, Key::None, 0, 0, b});
    }

    void doMouseUp(MouseButton b) override
    {
        dispatchInput({InputEvent::Type::MouseUp, Key::None, 0, 0, b});
    }

    bool doTypeText(std::string_view text) override
    {
        InputEvent e{InputEvent::Type::Text};
        e.text = std::string(text);
        dispatchInput(e);
        return true;
    }

    void doMouseScroll(int delta) override
    {
        dispatchInput({InputEvent::Type::MouseScroll, Key::None, 0, 0, MouseButton::Left, delta});
    }

private:
    void dispatchInput(const InputEvent& e)
    {
        if (recording_)
        {
            events_.push_back(e);
        }

        if (listener_)
        {
            listener_(e);
        }
    }

    Listener listener_;
    std::vector<InputEvent> events_;
    bool recording_ = true;
    std::optional<std::pair<int, int>> windowSize_;
    std::chrono::milliseconds settle_{0};
};

} // namespace og
