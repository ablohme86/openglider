/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-28
 *  File: Difficulty.h
 *
 *  Desc:
 *  Declarations and interface for Difficulty (game state
 *  representations, snapshots, and state providers).
 *
 *
 */
#pragma once

#include <string_view>

namespace og
{

// Highest mob level that is grey to a player of this level - one that gives no
// experience. Matches the thresholds the client colours names by.
inline int grayLevel(int playerLevel)
{
    if (playerLevel <= 5)
    {
        return 0;
    }

    if (playerLevel <= 39)
    {
        return playerLevel - 5 - playerLevel / 10;
    }

    if (playerLevel <= 59)
    {
        return playerLevel - 1 - playerLevel / 5;
    }

    return playerLevel - 9;
}

// How the client colours a mob's level against the player's.
enum class Difficulty
{
    Red,
    Orange,
    Yellow,
    Green,
    Gray
};

inline Difficulty difficultyOf(int mobLevel, int playerLevel)
{
    // Skull (level unknown to the client): as far above as it gets.
    if (mobLevel <= 0)
    {
        return Difficulty::Red;
    }

    const int diff = mobLevel - playerLevel;

    if (diff >= 5)
    {
        return Difficulty::Red;
    }

    if (diff >= 3)
    {
        return Difficulty::Orange;
    }

    if (diff >= -2)
    {
        return Difficulty::Yellow;
    }

    // Above the grey threshold it still gives experience.
    if (mobLevel > grayLevel(playerLevel))
    {
        return Difficulty::Green;
    }

    return Difficulty::Gray;
}

inline std::string_view toString(Difficulty d)
{
    switch (d)
    {
    case Difficulty::Red:
        return "Red";
    case Difficulty::Orange:
        return "Orange";
    case Difficulty::Yellow:
        return "Yellow";
    case Difficulty::Green:
        return "Green";
    case Difficulty::Gray:
        return "Gray";
    }

    return "Gray";
}

} // namespace og
