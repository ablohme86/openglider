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
 *  File: WineBridgeInputBackend.h
 *
 *  Desc:
 *  Declarations and interface for WineBridgeInputBackend (low-level OS
 *  and Wine input simulation backends).
 *
 *
 */
#pragma once

#include "og/input/IInputBackend.h"
#include "og/input/WineBridgeProtocol.h"

#include <chrono>
#include <string>

namespace og
{

// Linux backend that forwards input to og-inputbridge.exe over TCP on
// localhost. The bridge runs in the same wine prefix as WoW.exe and uses
// PostMessage, giving true background input without focusing the window.
class WineBridgeInputBackend final : public IInputBackend
{
public:
    explicit WineBridgeInputBackend(std::string host = "127.0.0.1", unsigned short port = winebridge::kDefaultPort);
    ~WineBridgeInputBackend() override;

    std::string name() const override
    {
        return "winebridge";
    }

    bool isConnected() const override
    {
        return fd_ >= 0;
    }

    bool canTypeText() const override
    {
        return true;
    }

    // Asked of the bridge (RECT), which measures the WoW window.
    std::optional<std::pair<int, int>> windowSize() override;

    bool connect();
    void disconnect();
    // Sends PING and waits briefly for PONG. Returns true if the bridge found the game window.
    bool ping();
    // True when a bridge answers PING at all, with or without the game window.
    bool answers();

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
    bool send(const std::string& line);
    bool ensureConnected();
    // "PONG 0" or "PONG 1", empty when nothing answered.
    std::string pingReply();
    // Sends a command the bridge answers with one line; empty when nothing did.
    std::string request(const std::string& line);

    std::string host_;
    unsigned short port_;
    int fd_ = -1;
    std::chrono::steady_clock::time_point lastAttempt_{};
};

} // namespace og
