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
 *  File: Win32PostMessage.h
 *
 *  Desc:
 *  Header-only helpers for delivering input to the WoW window with
 *  PostMessage.
 *
 *
 */
#pragma once

// Header-only helpers for delivering input to the WoW window with PostMessage.
// PostMessage goes straight into the target window's message queue, so the
// game does NOT need to be the foreground window. Shared by
// Win32InputBackend (native Windows) and og-inputbridge.exe (runs inside the
// wine prefix on Linux).
//
// Only include this from Windows translation units.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace og::win32
{

struct FindWindowQuery
{
    DWORD processId = 0; // 0 = any process
    const char* titleContains = "World of Warcraft";
    HWND result = nullptr;
};

inline BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* q = reinterpret_cast<FindWindowQuery*>(lParam);

    if (q->processId != 0)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);

        if (pid != q->processId)
        {
            return TRUE;
        }
    }

    char cls[128] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    // WoW's main window class is "GxWindowClassD3d" (Direct3D) or "GxWindowClass" (OpenGL).
    const bool classMatch = std::string(cls).rfind("GxWindowClass", 0) == 0;
    char title[256] = {};
    GetWindowTextA(hwnd, title, sizeof(title));
    const bool titleMatch = q->titleContains && *q->titleContains && std::string(title).find(q->titleContains) != std::string::npos;

    if (classMatch || titleMatch)
    {
        q->result = hwnd;
        return FALSE;
    }

    return TRUE;
}

inline HWND findGameWindow(DWORD processId = 0, const char* titleContains = "World of Warcraft")
{
    FindWindowQuery q;
    q.processId = processId;
    q.titleContains = titleContains;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&q));
    return q.result;
}

// `repeat`: a key-down sent again while the key is held (autorepeat), which
// says the key was down already.
inline LPARAM keyLParam(std::uint8_t vk, bool extended, bool down, bool repeat = false)
{
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);

    // On international keyboard layouts running under Wine/Windows, MapVirtualKeyA
    // for VK_OEM_MINUS (0xBD) and VK_OEM_PLUS (0xBB) maps to non-number-row scancodes.
    // In WoW, action bar slots 11 and 12 are bound by default to the two keys
    // immediately following '0' on the number row:
    // Scancode 0x0C = US '-' / Action bar slot 11
    // Scancode 0x0D = US '=' / Action bar slot 12
    if (vk == 0xBD || vk == 0xBF)
    {
        scan = 0x0C;
    }
    else if (vk == 0xBB || vk == 0xDC)
    {
        scan = 0x0D;
    }

    LPARAM lp = 1; // repeat count
    lp |= static_cast<LPARAM>(scan & 0xFF) << 16;

    if (extended)
    {
        lp |= static_cast<LPARAM>(1) << 24;
    }

    if (!down || repeat)
    {
        lp |= static_cast<LPARAM>(1) << 30; // previous key state
    }

    if (!down)
    {
        lp |= static_cast<LPARAM>(1) << 31; // transition state
    }

    return lp;
}

inline bool postKey(HWND hwnd, std::uint8_t vk, bool extended, bool down, bool repeat = false)
{
    if (!hwnd || vk == 0)
    {
        return false;
    }

    // On international keyboard layouts running under Wine/Proton, VK_OEM_MINUS (0xBD)
    // and VK_OEM_PLUS (0xBB) do not trigger action bar slots 11 and 12.
    // The physical keys immediately following '0' on the number row map to
    // 0xBF (VK_OEM_2 / Slot 11) and 0xDC (VK_OEM_5 / Slot 12).
    if (vk == 0xBD)
    {
        vk = 0xBF; // Slot 11 (Key -)
    }
    else if (vk == 0xBB)
    {
        vk = 0xDC; // Slot 12 (Key =)
    }

    // Alt combinations are system keys in Win32.
    const bool sys = vk == VK_MENU;
    const UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    return PostMessageA(hwnd, msg, vk, keyLParam(vk, extended, down, down && repeat)) != 0;
}

// One character into the window's focused edit box (the chat), apart from
// the keyboard layout and modifier state.
inline bool postChar(HWND hwnd, unsigned int codePoint)
{
    return hwnd && PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(codePoint), 1) != 0;
}

inline bool postMouseMove(HWND hwnd, int x, int y)
{
    return PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y)) != 0;
}

// button: 0 = left, 1 = right, 2 = middle
inline bool postMouseButton(HWND hwnd, int button, bool down, int x, int y)
{
    UINT msg = 0;
    WPARAM wp = 0;

    switch (button)
    {
    case 0:
        msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        wp = down ? MK_LBUTTON : 0;
        break;
    case 1:
        msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        wp = down ? MK_RBUTTON : 0;
        break;
    default:
        msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        wp = down ? MK_MBUTTON : 0;
        break;
    }

    return PostMessageA(hwnd, msg, wp, MAKELPARAM(x, y)) != 0;
}

inline bool postMouseScroll(HWND hwnd, int delta, int x, int y)
{
    return PostMessageA(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(delta)), MAKELPARAM(x, y)) != 0;
}

// A picture of the window's client area, scaled down to at most `maxWidth`
// pixels wide, as top-down rows of 8-bit BGR.
//
// BitBlt from the window's own DC, not PrintWindow: under Wine with DXVK,
// PrintWindow returns a black picture while BitBlt returns what the game shows,
// even with other windows on top of it. False when the window has no client
// area or the copy fails.
inline bool captureWindowBgr(HWND hwnd, int maxWidth, std::vector<std::uint8_t>& bgr, int& width, int& height)
{
    RECT rc{};

    if (!hwnd || !GetClientRect(hwnd, &rc))
    {
        return false;
    }

    const int sourceWidth = rc.right - rc.left;
    const int sourceHeight = rc.bottom - rc.top;

    if (sourceWidth <= 0 || sourceHeight <= 0)
    {
        return false;
    }

    width = maxWidth > 0 && sourceWidth > maxWidth ? maxWidth : sourceWidth;
    height = static_cast<int>(static_cast<long long>(sourceHeight) * width / sourceWidth);

    if (height <= 0)
    {
        return false;
    }

    HDC windowDc = GetDC(hwnd);

    if (!windowDc)
    {
        return false;
    }

    const auto makeDib = [windowDc](int w, int h, void*& bits)
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        return CreateDIBSection(windowDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    };
    void* fullBits = nullptr;
    void* scaledBits = nullptr;
    HDC fullDc = CreateCompatibleDC(windowDc);
    HDC scaledDc = CreateCompatibleDC(windowDc);
    HBITMAP full = makeDib(sourceWidth, sourceHeight, fullBits);
    HBITMAP scaled = makeDib(width, height, scaledBits);
    bool ok = fullDc && scaledDc && full && scaled;

    if (ok)
    {
        HGDIOBJ oldFull = SelectObject(fullDc, full);
        HGDIOBJ oldScaled = SelectObject(scaledDc, scaled);
        ok = BitBlt(fullDc, 0, 0, sourceWidth, sourceHeight, windowDc, 0, 0, SRCCOPY) != 0;

        if (ok)
        {
            SetStretchBltMode(scaledDc, HALFTONE);
            SetBrushOrgEx(scaledDc, 0, 0, nullptr);
            ok = StretchBlt(scaledDc, 0, 0, width, height, fullDc, 0, 0, sourceWidth, sourceHeight, SRCCOPY) != 0;
        }

        if (ok)
        {
            const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            const auto* source = static_cast<const std::uint8_t*>(scaledBits);
            bgr.resize(pixels * 3);

            for (std::size_t i = 0; i < pixels; ++i)
            {
                bgr[i * 3] = source[i * 4];
                bgr[i * 3 + 1] = source[i * 4 + 1];
                bgr[i * 3 + 2] = source[i * 4 + 2];
            }
        }

        SelectObject(fullDc, oldFull);
        SelectObject(scaledDc, oldScaled);
    }

    if (full)
    {
        DeleteObject(full);
    }

    if (scaled)
    {
        DeleteObject(scaled);
    }

    if (fullDc)
    {
        DeleteDC(fullDc);
    }

    if (scaledDc)
    {
        DeleteDC(scaledDc);
    }

    ReleaseDC(hwnd, windowDc);
    return ok;
}

} // namespace og::win32
