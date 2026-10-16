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
 *  File: Typematic.h
 *
 *  Desc:
 *  When a held key repeats, as a keyboard held down does on Windows:
 *  after a delay, the key pressed last sends key-down again and again
 *  until it is let go of.
 *
 *
 */
#pragma once

// When a held key repeats, as a keyboard held down does on Windows: after a
// delay, the key pressed last sends key-down again and again until it is let
// go of. Only the last key repeats; one pressed earlier and still held stays
// quiet, as on a real keyboard. Times are milliseconds on any steady clock.
//
// No platform code, so the Linux tests cover it; og-inputbridge.exe posts the
// repeats it asks for.

#include <cstdint>
#include <optional>

namespace og
{

class Typematic
{
public:
    // Windows' defaults: a delay of about 500 ms, then about 30 repeats a second.
    struct Config
    {
        std::uint32_t delayMin = 480;
        std::uint32_t delayMax = 540;
        std::uint32_t intervalMin = 31;
        std::uint32_t intervalMax = 36;
    };

    Typematic() = default;
    explicit Typematic(Config config, std::uint64_t seed = 0x9E3779B97F4A7C15ull) : config_(config), random_(seed ? seed : 1)
    {
    }

    // `key` went down at `now`: it is the one that repeats, from after the delay.
    void pressed(std::uint32_t key, std::uint64_t now)
    {
        key_ = key;
        next_ = now + draw(config_.delayMin, config_.delayMax);
    }

    // `key` was let go of; the repeating stops if it was the one repeating.
    void released(std::uint32_t key)
    {
        if (key_ == key)
        {
            key_.reset();
        }
    }

    void clear()
    {
        key_.reset();
    }

    // The key to repeat now, if its time has come; the next repeat is then
    // one interval on. A late call repeats once, not for every repeat it missed.
    std::optional<std::uint32_t> due(std::uint64_t now)
    {
        if (!key_ || now < next_)
        {
            return std::nullopt;
        }

        next_ = now + draw(config_.intervalMin, config_.intervalMax);
        return key_;
    }

    // When due() next has something: nullopt while no key repeats.
    std::optional<std::uint64_t> nextAt() const
    {
        if (!key_)
        {
            return std::nullopt;
        }

        return next_;
    }

private:
    std::uint64_t draw(std::uint32_t low, std::uint32_t high)
    {
        // xorshift64*
        random_ ^= random_ >> 12;
        random_ ^= random_ << 25;
        random_ ^= random_ >> 27;
        const std::uint64_t value = random_ * 0x2545F4914F6CDD1Dull;
        return high > low ? low + value % (high - low + 1) : low;
    }

    Config config_{};
    std::uint64_t random_ = 0x9E3779B97F4A7C15ull;
    std::optional<std::uint32_t> key_;
    std::uint64_t next_ = 0;
};

} // namespace og
