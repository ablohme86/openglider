/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-18
 *  File: ObjectFootprints.cpp
 *
 *  Desc:
 *  Implementation of ObjectFootprints: game objects' models' collision,
 *  as zones the way goes round.
 *
 *
 */
#include "og/navigation/ObjectFootprints.h"

#include "og/common/Db2.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace og
{

namespace
{

// GameObjectDisplayInfo's fields: GeoBox (six floats), then the model's FileDataID.
constexpr std::size_t kFileDataIdField = 6;
// Smaller than this, the character walks by it as it is: a candle on the ground.
constexpr float kSmallest = 0.3f;

} // namespace

std::optional<ObjectFootprints> ObjectFootprints::fromDb2(std::span<const std::uint8_t> displayInfo, ClientFileReader read, std::string& error)
{
    const auto table = Db2Table::parse(displayInfo, error);

    if (!table)
    {
        return std::nullopt;
    }

    std::unordered_map<std::uint32_t, std::uint32_t> models;

    for (const auto& row : table->rows())
    {
        if (row.fields.size() > kFileDataIdField && row.fields[kFileDataIdField] != 0)
        {
            models.emplace(row.id, row.fields[kFileDataIdField]);
        }
    }

    if (models.empty())
    {
        error = "no models in GameObjectDisplayInfo";
        return std::nullopt;
    }

    return ObjectFootprints(std::move(models), std::move(read));
}

ObjectFootprints::ObjectFootprints(std::unordered_map<std::uint32_t, std::uint32_t> models, ClientFileReader read) : models_(std::move(models)), read_(std::move(read))
{
}

std::optional<ObjectFootprints::Footprint> ObjectFootprints::of(std::uint32_t displayId)
{
    if (const auto it = known_.find(displayId); it != known_.end())
    {
        return it->second;
    }

    std::optional<Footprint> footprint;
    const auto model = models_.find(displayId);

    if (model != models_.end() && read_)
    {
        try
        {
            const auto bytes = read_(model->second);
            const CollisionGeometry g = bytes.empty() ? CollisionGeometry{} : parseModelCollision(bytes);

            if (!g.vertices.empty() && !g.triangles.empty())
            {
                Footprint f{0.0f, g.vertices.front().z, g.vertices.front().z};

                for (const Vec3& v : g.vertices)
                {
                    f.radius = std::max(f.radius, std::hypot(v.x, v.y));
                    f.bottom = std::min(f.bottom, v.z);
                    f.top = std::max(f.top, v.z);
                }

                footprint = f;
            }
        }
        catch (const std::exception&)
        {
            // A model not read is one not gone round.
        }
    }

    known_.emplace(displayId, footprint);
    return footprint;
}

std::vector<AvoidZone> ObjectFootprints::zones(const GameSnapshot& snapshot, float range)
{
    std::vector<AvoidZone> zones;
    const Vec3& at = snapshot.player.position;

    for (const GameObjectState& o : snapshot.objects)
    {
        if (o.kind != GameObjectKind::Generic || o.displayId == 0 || distance2D(o.position, at) > range)
        {
            continue;
        }

        const auto f = of(o.displayId);

        if (!f || f->radius * o.scale < kSmallest)
        {
            continue;
        }

        const float bottom = o.position.z + f->bottom * o.scale;
        const float top = o.position.z + f->top * o.scale;
        // Up the height of it, and a little over and under: a candle on a
        // table is above the floor the character walks on, a lantern hung
        // overhead over it.
        zones.push_back(AvoidZone{Vec3{o.position.x, o.position.y, (bottom + top) / 2.0f}, f->radius * o.scale, o.name, false, (top - bottom) / 2.0f + 1.0f});
    }

    return zones;
}

} // namespace og
