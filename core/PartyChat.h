/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-04
 *  File: PartyChat.h
 *
 *  Desc:
 *  Declarations and interface for PartyChat (bot engine, state
 *  machines, and automated behaviors).
 *
 *
 */
#pragma once

#include "og/common/MathUtil.h"

#include <array>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

// What the party bots say to each other in party chat (docs/PartySystem.md):
// each kind has several lines, from data/party_messages.txt, so it reads like
// players talking. A line is known by any line of its kind, whatever its case
// and punctuation; a position line by its template, "im at {x} {y}".
class PartyChat
{
public:
    enum class Kind
    {
        Vendor,     // my bags are full, or my gear needs repair
        VendorDone, // done at the vendor, go on
        Lost,       // I cannot see the leader
        Position,   // where I am: {x} {y}, the world's coordinates
        Ack         // understood
    };
    static constexpr std::size_t kKinds = 5;

    // A line heard: what it is, and for a position, the place (height 0).
    struct Heard
    {
        Kind kind = Kind::Ack;
        std::optional<Vec3> place;
    };

    // "[vendor]" and the lines under it; '#' starts a comment. False if no
    // kind has a line.
    bool loadText(std::string_view text);
    bool loadFile(const std::string& path);

    std::optional<Heard> hear(std::string_view text) const;
    // A line of the kind, not the one said last for it; a position line with
    // the place filled in. Empty if the kind has no line.
    std::string say(Kind kind, std::mt19937& random, const Vec3& place = {});

    static std::string_view nameOf(Kind kind);

private:
    std::array<std::vector<std::string>, kKinds> lines_;
    std::array<int, kKinds> last_{-1, -1, -1, -1, -1};
};

} // namespace og
