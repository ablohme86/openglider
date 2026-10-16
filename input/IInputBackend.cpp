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
 *  File: IInputBackend.cpp
 *
 *  Desc:
 *  Implementation of IInputBackend (low-level OS and Wine input
 *  simulation backends).
 *
 *
 */
#include "og/input/IInputBackend.h"

#include <chrono>
#include <thread>
#include <vector>

namespace og
{

void IInputBackend::keyDown(Key key)
{
    if (key == Key::None)
    {
        return;
    }

    held_.insert(key);
    lastKeyEvent_ = std::chrono::steady_clock::now();
    doKeyDown(key);
}

void IInputBackend::keyUp(Key key)
{
    if (key == Key::None)
    {
        return;
    }

    held_.erase(key);
    lastKeyEvent_ = std::chrono::steady_clock::now();
    doKeyUp(key);
}

void IInputBackend::settleBeforeTyping()
{
    releaseAll();
    const auto quiet = typingSettle();

    if (quiet <= std::chrono::milliseconds(0) || lastKeyEvent_ == std::chrono::steady_clock::time_point{})
    {
        return;
    }

    const auto since = std::chrono::steady_clock::now() - lastKeyEvent_;

    if (since < quiet)
    {
        std::this_thread::sleep_for(quiet - since);
    }
}

void IInputBackend::press(const KeyChord& chord)
{
    if (!chord.valid())
    {
        return;
    }

    std::vector<Key> mods;

    if (chord.modifiers & ModCtrl)
    {
        mods.push_back(Key::Control);
    }

    if (chord.modifiers & ModAlt)
    {
        mods.push_back(Key::Alt);
    }

    if (chord.modifiers & ModShift)
    {
        mods.push_back(Key::Shift);
    }

    // Only press modifiers that are not already held, and only release those
    // we pressed ourselves.
    std::vector<Key> pressedMods;

    for (Key m : mods)
    {
        if (!isHeld(m))
        {
            keyDown(m);
            pressedMods.push_back(m);
        }
    }

    const bool wasHeld = isHeld(chord.key);
    keyDown(chord.key);

    if (!wasHeld)
    {
        // Hold key down briefly so game engine / UI message loop registers the key press across frames.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        keyUp(chord.key);
    }

    for (auto it = pressedMods.rbegin(); it != pressedMods.rend(); ++it)
    {
        keyUp(*it);
    }
}

void IInputBackend::releaseAll()
{
    const std::set<Key> keys = held_;

    for (Key k : keys)
    {
        keyUp(k);
    }
}

} // namespace og
