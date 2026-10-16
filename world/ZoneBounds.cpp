/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-30
 *  File: ZoneBounds.cpp
 *
 *  Desc:
 *  Implementation of ZoneBounds (world model, travel graph, zone
 *  bounds, and quest databases).
 *
 *
 */
#include "og/world/ZoneBounds.h"

#include "og/common/Json.h"
#include "og/common/Sqlite.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace og
{

bool ZoneBounds::loadFromJson(std::string_view text, std::string* error)
{
    try
    {
        const Json j = Json::parse(text);
        const Json* zones = j.find("zones");

        if (!zones || !zones->isObject())
        {
            if (error)
            {
                *error = "expected a \"zones\" object";
            }

            return false;
        }

        for (const auto& [key, z] : zones->asObject())
        {
            std::uint32_t id = 0;
            std::from_chars(key.data(), key.data() + key.size(), id);
            const auto value = [&](std::string_view name)
            {
                const Json* v = z.find(name);
                return v ? v->asDouble() : 0.0;
            };
            const Bounds b{value("left"), value("right"), value("top"), value("bottom")};

            if (id && b.left != b.right && b.top != b.bottom)
            {
                bounds_[id] = b;
            }
        }

        return true;
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = e.what();
        }

        return false;
    }
}

bool ZoneBounds::loadFromDb(const SqliteDb& db, std::string* error)
{
    try
    {
        auto stmt = const_cast<SqliteDb&>(db).prepare("SELECT id, left, right, top, bottom FROM zones");
        while (stmt.step())
        {
            const auto id = stmt.columnUint32(0);
            const Bounds b{stmt.columnDouble(1), stmt.columnDouble(2), stmt.columnDouble(3), stmt.columnDouble(4)};
            if (id && b.left != b.right && b.top != b.bottom)
            {
                bounds_[id] = b;
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = e.what();
        }
        return false;
    }
}

bool ZoneBounds::loadFromFile(const std::string& path, std::string* error)
{
    if (path.ends_with(".db") || path.ends_with(".sqlite"))
    {
        SqliteDb db;
        if (db.openReadOnly(path, error))
        {
            return loadFromDb(db, error);
        }
        return false;
    }

    std::ifstream file(path);

    if (!file.is_open())
    {
        if (error)
        {
            *error = "could not open " + path;
        }

        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    return loadFromJson(ss.str(), error);
}

std::string ZoneBounds::toJson() const
{
    std::string out = "{\"zones\":{\n";
    bool first = true;

    for (const auto& [zone, b] : bounds_)
    {
        out += std::format("{}\"{}\":{{\"left\":{:.2f},\"right\":{:.2f},\"top\":{:.2f},\"bottom\":{:.2f}}}", first ? "" : ",\n", zone, b.left, b.right, b.top, b.bottom);
        first = false;
    }

    return out + "\n}}\n";
}

std::optional<ZoneBounds::Bounds> ZoneBounds::find(std::uint32_t zone) const
{
    const auto it = bounds_.find(zone);
    return it == bounds_.end() ? std::nullopt : std::optional(it->second);
}

std::optional<Vec3> ZoneBounds::toWorld(std::uint32_t zone, double mapX, double mapY) const
{
    const auto b = find(zone);

    if (!b)
    {
        return std::nullopt;
    }

    return Vec3{static_cast<float>(b->top - mapY / 100.0 * (b->top - b->bottom)), static_cast<float>(b->left - mapX / 100.0 * (b->left - b->right)), 0.0f};
}

bool ZoneBounds::calibrate(std::uint32_t zone, double mapX, double mapY, const Vec3& world)
{
    if (bounds_.contains(zone))
    {
        return false;
    }

    auto& samples = samples_[zone];

    for (const auto& s : samples)
    {
        if (std::abs(s.mapX - mapX) < 0.01 && std::abs(s.mapY - mapY) < 0.01)
        {
            return false; // seen already
        }
    }

    samples.push_back({mapX, mapY, world.x, world.y});
    // Far enough apart on both axes that a hundredth of a percent of rounding
    // in Questie's coordinates does not tilt the line: 5 % of a zone is some
    // 100-200 yards.
    const auto spread = [&](auto member)
    {
        const auto [lo, hi] = std::minmax_element(samples.begin(), samples.end(), [&](const Sample& a, const Sample& b) { return a.*member < b.*member; });
        return (*hi).*member - (*lo).*member;
    };

    if (samples.size() < 3 || spread(&Sample::mapX) < 5.0 || spread(&Sample::mapY) < 5.0)
    {
        return false;
    }

    // A straight line through the samples on each axis (least squares).
    const auto fit = [&](double Sample::* from, double Sample::* to)
    {
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;

        for (const auto& s : samples)
        {
            n += 1;
            sx += s.*from;
            sy += s.*to;
            sxx += s.*from * s.*from;
            sxy += s.*from * s.*to;
        }

        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        return std::pair{(sy - slope * sx) / n, slope};
    };
    const auto [top, perY] = fit(&Sample::mapY, &Sample::worldX);
    const auto [left, perX] = fit(&Sample::mapX, &Sample::worldY);
    bounds_[zone] = Bounds{left, left + 100.0 * perX, top, top + 100.0 * perY};
    return true;
}

} // namespace og
