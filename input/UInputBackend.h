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
 *  File: UInputBackend.h
 *
 *  Desc:
 *  Declarations and interface for UInputBackend (low-level OS and Wine
 *  input simulation backends).
 *
 *
 */
#pragma once

#include "og/input/IInputBackend.h"

namespace og
{

// Linux hardware-level input device via /dev/uinput.
// Emits real evdev events that the Linux kernel, Wayland compositor, Xwayland,
// and Wine receive as genuine physical keyboard and mouse input.
// Works seamlessly on Wayland and X11 without requiring XSendEvent or window IDs.
class UInputBackend final : public IInputBackend
{
public:
    UInputBackend();
    ~UInputBackend() override;

    std::string name() const override
    {
        return "uinput";
    }

    bool isConnected() const override
    {
        return fd_ >= 0;
    }

protected:
    void doKeyDown(Key key) override;
    void doKeyUp(Key key) override;
    void doMouseMove(int x, int y) override;
    void doClick(MouseButton button) override;

private:
    void emit(int type, int code, int val);
    void syn();

    int fd_ = -1;
};

} // namespace og
