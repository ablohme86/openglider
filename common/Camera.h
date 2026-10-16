/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-15
 *  File: Camera.h
 *
 *  Desc:
 *  Declarations and interface for Camera (common utility functions,
 *  math, logging, and data structures).
 *
 *
 */
#pragma once

#include "og/common/MathUtil.h"
#include "og/common/Vec3.h"

#include <cmath>
#include <optional>

namespace og
{

struct ScreenPoint
{
    int x = 0;
    int y = 0;

    constexpr bool operator==(const ScreenPoint&) const = default;
};

// 3D Camera view and projection model for World-To-Screen transformations.
struct Camera
{
    Vec3 position;
    // 3x3 orientation matrix in camera space:
    // matrix[0] = Right vector (x, y, z)
    // matrix[1] = Up vector (x, y, z)
    // matrix[2] = Forward vector (x, y, z)
    float matrix[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}};
    float fov = 1.25f;           // Field of view in radians (horizontal in WoW)
    float aspect = 16.0f / 9.0f; // Width / Height aspect ratio

    // Transforms a 3D world coordinate into 2D window pixel coordinates.
    // Returns nullopt if the target is behind the camera or outside the viewport.
    std::optional<ScreenPoint> worldToScreen(const Vec3& worldPoint, int viewportWidth = 1920, int viewportHeight = 1080) const
    {
        if (viewportWidth <= 0 || viewportHeight <= 0)
        {
            return std::nullopt;
        }

        const Vec3 diff = worldPoint - position;

        // Transform into camera local space using dot products with camera axes.
        const float viewRight = diff.x * matrix[0][0] + diff.y * matrix[0][1] + diff.z * matrix[0][2];
        const float viewUp = diff.x * matrix[1][0] + diff.y * matrix[1][1] + diff.z * matrix[1][2];
        const float viewForward = diff.x * matrix[2][0] + diff.y * matrix[2][1] + diff.z * matrix[2][2];

        // Must be in front of the camera near plane
        if (viewForward < 0.2f)
        {
            return std::nullopt;
        }

        const float effectiveAspect = aspect > 0.01f ? aspect : (static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight));
        const float tanHalfFov = std::tan(fov * 0.5f);

        if (tanHalfFov < 0.001f)
        {
            return std::nullopt;
        }

        // Normalized Device Coordinates (-1.0 to 1.0)
        const float xNdc = viewRight / (viewForward * tanHalfFov);
        const float yNdc = viewUp / (viewForward * (tanHalfFov / effectiveAspect));

        if (std::abs(xNdc) > 1.0f || std::abs(yNdc) > 1.0f)
        {
            return std::nullopt; // Outside screen viewport
        }

        // Screen pixel coordinates (0,0 is top-left in window space)
        const int screenX = static_cast<int>((1.0f + xNdc) * 0.5f * static_cast<float>(viewportWidth));
        const int screenY = static_cast<int>((1.0f - yNdc) * 0.5f * static_cast<float>(viewportHeight));

        return ScreenPoint{screenX, screenY};
    }

    // 2D horizontal heading in radians (+X North, +Y West)
    float heading() const
    {
        return normalizeHeading(std::atan2(matrix[2][1], matrix[2][0]));
    }

    // Distance between camera position and a world point (e.g. player position = zoom distance)
    float distanceTo(const Vec3& p) const
    {
        const float dx = position.x - p.x;
        const float dy = position.y - p.y;
        const float dz = position.z - p.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Checks if the camera is facing back towards the player (opposite to player's heading)
    bool isFacingPlayer(float playerHeading, float threshold = 1.2f) const
    {
        const float delta = std::fabs(normalizeAngle(heading() - playerHeading));
        return delta > threshold;
    }

    // Horizontal angle (radians) from camera forward view to world point (positive = right, negative = left)
    float angleTo(const Vec3& worldPoint) const
    {
        const Vec3 diff = worldPoint - position;
        const float viewRight = diff.x * matrix[0][0] + diff.y * matrix[0][1] + diff.z * matrix[0][2];
        const float viewForward = diff.x * matrix[2][0] + diff.y * matrix[2][1] + diff.z * matrix[2][2];
        return std::atan2(viewRight, viewForward);
    }
};

} // namespace og
