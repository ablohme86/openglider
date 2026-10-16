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
 *  File: X11InputBackend.cpp
 *
 *  Desc:
 *  Implementation of X11InputBackend (low-level OS and Wine input
 *  simulation backends).
 *
 *
 */
#include "og/input/X11InputBackend.h"

// X11 headers define macros such as None/Bool/Status; include them last and do
// not reference og::Key::None below this point.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>

namespace og
{

namespace
{

Display* dpy(void* d)
{
    return static_cast<Display*>(d);
}

KeySym toKeySym(Key key)
{
    const auto k = static_cast<int>(key);

    if (k >= static_cast<int>(Key::A) && k <= static_cast<int>(Key::Z))
    {
        return XK_a + (k - static_cast<int>(Key::A));
    }

    if (k >= static_cast<int>(Key::D0) && k <= static_cast<int>(Key::D9))
    {
        return XK_0 + (k - static_cast<int>(Key::D0));
    }

    if (k >= static_cast<int>(Key::F1) && k <= static_cast<int>(Key::F12))
    {
        return XK_F1 + (k - static_cast<int>(Key::F1));
    }

    if (k >= static_cast<int>(Key::Numpad0) && k <= static_cast<int>(Key::Numpad9))
    {
        return XK_KP_0 + (k - static_cast<int>(Key::Numpad0));
    }

    switch (key)
    {
    case Key::NumpadAdd:
        return XK_KP_Add;
    case Key::NumpadSubtract:
        return XK_KP_Subtract;
    case Key::NumpadMultiply:
        return XK_KP_Multiply;
    case Key::NumpadDivide:
        return XK_KP_Divide;
    case Key::NumpadDecimal:
        return XK_KP_Decimal;
    case Key::Space:
        return XK_space;
    case Key::Enter:
        return XK_Return;
    case Key::Escape:
        return XK_Escape;
    case Key::Tab:
        return XK_Tab;
    case Key::Backspace:
        return XK_BackSpace;
    case Key::Up:
        return XK_Up;
    case Key::Down:
        return XK_Down;
    case Key::Left:
        return XK_Left;
    case Key::Right:
        return XK_Right;
    case Key::Insert:
        return XK_Insert;
    case Key::Delete:
        return XK_Delete;
    case Key::Home:
        return XK_Home;
    case Key::End:
        return XK_End;
    case Key::PageUp:
        return XK_Page_Up;
    case Key::PageDown:
        return XK_Page_Down;
    case Key::Minus:
        return XK_minus;
    case Key::Equals:
        return XK_equal;
    case Key::LeftBracket:
        return XK_bracketleft;
    case Key::RightBracket:
        return XK_bracketright;
    case Key::Semicolon:
        return XK_semicolon;
    case Key::Apostrophe:
        return XK_apostrophe;
    case Key::Comma:
        return XK_comma;
    case Key::Period:
        return XK_period;
    case Key::Slash:
        return XK_slash;
    case Key::Backslash:
        return XK_backslash;
    case Key::Grave:
        return XK_grave;
    case Key::Shift:
        return XK_Shift_L;
    case Key::Control:
        return XK_Control_L;
    case Key::Alt:
        return XK_Alt_L;
    default:
        return NoSymbol;
    }
}

std::string windowTitle(Display* d, Window w)
{
    Atom netWmName = XInternAtom(d, "_NET_WM_NAME", True);
    Atom utf8 = XInternAtom(d, "UTF8_STRING", True);

    if (netWmName != 0 && utf8 != 0)
    {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(d, w, netWmName, 0, 1024, False, utf8, &type, &format, &count, &after, &data) == Success && data)
        {
            std::string title(reinterpret_cast<char*>(data), count);
            XFree(data);

            if (!title.empty())
            {
                return title;
            }
        }
    }

    char* name = nullptr;

    if (XFetchName(d, w, &name) && name)
    {
        std::string title(name);
        XFree(name);
        return title;
    }

    return {};
}

int windowPid(Display* d, Window w)
{
    Atom pidAtom = XInternAtom(d, "_NET_WM_PID", True);

    if (pidAtom == 0)
    {
        return 0;
    }

    Atom type = 0;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    int pid = 0;

    if (XGetWindowProperty(d, w, pidAtom, 0, 1, False, XA_CARDINAL, &type, &format, &count, &after, &data) == Success && data)
    {
        if (count > 0)
        {
            pid = static_cast<int>(*reinterpret_cast<unsigned long*>(data));
        }

        XFree(data);
    }

    return pid;
}

void walkWindows(Display* d, Window w, const std::function<void(Window)>& fn, int depth = 0)
{
    fn(w);

    if (depth > 8)
    {
        return;
    }

    Window root = 0, parent = 0;
    Window* children = nullptr;
    unsigned int n = 0;

    if (!XQueryTree(d, w, &root, &parent, &children, &n))
    {
        return;
    }

    for (unsigned int i = 0; i < n; ++i)
    {
        walkWindows(d, children[i], fn, depth + 1);
    }

    if (children)
    {
        XFree(children);
    }
}

std::string lowerStr(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

X11InputBackend::X11InputBackend(std::string titleContains, unsigned long windowId)
{
    if (titleContains.empty())
    {
        titleContains = "World of Warcraft";
    }

    Display* d = XOpenDisplay(nullptr);
    display_ = d;

    if (!d)
    {
        return;
    }

    if (windowId != 0)
    {
        window_ = windowId;
        return;
    }

    const std::string needle = lowerStr(titleContains);
    walkWindows(d, DefaultRootWindow(d),
                [&](Window w)
                {
                    if (window_ != 0)
                    {
                        return;
                    }

                    const std::string title = windowTitle(d, w);

                    if (!title.empty() && lowerStr(title).find(needle) != std::string::npos)
                    {
                        window_ = w;
                    }
                });
}

X11InputBackend::~X11InputBackend()
{
    releaseAll();

    if (display_)
    {
        XCloseDisplay(dpy(display_));
    }
}

bool X11InputBackend::isConnected() const
{
    return display_ != nullptr && window_ != 0;
}

std::vector<X11InputBackend::WindowInfo> X11InputBackend::listWindows()
{
    std::vector<WindowInfo> out;
    Display* d = XOpenDisplay(nullptr);

    if (!d)
    {
        return out;
    }

    walkWindows(d, DefaultRootWindow(d),
                [&](Window w)
                {
                    std::string title = windowTitle(d, w);

                    if (!title.empty())
                    {
                        out.push_back({w, std::move(title), windowPid(d, w)});
                    }
                });
    XCloseDisplay(d);
    return out;
}

void X11InputBackend::sendKey(Key key, bool down)
{
    if (!isConnected())
    {
        return;
    }

    Display* d = dpy(display_);
    const KeySym sym = toKeySym(key);

    if (sym == NoSymbol)
    {
        return;
    }

    unsigned int state = 0;

    if (isHeld(Key::Shift))
    {
        state |= ShiftMask;
    }

    if (isHeld(Key::Control))
    {
        state |= ControlMask;
    }

    if (isHeld(Key::Alt))
    {
        state |= Mod1Mask;
    }

    XKeyEvent ev{};
    ev.type = down ? KeyPress : KeyRelease;
    ev.display = d;
    ev.window = window_;
    ev.root = DefaultRootWindow(d);
    ev.subwindow = 0;
    ev.time = CurrentTime;
    ev.x = ev.y = ev.x_root = ev.y_root = 1;
    KeyCode kc = XKeysymToKeycode(d, sym);

    // On international keyboard layouts (e.g. Norwegian, German, French),
    // XK_minus and XK_equal map to non-number-row keycodes or require Shift (e.g. keycodes 61, 19).
    // In WoW running under Wine, action bar slots 11 and 12 are physically bound to the two keys
    // immediately following '0' on the number row (Linux kernel evdev 12 and 13 -> X11 keycodes 20 and 21).
    if (key == Key::Minus)
    {
        const KeyCode kc0 = XKeysymToKeycode(d, XK_0);

        if (kc0 > 0)
        {
            kc = static_cast<KeyCode>(kc0 + 1);
        }
    }
    else if (key == Key::Equals)
    {
        const KeyCode kc0 = XKeysymToKeycode(d, XK_0);

        if (kc0 > 0)
        {
            kc = static_cast<KeyCode>(kc0 + 2);
        }
    }

    if (kc == 0)
    {
        return;
    }

    ev.keycode = kc;
    ev.state = state;
    XSendEvent(d, window_, False, down ? KeyPressMask : KeyReleaseMask, reinterpret_cast<XEvent*>(&ev));
    XFlush(d);
}

void X11InputBackend::sendButton(int button, bool down)
{
    if (!isConnected())
    {
        return;
    }

    Display* d = dpy(display_);
    XButtonEvent ev{};
    ev.type = down ? ButtonPress : ButtonRelease;
    ev.display = d;
    ev.window = window_;
    ev.root = DefaultRootWindow(d);
    ev.time = CurrentTime;
    ev.x = mouseX_;
    ev.y = mouseY_;
    ev.same_screen = True;
    ev.button = static_cast<unsigned int>(button);
    XSendEvent(d, window_, True, down ? ButtonPressMask : ButtonReleaseMask, reinterpret_cast<XEvent*>(&ev));
    XFlush(d);
}

void X11InputBackend::doKeyDown(Key key)
{
    sendKey(key, true);
}

void X11InputBackend::doKeyUp(Key key)
{
    sendKey(key, false);
}

void X11InputBackend::doMouseMove(int x, int y)
{
    mouseX_ = x;
    mouseY_ = y;

    if (!isConnected())
    {
        return;
    }

    Display* d = dpy(display_);
    XMotionEvent ev{};
    ev.type = MotionNotify;
    ev.display = d;
    ev.window = window_;
    ev.root = DefaultRootWindow(d);
    ev.time = CurrentTime;
    ev.x = x;
    ev.y = y;
    ev.same_screen = True;
    XSendEvent(d, window_, True, PointerMotionMask, reinterpret_cast<XEvent*>(&ev));
    XFlush(d);
}

void X11InputBackend::doClick(MouseButton button)
{
    const int b = button == MouseButton::Left ? Button1 : button == MouseButton::Right ? Button3 : Button2;
    sendButton(b, true);
    sendButton(b, false);
}

void X11InputBackend::doMouseDown(MouseButton button)
{
    const int b = button == MouseButton::Left ? Button1 : button == MouseButton::Right ? Button3 : Button2;
    sendButton(b, true);
}

void X11InputBackend::doMouseUp(MouseButton button)
{
    const int b = button == MouseButton::Left ? Button1 : button == MouseButton::Right ? Button3 : Button2;
    sendButton(b, false);
}

void X11InputBackend::doMouseScroll(int delta)
{
    const int b = delta < 0 ? Button5 : Button4; // 5 = WheelDown (zoom out), 4 = WheelUp (zoom in)
    const int steps = std::max(1, std::abs(delta) / 120);

    for (int i = 0; i < steps; ++i)
    {
        sendButton(b, true);
        sendButton(b, false);
    }
}

} // namespace og
