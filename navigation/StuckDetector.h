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
 *  File: StuckDetector.h
 *
 *  Desc:
 *  Declarations and interface for StuckDetector (navigation,
 *  pathfinding, movement control, and stuck recovery).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Vec3.h"

#include <optional>

namespace og
{

struct StuckConfig
{
    float minProgress = 0.5f; // yards that must be covered ...
    Duration window{2000};    // ... within this time while trying to move forward
};

// Detects "forward is held but we are not getting anywhere".
class StuckDetector
{
public:
    explicit StuckDetector(StuckConfig config = {}) : config_(config)
    {
    }

    // Call every tick. Returns true exactly once per detected stuck window.
    bool update(TimePoint now, const Vec3& position, bool tryingToMove);
    void reset()
    {
        reference_.reset();
    }

    const StuckConfig& config() const
    {
        return config_;
    }

    void setWindow(Duration window)
    {
        config_.window = window;
    }

private:
    StuckConfig config_;
    std::optional<Vec3> reference_;
    TimePoint referenceTime_{};
};

} // namespace og
