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
 *  File: InputBackendFactory.h
 *
 *  Desc:
 *  Declarations and interface for InputBackendFactory (low-level OS and
 *  Wine input simulation backends).
 *
 *
 */
#pragma once

#include "og/input/IInputBackend.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

struct InputBackendConfig
{
    // "virtual", "winebridge", "x11" or "win32". "auto" picks win32 on Windows
    // and winebridge on Linux.
    std::string type = "auto";
    std::string host = "127.0.0.1";
    // 0: the bridge the application started before, a hand-started one on the default
    // port, or else a new bridge on a random port. Any other port is used as is.
    unsigned short port = 0;
    std::string windowTitle = "World of Warcraft";
    unsigned long windowId = 0;
    std::uint32_t processId = 0;
};

// Returns nullptr with `error` set if the backend is not available in this build.
std::unique_ptr<IInputBackend> createInputBackend(const InputBackendConfig& config, std::string* error = nullptr);
std::vector<std::string> availableInputBackends();

// Where og-inputbridge most likely listens: the random port the application started
// it on last, or the default port a bridge started by hand listens on.
unsigned short knownInputBridgePort();

// og-inputbridge up and listening, for a launcher to see to before the UI
// starts. Linux only: on Windows there is no bridge, and nothing is done.
struct InputBridgeStatus
{
    bool running = false;        // listening now
    bool alreadyRunning = false; // was listening before this call
    unsigned short port = 0;
    std::string error; // why it is not running
};
// Starts the bridge on a random port unless one listens on the remembered or
// the default port. Only looks whether something listens there: a bridge
// client lets go of every held key on connecting, and a bot may be running.
InputBridgeStatus ensureInputBridge();

#ifndef _WIN32
// Stops the og-inputbridge.exe processes of this user - the Wine process and
// its launchers - except the one listening on `keepPort` (0: all of them), so
// sessions do not leave one each behind. Done before a new one is started,
// and when an old one is taken on. `exeName` is for tests, which must not
// touch the real ones.
void stopOtherInputBridges(unsigned short keepPort, std::string_view exeName = "og-inputbridge.exe");
#endif

} // namespace og
