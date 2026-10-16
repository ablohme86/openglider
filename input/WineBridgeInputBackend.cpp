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
 *  File: WineBridgeInputBackend.cpp
 *
 *  Desc:
 *  Implementation of WineBridgeInputBackend (low-level OS and Wine
 *  input simulation backends).
 *
 *
 */
#include "og/input/WineBridgeInputBackend.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>

namespace og
{

WineBridgeInputBackend::WineBridgeInputBackend(std::string host, unsigned short port) : host_(std::move(host)), port_(port)
{
}

WineBridgeInputBackend::~WineBridgeInputBackend()
{
    releaseAll();
    disconnect();
}

bool WineBridgeInputBackend::connect()
{
    disconnect();
    lastAttempt_ = std::chrono::steady_clock::now();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1)
    {
        ::close(fd);
        return false;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return false;
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fd_ = fd;
    send("RELEASEALL\n");
    return true;
}

void WineBridgeInputBackend::disconnect()
{
    if (fd_ >= 0)
    {
        send("RELEASEALL\n");
        ::close(fd_);
    }

    fd_ = -1;
}

bool WineBridgeInputBackend::ensureConnected()
{
    if (fd_ >= 0)
    {
        return true;
    }

    // Avoid hammering connect() every tick while the bridge is not running.
    if (std::chrono::steady_clock::now() - lastAttempt_ < std::chrono::seconds(2))
    {
        return false;
    }

    return connect();
}

bool WineBridgeInputBackend::send(const std::string& line)
{
    if (!ensureConnected())
    {
        return false;
    }

    const char* data = line.data();
    std::size_t left = line.size();

    while (left > 0)
    {
        const ssize_t n = ::send(fd_, data, left, MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            disconnect();
            return false;
        }

        data += n;
        left -= static_cast<std::size_t>(n);
    }

    return true;
}

std::string WineBridgeInputBackend::pingReply()
{
    return request("PING\n");
}

std::optional<std::pair<int, int>> WineBridgeInputBackend::windowSize()
{
    int width = 0;
    int height = 0;

    // "RECT 0 0" while the bridge has no window.
    if (std::sscanf(request("RECT\n").c_str(), "RECT %d %d", &width, &height) != 2 || width <= 0 || height <= 0)
    {
        return std::nullopt;
    }

    return std::pair{width, height};
}

std::string WineBridgeInputBackend::request(const std::string& line)
{
    if (!send(line))
    {
        return {};
    }

    pollfd pfd{fd_, POLLIN, 0};

    if (::poll(&pfd, 1, 500) <= 0)
    {
        return {};
    }

    char buf[64] = {};
    const ssize_t n = ::recv(fd_, buf, sizeof(buf) - 1, 0);

    if (n <= 0)
    {
        disconnect();
        return {};
    }

    return buf;
}

bool WineBridgeInputBackend::ping()
{
    return pingReply().starts_with("PONG 1");
}

bool WineBridgeInputBackend::answers()
{
    return pingReply().starts_with("PONG");
}

void WineBridgeInputBackend::doKeyDown(Key key)
{
    send(std::format("KD {} {}\n", toWindowsVirtualKey(key), isExtendedKey(key) ? 1 : 0));
}

void WineBridgeInputBackend::doKeyUp(Key key)
{
    send(std::format("KU {} {}\n", toWindowsVirtualKey(key), isExtendedKey(key) ? 1 : 0));
}

void WineBridgeInputBackend::doMouseMove(int x, int y)
{
    send(std::format("MM {} {}\n", x, y));
}

bool WineBridgeInputBackend::doTypeText(std::string_view text)
{
    // One character per line, as its code point; ASCII is all the bot types.
    std::string lines;

    for (const char c : text)
    {
        lines += std::format("CHAR {}\n", static_cast<unsigned char>(c));
    }

    return send(lines);
}

void WineBridgeInputBackend::doClick(MouseButton button)
{
    const int b = button == MouseButton::Left ? 0 : button == MouseButton::Right ? 1 : 2;
    send(std::format("MB {} 1\nMB {} 0\n", b, b));
}

void WineBridgeInputBackend::doMouseDown(MouseButton button)
{
    const int b = button == MouseButton::Left ? 0 : button == MouseButton::Right ? 1 : 2;
    send(std::format("MB {} 1\n", b));
}

void WineBridgeInputBackend::doMouseUp(MouseButton button)
{
    const int b = button == MouseButton::Left ? 0 : button == MouseButton::Right ? 1 : 2;
    send(std::format("MB {} 0\n", b));
}

void WineBridgeInputBackend::doMouseScroll(int delta)
{
    send(std::format("MW {}\n", delta));
}

} // namespace og
