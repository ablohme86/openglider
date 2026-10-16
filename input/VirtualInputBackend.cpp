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
 *  File: VirtualInputBackend.cpp
 *
 *  Desc:
 *  Implementation of VirtualInputBackend (low-level OS and Wine input
 *  simulation backends).
 *
 *
 */
#include "og/input/VirtualInputBackend.h"

#include <format>

namespace og
{

std::string InputEvent::toString() const
{
    switch (type)
    {
    case Type::KeyDown:
        return std::format("down {}", keyName(key));
    case Type::KeyUp:
        return std::format("up {}", keyName(key));
    case Type::MouseMove:
        return std::format("move {},{}", x, y);
    case Type::Click:
        return std::format("click {}", button == MouseButton::Left ? "left" : button == MouseButton::Right ? "right" : "middle");
    case Type::MouseDown:
        return std::format("mousedown {}", button == MouseButton::Left ? "left" : button == MouseButton::Right ? "right" : "middle");
    case Type::MouseUp:
        return std::format("mouseup {}", button == MouseButton::Left ? "left" : button == MouseButton::Right ? "right" : "middle");
    case Type::MouseScroll:
        return std::format("scroll {}", scrollDelta);
    case Type::Text:
        return std::format("text {}", text);
    }

    return "?";
}

std::vector<Key> VirtualInputBackend::pressedKeys() const
{
    std::vector<Key> out;

    for (const auto& e : events_)
    {
        if (e.type == InputEvent::Type::KeyDown)
        {
            out.push_back(e.key);
        }
    }

    return out;
}

} // namespace og
