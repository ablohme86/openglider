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
 *  File: Win32InputBackend.h
 *
 *  Desc:
 *  Declarations and interface for Win32InputBackend (low-level OS and
 *  Wine input simulation backends).
 *
 *
 */
#pragma once

#include "og/input/IInputBackend.h"

#include <cstdint>

namespace og
{

// Native Windows backend: PostMessage straight to the WoW window, so the game
// can stay in the background. Only built on Windows.
class Win32InputBackend final : public IInputBackend
{
public:
    // processId = 0: first window matching the WoW window class / title.
    explicit Win32InputBackend(std::uint32_t processId = 0);
    bool canTypeText() const override
    {
        return true;
    }

    std::optional<std::pair<int, int>> windowSize() override;

    std::string name() const override
    {
        return "win32";
    }

    bool isConnected() const override;

    // Re-resolves the window handle (e.g. after the game restarted).
    bool reconnect();

protected:
    void doKeyDown(Key key) override;
    void doKeyUp(Key key) override;
    void doMouseMove(int x, int y) override;
    void doClick(MouseButton button) override;
    void doMouseDown(MouseButton button) override;
    void doMouseUp(MouseButton button) override;
    void doMouseScroll(int delta) override;
    bool doTypeText(std::string_view text) override;

private:
    std::uint32_t processId_;
    // Looked for again while there is none: the window not up yet when
    // attached, or made anew (the game restarted, the display mode changed).
    mutable void* hwnd_ = nullptr;
    mutable std::uint64_t lookedAt_ = 0;
    int mouseX_ = 0;
    int mouseY_ = 0;
};

} // namespace og
