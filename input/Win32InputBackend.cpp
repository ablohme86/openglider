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
 *  File: Win32InputBackend.cpp
 *
 *  Desc:
 *  Implementation of Win32InputBackend (low-level OS and Wine input
 *  simulation backends).
 *
 *
 */
#include "og/input/Win32InputBackend.h"

#include "og/input/Win32PostMessage.h"

namespace og
{

Win32InputBackend::Win32InputBackend(std::uint32_t processId) : processId_(processId)
{
    reconnect();
}

bool Win32InputBackend::reconnect()
{
    hwnd_ = win32::findGameWindow(processId_);
    return hwnd_ != nullptr;
}

std::optional<std::pair<int, int>> Win32InputBackend::windowSize()
{
    RECT rc{};

    if (!hwnd_ || !GetClientRect(static_cast<HWND>(hwnd_), &rc) || rc.right <= rc.left || rc.bottom <= rc.top)
    {
        return std::nullopt;
    }

    return std::pair{static_cast<int>(rc.right - rc.left), static_cast<int>(rc.bottom - rc.top)};
}

bool Win32InputBackend::isConnected() const
{
    if (hwnd_ && IsWindow(static_cast<HWND>(hwnd_)))
    {
        return true;
    }

    // Once a second at most: every action asks.
    if (const std::uint64_t now = GetTickCount64(); now - lookedAt_ >= 1000)
    {
        lookedAt_ = now;
        hwnd_ = win32::findGameWindow(processId_);
    }

    return hwnd_ != nullptr;
}

void Win32InputBackend::doKeyDown(Key key)
{
    win32::postKey(static_cast<HWND>(hwnd_), toWindowsVirtualKey(key), isExtendedKey(key), true);
}

void Win32InputBackend::doKeyUp(Key key)
{
    win32::postKey(static_cast<HWND>(hwnd_), toWindowsVirtualKey(key), isExtendedKey(key), false);
}

bool Win32InputBackend::doTypeText(std::string_view text)
{
    for (const char c : text)
    {
        if (!win32::postChar(static_cast<HWND>(hwnd_), static_cast<unsigned char>(c)))
        {
            return false;
        }
    }

    return true;
}

void Win32InputBackend::doMouseMove(int x, int y)
{
    mouseX_ = x;
    mouseY_ = y;
    win32::postMouseMove(static_cast<HWND>(hwnd_), x, y);
}

void Win32InputBackend::doClick(MouseButton button)
{
    const int b = button == MouseButton::Left ? 0 : button == MouseButton::Right ? 1 : 2;
    win32::postMouseButton(static_cast<HWND>(hwnd_), b, true, mouseX_, mouseY_);
    win32::postMouseButton(static_cast<HWND>(hwnd_), b, false, mouseX_, mouseY_);
}

void Win32InputBackend::doMouseDown(MouseButton button)
{
    const int b = button == MouseButton::Left ? 0 : button == MouseButton::Right ? 1 : 2;
    win32::postMouseButton(static_cast<HWND>(hwnd_), b, true, mouseX_, mouseY_);
}

void Win32InputBackend::doMouseUp(MouseButton button)
{
    const int b = button == MouseButton::Left ? 0 : button == MouseButton::Right ? 1 : 2;
    win32::postMouseButton(static_cast<HWND>(hwnd_), b, false, mouseX_, mouseY_);
}

void Win32InputBackend::doMouseScroll(int delta)
{
    win32::postMouseScroll(static_cast<HWND>(hwnd_), delta, mouseX_, mouseY_);
}

} // namespace og
