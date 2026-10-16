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
 *  File: UInputBackend.cpp
 *
 *  Desc:
 *  Implementation of UInputBackend (low-level OS and Wine input
 *  simulation backends).
 *
 *
 */
#include "og/input/UInputBackend.h"
#include "og/common/FileUtil.h"

#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace og
{

namespace
{

int toLinuxKey(Key key)
{
    switch (key)
    {
    case Key::A:
        return KEY_A;
    case Key::B:
        return KEY_B;
    case Key::C:
        return KEY_C;
    case Key::D:
        return KEY_D;
    case Key::E:
        return KEY_E;
    case Key::F:
        return KEY_F;
    case Key::G:
        return KEY_G;
    case Key::H:
        return KEY_H;
    case Key::I:
        return KEY_I;
    case Key::J:
        return KEY_J;
    case Key::K:
        return KEY_K;
    case Key::L:
        return KEY_L;
    case Key::M:
        return KEY_M;
    case Key::N:
        return KEY_N;
    case Key::O:
        return KEY_O;
    case Key::P:
        return KEY_P;
    case Key::Q:
        return KEY_Q;
    case Key::R:
        return KEY_R;
    case Key::S:
        return KEY_S;
    case Key::T:
        return KEY_T;
    case Key::U:
        return KEY_U;
    case Key::V:
        return KEY_V;
    case Key::W:
        return KEY_W;
    case Key::X:
        return KEY_X;
    case Key::Y:
        return KEY_Y;
    case Key::Z:
        return KEY_Z;
    case Key::D0:
        return KEY_0;
    case Key::D1:
        return KEY_1;
    case Key::D2:
        return KEY_2;
    case Key::D3:
        return KEY_3;
    case Key::D4:
        return KEY_4;
    case Key::D5:
        return KEY_5;
    case Key::D6:
        return KEY_6;
    case Key::D7:
        return KEY_7;
    case Key::D8:
        return KEY_8;
    case Key::D9:
        return KEY_9;
    case Key::F1:
        return KEY_F1;
    case Key::F2:
        return KEY_F2;
    case Key::F3:
        return KEY_F3;
    case Key::F4:
        return KEY_F4;
    case Key::F5:
        return KEY_F5;
    case Key::F6:
        return KEY_F6;
    case Key::F7:
        return KEY_F7;
    case Key::F8:
        return KEY_F8;
    case Key::F9:
        return KEY_F9;
    case Key::F10:
        return KEY_F10;
    case Key::F11:
        return KEY_F11;
    case Key::F12:
        return KEY_F12;
    case Key::Numpad0:
        return KEY_KP0;
    case Key::Numpad1:
        return KEY_KP1;
    case Key::Numpad2:
        return KEY_KP2;
    case Key::Numpad3:
        return KEY_KP3;
    case Key::Numpad4:
        return KEY_KP4;
    case Key::Numpad5:
        return KEY_KP5;
    case Key::Numpad6:
        return KEY_KP6;
    case Key::Numpad7:
        return KEY_KP7;
    case Key::Numpad8:
        return KEY_KP8;
    case Key::Numpad9:
        return KEY_KP9;
    case Key::NumpadAdd:
        return KEY_KPPLUS;
    case Key::NumpadSubtract:
        return KEY_KPMINUS;
    case Key::NumpadMultiply:
        return KEY_KPASTERISK;
    case Key::NumpadDivide:
        return KEY_KPSLASH;
    case Key::NumpadDecimal:
        return KEY_KPDOT;
    case Key::Space:
        return KEY_SPACE;
    case Key::Enter:
        return KEY_ENTER;
    case Key::Escape:
        return KEY_ESC;
    case Key::Tab:
        return KEY_TAB;
    case Key::Backspace:
        return KEY_BACKSPACE;
    case Key::Up:
        return KEY_UP;
    case Key::Down:
        return KEY_DOWN;
    case Key::Left:
        return KEY_LEFT;
    case Key::Right:
        return KEY_RIGHT;
    case Key::Insert:
        return KEY_INSERT;
    case Key::Delete:
        return KEY_DELETE;
    case Key::Home:
        return KEY_HOME;
    case Key::End:
        return KEY_END;
    case Key::PageUp:
        return KEY_PAGEUP;
    case Key::PageDown:
        return KEY_PAGEDOWN;
    case Key::Minus:
        return KEY_MINUS;
    case Key::Equals:
        return KEY_EQUAL;
    case Key::LeftBracket:
        return KEY_LEFTBRACE;
    case Key::RightBracket:
        return KEY_RIGHTBRACE;
    case Key::Semicolon:
        return KEY_SEMICOLON;
    case Key::Apostrophe:
        return KEY_APOSTROPHE;
    case Key::Comma:
        return KEY_COMMA;
    case Key::Period:
        return KEY_DOT;
    case Key::Slash:
        return KEY_SLASH;
    case Key::Backslash:
        return KEY_BACKSLASH;
    case Key::Grave:
        return KEY_GRAVE;
    case Key::Shift:
        return KEY_LEFTSHIFT;
    case Key::Control:
        return KEY_LEFTCTRL;
    case Key::Alt:
        return KEY_LEFTALT;
    default:
        return 0;
    }
}

} // namespace

UInputBackend::UInputBackend()
{
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (fd_ < 0)
    {
        return;
    }

    ::ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ::ioctl(fd_, UI_SET_EVBIT, EV_SYN);
    ::ioctl(fd_, UI_SET_EVBIT, EV_REL);
    ::ioctl(fd_, UI_SET_RELBIT, REL_X);
    ::ioctl(fd_, UI_SET_RELBIT, REL_Y);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_RIGHT);
    ::ioctl(fd_, UI_SET_KEYBIT, BTN_MIDDLE);

    for (int k = static_cast<int>(Key::None) + 1; k < static_cast<int>(Key::Count_); ++k)
    {
        int code = toLinuxKey(static_cast<Key>(k));

        if (code > 0)
        {
            ::ioctl(fd_, UI_SET_KEYBIT, code);
        }
    }

    struct uinput_setup usetup{};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234;
    usetup.id.product = 0x5678;
    const std::string name = std::string(applicationName()) + " Virtual Device";
    std::strncpy(usetup.name, name.c_str(), UINPUT_MAX_NAME_SIZE - 1);

    if (::ioctl(fd_, UI_DEV_SETUP, &usetup) < 0 || ::ioctl(fd_, UI_DEV_CREATE) < 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

UInputBackend::~UInputBackend()
{
    if (fd_ >= 0)
    {
        ::ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
        fd_ = -1;
    }
}

void UInputBackend::emit(int type, int code, int val)
{
    if (fd_ < 0)
    {
        return;
    }

    struct input_event ie{};
    ie.type = static_cast<__u16>(type);
    ie.code = static_cast<__u16>(code);
    ie.value = val;
    ::write(fd_, &ie, sizeof(ie));
}

void UInputBackend::syn()
{
    emit(EV_SYN, SYN_REPORT, 0);
}

void UInputBackend::doKeyDown(Key key)
{
    int code = toLinuxKey(key);

    if (code > 0)
    {
        emit(EV_KEY, code, 1);
        syn();
    }
}

void UInputBackend::doKeyUp(Key key)
{
    int code = toLinuxKey(key);

    if (code > 0)
    {
        emit(EV_KEY, code, 0);
        syn();
    }
}

void UInputBackend::doMouseMove(int x, int y)
{
    emit(EV_REL, REL_X, x);
    emit(EV_REL, REL_Y, y);
    syn();
}

void UInputBackend::doClick(MouseButton button)
{
    int btn = button == MouseButton::Left ? BTN_LEFT : button == MouseButton::Right ? BTN_RIGHT : BTN_MIDDLE;
    emit(EV_KEY, btn, 1);
    syn();
    emit(EV_KEY, btn, 0);
    syn();
}

} // namespace og
