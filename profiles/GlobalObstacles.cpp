/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-29
 *  File: GlobalObstacles.cpp
 *
 *  Desc:
 *  Implementation of GlobalObstacles (glider profile parsing,
 *  validation, and recording).
 *
 *
 */
#include "og/profiles/GlobalObstacles.h"

#include "og/common/FileUtil.h"
#include "og/common/Json.h"
#include "og/profiles/ProfileLoader.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace og
{

namespace
{

std::string_view kindName(GlobalObstacle::Kind kind)
{
    switch (kind)
    {
    case GlobalObstacle::Kind::AvoidZone:
        return "avoidZone";
    case GlobalObstacle::Kind::Maneuver:
        return "maneuver";
    case GlobalObstacle::Kind::Detour:
        return "detour";
    }

    return "avoidZone";
}

std::optional<GlobalObstacle::Kind> parseKind(std::string_view name)
{
    if (name == "avoidZone")
    {
        return GlobalObstacle::Kind::AvoidZone;
    }

    if (name == "maneuver")
    {
        return GlobalObstacle::Kind::Maneuver;
    }

    if (name == "detour")
    {
        return GlobalObstacle::Kind::Detour;
    }

    return std::nullopt;
}

double round2(float v)
{
    return std::round(static_cast<double>(v) * 100.0) / 100.0;
}

Json point(const Vec3& p)
{
    return Json(Json::Array{round2(p.x), round2(p.y), round2(p.z)});
}

std::optional<Vec3> readPoint(const Json& p)
{
    if (p.isArray() && p.size() >= 3 && p[0].isNumber() && p[1].isNumber() && p[2].isNumber())
    {
        return Vec3{p[0].asFloat(), p[1].asFloat(), p[2].asFloat()};
    }

    return std::nullopt;
}

} // namespace

bool GlobalObstacles::add(GlobalObstacle obstacle)
{
    for (GlobalObstacle& known : obstacles_)
    {
        if (known.kind != obstacle.kind || distance(known.position, obstacle.position) > kSameSpot)
        {
            continue;
        }

        if (known.kind == GlobalObstacle::Kind::Maneuver && known.maneuver != obstacle.maneuver)
        {
            continue;
        }

        // Two continents can share coordinates; only a known, different map tells them apart.
        if (known.mapId && obstacle.mapId && *known.mapId != *obstacle.mapId)
        {
            continue;
        }

        if (!known.mapId && obstacle.mapId)
        {
            known.mapId = obstacle.mapId;
            changed_ = true;
        }

        return false;
    }

    obstacles_.push_back(std::move(obstacle));
    changed_ = true;
    return true;
}

int GlobalObstacles::merge(const LearnedObstacles& learned, std::optional<std::uint32_t> mapId, const std::string& source)
{
    int added = 0;

    for (const AvoidZone& zone : learned.avoidZones)
    {
        GlobalObstacle o;
        o.kind = GlobalObstacle::Kind::AvoidZone;
        o.mapId = mapId;
        o.position = zone.center;
        o.radius = zone.radius;
        o.source = source;
        added += add(std::move(o)) ? 1 : 0;
    }

    for (const LearnedManeuver& m : learned.maneuvers)
    {
        GlobalObstacle o;
        o.kind = GlobalObstacle::Kind::Maneuver;
        o.mapId = mapId;
        o.position = m.position;
        o.heading = m.heading;
        o.maneuver = m.maneuver;
        o.source = source;
        added += add(std::move(o)) ? 1 : 0;
    }

    for (const LearnedDetour& d : learned.detours)
    {
        GlobalObstacle o;
        o.kind = GlobalObstacle::Kind::Detour;
        o.mapId = mapId;
        o.position = d.stuckAt;
        o.from = d.from;
        o.to = d.to;
        o.points = d.points;
        o.source = source;
        added += add(std::move(o)) ? 1 : 0;
    }

    return added;
}

int GlobalObstacles::tagZone(std::uint32_t mapId, std::uint32_t zoneId, const Vec3& at, float radius)
{
    if (zoneId == 0)
    {
        return 0;
    }

    int tagged = 0;

    for (GlobalObstacle& o : obstacles_)
    {
        // A spot with no map could be on another continent under the same coordinates.
        if (o.zoneId || !o.mapId || *o.mapId != mapId || distance(o.position, at) > radius)
        {
            continue;
        }

        o.zoneId = zoneId;
        ++tagged;
    }

    if (tagged > 0)
    {
        changed_ = true;
    }

    return tagged;
}

int GlobalObstacles::importProfiles(const std::filesystem::path& directory)
{
    std::error_code ec;
    std::vector<std::filesystem::path> files;

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            files.push_back(entry.path());
        }
    }

    // The same order every time, so the file does not reshuffle.
    std::sort(files.begin(), files.end());
    int added = 0;

    for (const auto& file : files)
    {
        const auto loaded = loadProfile(file);

        if (!loaded.profile)
        {
            continue;
        }

        added += merge(loaded.profile->learned, std::nullopt, file.filename().string());
    }

    return added;
}

bool GlobalObstacles::load(const std::filesystem::path& path, std::string* error)
{
    obstacles_.clear();
    changed_ = false;

    if (!std::filesystem::exists(path))
    {
        return true;
    }

    const auto text = readTextFile(path);

    if (!text)
    {
        if (error)
        {
            *error = std::format("cannot read {}", path.string());
        }

        return false;
    }

    Json root;

    try
    {
        root = Json::parse(*text);
    }
    catch (const std::exception& e)
    {
        if (error)
        {
            *error = std::format("{}: {}", path.string(), e.what());
        }

        return false;
    }

    for (const Json& e : root["obstacles"].asArray())
    {
        const auto kind = parseKind(e["kind"].asString(""));
        const auto position = readPoint(e["position"]);

        if (!kind || !position)
        {
            continue;
        }

        GlobalObstacle o;
        o.kind = *kind;
        o.position = *position;

        if (e["map"].isNumber())
        {
            o.mapId = static_cast<std::uint32_t>(e["map"].asInt(0));
        }

        if (e["zone"].isNumber())
        {
            o.zoneId = static_cast<std::uint32_t>(e["zone"].asInt(0));
        }

        o.source = e["source"].asString("");

        if (o.kind == GlobalObstacle::Kind::AvoidZone)
        {
            o.radius = std::max(1.0f, e["radius"].asFloat(6.0f));
        }

        if (o.kind == GlobalObstacle::Kind::Maneuver)
        {
            const auto maneuver = parseObstacleManeuver(e["action"].asString(""));

            if (!maneuver)
            {
                continue;
            }

            o.maneuver = *maneuver;
            o.heading = e["heading"].asFloat(0.0f);
        }

        if (o.kind == GlobalObstacle::Kind::Detour)
        {
            const auto from = readPoint(e["from"]);
            const auto to = readPoint(e["to"]);

            if (!from || !to)
            {
                continue;
            }

            o.from = *from;
            o.to = *to;

            for (const Json& p : e["points"].asArray())
            {
                if (const auto at = readPoint(p))
                {
                    o.points.push_back(*at);
                }
            }

            if (o.points.empty())
            {
                continue;
            }
        }

        obstacles_.push_back(std::move(o));
    }

    return true;
}

bool GlobalObstacles::save(const std::filesystem::path& path) const
{
    Json list = Json::array();

    for (const GlobalObstacle& o : obstacles_)
    {
        Json e = Json::object();
        e["kind"] = std::string(kindName(o.kind));

        if (o.mapId)
        {
            e["map"] = static_cast<std::int64_t>(*o.mapId);
        }

        if (o.zoneId)
        {
            e["zone"] = static_cast<std::int64_t>(*o.zoneId);
        }

        e["position"] = point(o.position);

        switch (o.kind)
        {
        case GlobalObstacle::Kind::AvoidZone:
            e["radius"] = round2(o.radius);
            break;
        case GlobalObstacle::Kind::Maneuver:
            e["action"] = std::string(toString(o.maneuver));
            e["heading"] = std::round(static_cast<double>(o.heading) * 1000.0) / 1000.0;
            break;
        case GlobalObstacle::Kind::Detour:
        {
            e["from"] = point(o.from);
            e["to"] = point(o.to);
            Json points = Json::array();

            for (const Vec3& p : o.points)
            {
                points.push_back(point(p));
            }

            e["points"] = std::move(points);
            break;
        }
        }

        e["source"] = o.source;
        list.push_back(std::move(e));
    }

    Json root = Json::object();
    root["obstacles"] = std::move(list);

    if (!writeTextFile(path, root.dump(2) + "\n"))
    {
        return false;
    }

    changed_ = false;
    return true;
}

} // namespace og
