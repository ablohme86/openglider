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
 *  File: X11InputBackend.h
 *
 *  Desc:
 *  Declarations and interface for X11InputBackend (low-level OS and
 *  Wine input simulation backends).
 *
 *
 */
#pragma once

#include "og/input/IInputBackend.h"

#include <string>
#include <vector>

namespace og
{

// Sends synthetic X11 key events (XSendEvent) to a specific window, e.g. the
// wine window hosting WoW.exe. This does not require focus from X's point of
// view, but Wine may drop keyboard input for windows it does not consider
// active - prefer WineBridgeInputBackend for reliable background input.
// Experimental.
class X11InputBackend final : public IInputBackend
{
public:
    struct WindowInfo
    {
        unsigned long id = 0;
        std::string title;
        int pid = 0;
    };

    // windowId = 0: search for a window whose title contains `titleContains`.
    explicit X11InputBackend(std::string titleContains = "World of Warcraft", unsigned long windowId = 0);
    ~X11InputBackend() override;

    std::string name() const override
    {
        return "x11";
    }

    bool isConnected() const override;
    unsigned long windowId() const
    {
        return window_;
    }

    static std::vector<WindowInfo> listWindows();

protected:
    void doKeyDown(Key key) override;
    void doKeyUp(Key key) override;
    void doMouseMove(int x, int y) override;
    void doClick(MouseButton button) override;
    void doMouseDown(MouseButton button) override;
    void doMouseUp(MouseButton button) override;
    void doMouseScroll(int delta) override;

private:
    void sendKey(Key key, bool down);
    void sendButton(int button, bool down);

    void* display_ = nullptr;
    unsigned long window_ = 0;
    int mouseX_ = 0;
    int mouseY_ = 0;
};

} // namespace og
