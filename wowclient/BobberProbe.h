/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-21
 *  File: BobberProbe.h
 *
 *  Desc:
 *  Declarations and interface for BobberProbe (WoW client memory
 *  reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace og::wowclient
{

// Finds a fishing bobber's "created by" and "bite" fields from samples of its
// object memory, for a fishing probe. createdBy is where the
// player's GUID sits in the bobber. The bite byte stays put from the cast
// until the fish bites, then changes once; the moment the player says they saw
// the splash narrows the bytes that do that down to the one that matters.
class BobberProbe
{
public:
    explicit BobberProbe(std::vector<std::uint8_t> playerGuid) : playerGuid_(std::move(playerGuid))
    {
    }

    // One read of the bobber's memory, `at` since the cast. Every sample must
    // be the same size.
    void sample(Duration at, std::span<const std::uint8_t> bytes);
    // When the splash was seen.
    void markBite(Duration at)
    {
        bite_ = at;
    }

    bool biteMarked() const
    {
        return bite_.has_value();
    }

    std::size_t samples() const
    {
        return samples_.size();
    }

    // Offsets (8-byte aligned) holding the player's GUID once the bobber has
    // settled (see biteCandidates).
    std::vector<std::uint32_t> createdByCandidates(Duration settle = Duration(2500)) const;

    struct BiteCandidate
    {
        std::uint32_t offset = 0;
        std::uint8_t before = 0;
        std::uint8_t after = 0;
        Duration changedAt{0};                                  // the first change
        int changes = 0;                                        // between settling and the end
        std::vector<std::pair<Duration, std::uint8_t>> history; // the first few changes
    };
    // Bytes constant from `settle` on (a new bobber is still being filled in
    // for the first second or two) until they first change. The last `tail` is
    // left out: that is the client tearing the object down. With a marked bite
    // the change has to fall at most `before` ahead of it or `after` past it.
    // 0 -> 1 first, then the fewest changes, then the closest to the mark.
    std::vector<BiteCandidate> biteCandidates(Duration settle = Duration(2500), Duration tail = Duration(500), Duration before = Duration(1500), Duration after = Duration(400)) const;

private:
    struct Sample
    {
        Duration at;
        std::vector<std::uint8_t> bytes;
    };
    std::vector<std::uint8_t> playerGuid_;
    std::vector<Sample> samples_;
    std::optional<Duration> bite_;
};

// Offsets present in every list, for combining several casts.
std::vector<std::uint32_t> commonOffsets(const std::vector<std::vector<std::uint32_t>>& lists);

} // namespace og::wowclient
