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
 *  File: WineBridgeProtocol.h
 *
 *  Desc:
 *  Line-based protocol between the engine (Linux) and og-
 *  inputbridge.exe (running inside the same wine prefix as WoW.exe).
 *
 *
 */
#pragma once

// Line-based protocol between the engine (Linux) and og-inputbridge.exe
// (running inside the same wine prefix as WoW.exe). One command per line:
//
//   KD <vk> <extended>      key down   (vk = Windows virtual key, decimal)
//   KU <vk> <extended>      key up
//   MM <x> <y>              mouse move (client coordinates)
//   MB <button> <down>      mouse button, button 0=left 1=right 2=middle
//   CHAR <code>             one character (WM_CHAR) into the focused edit box, e.g. the chat
//   PING                    bridge answers "PONG <window-found 0|1>"
//   CAP <maxWidth>          a picture of the game window, scaled to at most maxWidth:
//                           "FRAME <width> <height> <bytes>\n" and then that many
//                           bytes of top-down BGR rows; "FRAME 0 0 0\n" without a window
//
// The bridge PostMessages every command to the WoW window, which delivers the
// input even when the window is not focused.

namespace og::winebridge
{
inline constexpr unsigned short kDefaultPort = 47800;
}
