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
 *  File: StuckDetector.cpp
 *
 *  Desc:
 *  Implementation of StuckDetector (navigation, pathfinding, movement
 *  control, and stuck recovery).
 *
 *
 */
#include "og/navigation/StuckDetector.h"

namespace og
{

bool StuckDetector::update(TimePoint now, const Vec3& position, bool tryingToMove)
{
    if (!tryingToMove)
    {
        reference_.reset();
        return false;
    }

    if (!reference_ || distance2D(*reference_, position) >= config_.minProgress)
    {
        reference_ = position;
        referenceTime_ = now;
        return false;
    }

    if (now - referenceTime_ >= config_.window)
    {
        // Start a new window so we report again only if we stay stuck.
        reference_ = position;
        referenceTime_ = now;
        return true;
    }

    return false;
}

} // namespace og
