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
 *  File: ProfileLoader.cpp
 *
 *  Desc:
 *  Implementation of ProfileLoader (glider profile parsing, validation,
 *  and recording).
 *
 *
 */
#include "og/profiles/ProfileLoader.h"

#include "og/common/FileUtil.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace og
{

namespace
{

std::vector<Vec3> readPoints(const Json& arr, const char* field, std::vector<std::string>& errors)
{
    std::vector<Vec3> out;

    if (arr.isNull())
    {
        return out;
    }

    if (!arr.isArray())
    {
        errors.push_back(std::format("'{}' must be an array of [x, y, z]", field));
        return out;
    }

    for (std::size_t i = 0; i < arr.size(); ++i)
    {
        const Json& p = arr[i];
        const bool valid = (p.isArray() && p.size() >= 3 && p[0].isNumber() && p[1].isNumber() && p[2].isNumber()) || (p.isObject() && p["x"].isNumber() && p["y"].isNumber() && p["z"].isNumber());

        if (!valid)
        {
            errors.push_back(std::format("{}[{}]: expected [x, y, z]", field, i));
            continue;
        }

        if (p.isArray())
        {
            out.push_back({p[0].asFloat(), p[1].asFloat(), p[2].asFloat()});
        }
        else
        {
            out.push_back({p["x"].asFloat(), p["y"].asFloat(), p["z"].asFloat()});
        }
    }

    return out;
}

std::optional<Vec3> readPoint(const Json& p)
{
    if (p.isArray() && p.size() >= 3 && p[0].isNumber() && p[1].isNumber() && p[2].isNumber())
    {
        return Vec3{p[0].asFloat(), p[1].asFloat(), p[2].asFloat()};
    }

    return std::nullopt;
}

// The bot writes this section itself, so an entry it cannot read is skipped
// rather than refusing the whole profile.
LearnedObstacles readLearned(const Json& learned)
{
    LearnedObstacles out;

    for (const auto& m : learned["maneuvers"].asArray())
    {
        const auto position = readPoint(m["position"]);
        const auto maneuver = parseObstacleManeuver(m["action"].asString(""));

        if (!position || !maneuver)
        {
            continue;
        }

        LearnedManeuver entry;
        entry.position = *position;
        entry.heading = m["heading"].asFloat(0.0f);
        entry.maneuver = *maneuver;
        entry.successes = std::max(1, static_cast<int>(m["successes"].asInt(1)));
        entry.failures = std::max(0, static_cast<int>(m["failures"].asInt(0)));
        out.maneuvers.push_back(entry);
    }

    for (const auto& d : learned["detours"].asArray())
    {
        const auto from = readPoint(d["from"]);
        const auto to = readPoint(d["to"]);
        std::vector<std::string> ignored;
        LearnedDetour entry;
        entry.points = readPoints(d["points"], "learned.detours.points", ignored);

        if (!from || !to || entry.points.empty() || !ignored.empty())
        {
            continue;
        }

        entry.from = *from;
        entry.to = *to;
        entry.stuckAt = readPoint(d["stuckAt"]).value_or(*to);
        out.detours.push_back(std::move(entry));
    }

    return out;
}

// Spots free roaming got stuck at, kept in their own "freeroamObstacles" section.
std::vector<AvoidZone> readFreeroamObstacles(const Json& zones)
{
    std::vector<AvoidZone> out;

    for (const auto& z : zones.asArray())
    {
        const auto center = readPoint(z["center"]);

        if (!center)
        {
            continue;
        }

        out.push_back(AvoidZone{*center, std::max(1.0f, z["radius"].asFloat(6.0f))});
    }

    return out;
}

std::vector<std::string> readStrings(const Json& arr)
{
    std::vector<std::string> out;

    for (const auto& v : arr.asArray())
    {
        if (v.isString())
        {
            out.push_back(v.asString());
        }
    }

    return out;
}

double round2(float v)
{
    return std::round(static_cast<double>(v) * 100.0) / 100.0;
}

Json pointsToJson(const std::vector<Vec3>& pts)
{
    Json arr = Json::array();

    for (const auto& p : pts)
    {
        arr.push_back(Json(Json::Array{round2(p.x), round2(p.y), round2(p.z)}));
    }

    return arr;
}

Json pointToJson(const Vec3& p)
{
    return Json(Json::Array{round2(p.x), round2(p.y), round2(p.z)});
}

Json learnedToJson(const LearnedObstacles& learned)
{
    Json maneuvers = Json::array();

    for (const auto& m : learned.maneuvers)
    {
        Json e = Json::object();
        e["position"] = pointToJson(m.position);
        e["heading"] = std::round(static_cast<double>(m.heading) * 1000.0) / 1000.0;
        e["action"] = std::string(toString(m.maneuver));
        e["successes"] = static_cast<std::int64_t>(m.successes);
        e["failures"] = static_cast<std::int64_t>(m.failures);
        maneuvers.push_back(std::move(e));
    }

    Json detours = Json::array();

    for (const auto& d : learned.detours)
    {
        Json e = Json::object();
        e["from"] = pointToJson(d.from);
        e["to"] = pointToJson(d.to);
        e["stuckAt"] = pointToJson(d.stuckAt);
        e["points"] = pointsToJson(d.points);
        detours.push_back(std::move(e));
    }

    Json out = Json::object();
    out["maneuvers"] = std::move(maneuvers);
    out["detours"] = std::move(detours);
    return out;
}

Json freeroamObstaclesToJson(const std::vector<AvoidZone>& zones)
{
    Json out = Json::array();

    for (const auto& z : zones)
    {
        Json e = Json::object();
        e["center"] = pointToJson(z.center);
        e["radius"] = round2(z.radius);
        out.push_back(std::move(e));
    }

    return out;
}

Json stringsToJson(const std::vector<std::string>& v)
{
    Json arr = Json::array();

    for (const auto& s : v)
    {
        arr.push_back(s);
    }

    return arr;
}

} // namespace

ProfileLoadResult parseProfile(const std::string& jsonText)
{
    ProfileLoadResult result;
    Json root;

    try
    {
        root = Json::parse(jsonText);
    }
    catch (const JsonError& e)
    {
        result.errors.push_back(e.what());
        return result;
    }

    if (!root.isObject())
    {
        result.errors.push_back("profile must be a JSON object");
        return result;
    }

    GliderProfile p;
    p.name = root["name"].asString("Unnamed");

    if (const Json* level = root.find("level"))
    {
        p.minLevel = static_cast<int>((*level)["min"].asInt(1));
        p.maxLevel = static_cast<int>((*level)["max"].asInt(255));
    }

    p.targets = readStrings(root["targets"]);
    p.exclusions = readStrings(root["exclusions"]);
    p.waypoints = readPoints(root["waypoints"], "waypoints", result.errors);
    p.ghostWaypoints = readPoints(root["ghostWaypoints"], "ghostWaypoints", result.errors);
    p.vendorWaypoints = readPoints(root["vendorWaypoints"], "vendorWaypoints", result.errors);
    p.vendorName = root["vendorName"].asString("");

    if (const auto* zones = root.find("noGoZones"))
    {
        if (!zones->isArray())
        {
            result.errors.push_back("'noGoZones' must be an array");
        }
        else
        {
            for (const auto& zone : zones->asArray())
            {
                const auto center = readPoint(zone["center"]);
                const float radius = zone["radius"].asFloat(0.0f);

                if (!center || !std::isfinite(center->x) || !std::isfinite(center->y) || !std::isfinite(center->z) || !std::isfinite(radius) || radius <= 0.0f)
                {
                    result.errors.push_back("noGoZones: expected finite center [x, y, z] and positive radius");
                }
                else
                {
                    p.noGoZones.push_back({*center, radius});
                }
            }
        }
    }

    p.routeMode = parseRouteMode(root["routeMode"].asString("loop"));

    if (const Json* o = root.find("options"))
    {
        // "loot", "skin" and "gather" in older profiles are left alone: General
        // Settings decides those now.
        p.options.attackElites = (*o)["attackElites"].asBool(p.options.attackElites);
        p.options.pullDistance = static_cast<float>((*o)["pullDistance"].asDouble(p.options.pullDistance));
        p.options.gatherDistance = static_cast<float>((*o)["gatherDistance"].asDouble(p.options.gatherDistance));
        p.options.stopAtLevel = std::max(0, static_cast<int>((*o)["stopAtLevel"].asInt(p.options.stopAtLevel)));
        p.options.minPullLevel = std::max(0, static_cast<int>((*o)["minPullLevel"].asInt(p.options.minPullLevel)));
        p.options.maxPullLevel = std::max(0, static_cast<int>((*o)["maxPullLevel"].asInt(p.options.maxPullLevel)));
        p.options.fish = (*o)["fish"].asBool(p.options.fish);
        p.options.lureMinutes = std::max(0, static_cast<int>((*o)["lureMinutes"].asInt(p.options.lureMinutes)));
    }

    for (const auto& id : root["zoneIds"].asArray())
    {
        if (id.isNumber() && id.asInt(0) > 0 && std::find(p.zoneIds.begin(), p.zoneIds.end(), static_cast<std::uint32_t>(id.asInt(0))) == p.zoneIds.end())
        {
            p.zoneIds.push_back(static_cast<std::uint32_t>(id.asInt(0)));
        }
    }

    if (const Json* learned = root.find("learned"))
    {
        p.learned = readLearned(*learned);
    }

    if (const Json* zones = root.find("freeroamObstacles"))
    {
        p.learned.avoidZones = readFreeroamObstacles(*zones);
    }

    for (const auto& s : root["fishingSpots"].asArray())
    {
        FishingSpot spot;
        spot.name = s["name"].asString();
        const Json& pos = s["position"];
        spot.position = {pos[0].asFloat(), pos[1].asFloat(), pos[2].asFloat()};
        spot.heading = s["heading"].asFloat();
        spot.safeRadius = s["safeRadius"].asFloat(15.0f);
        spot.tags = readStrings(s["tags"]);
        p.fishingSpots.push_back(std::move(spot));
    }

    result.profile = std::move(p);
    return result;
}

ProfileLoadResult loadProfile(const std::filesystem::path& path)
{
    auto text = readTextFile(path);

    if (!text)
    {
        ProfileLoadResult r;
        r.errors.push_back(std::format("cannot read {}", path.string()));
        return r;
    }

    return parseProfile(*text);
}

Json profileToJson(const GliderProfile& p)
{
    Json j = Json::object();
    j["name"] = p.name;
    Json level = Json::object();
    level["min"] = p.minLevel;
    level["max"] = p.maxLevel;
    j["level"] = std::move(level);
    j["targets"] = stringsToJson(p.targets);

    if (!p.exclusions.empty())
    {
        j["exclusions"] = stringsToJson(p.exclusions);
    }

    j["routeMode"] = std::string(toString(p.routeMode));
    j["waypoints"] = pointsToJson(p.waypoints);
    j["noGoZones"] = freeroamObstaclesToJson(p.noGoZones);

    if (!p.vendorName.empty())
    {
        j["vendorName"] = p.vendorName;
    }

    Json o = Json::object();
    o["attackElites"] = p.options.attackElites;
    o["pullDistance"] = round2(p.options.pullDistance);
    o["gatherDistance"] = round2(p.options.gatherDistance);
    o["stopAtLevel"] = static_cast<std::int64_t>(p.options.stopAtLevel);
    o["minPullLevel"] = static_cast<std::int64_t>(p.options.minPullLevel);
    o["maxPullLevel"] = static_cast<std::int64_t>(p.options.maxPullLevel);

    if (p.options.fish)
    {
        o["fish"] = true;
    }

    if (p.options.lureMinutes > 0)
    {
        o["lureMinutes"] = static_cast<std::int64_t>(p.options.lureMinutes);
    }

    j["options"] = std::move(o);

    if (!p.zoneIds.empty())
    {
        Json ids = Json::array();

        for (const std::uint32_t id : p.zoneIds)
        {
            ids.push_back(Json(static_cast<std::int64_t>(id)));
        }

        j["zoneIds"] = std::move(ids);
    }

    if (!p.learned.maneuvers.empty() || !p.learned.detours.empty())
    {
        j["learned"] = learnedToJson(p.learned);
    }

    if (!p.learned.avoidZones.empty())
    {
        j["freeroamObstacles"] = freeroamObstaclesToJson(p.learned.avoidZones);
    }

    if (!p.fishingSpots.empty())
    {
        Json spots = Json::array();

        for (const auto& s : p.fishingSpots)
        {
            Json js = Json::object();
            js["name"] = s.name;
            js["position"] = Json(Json::Array{round2(s.position.x), round2(s.position.y), round2(s.position.z)});
            js["heading"] = round2(s.heading);
            js["safeRadius"] = round2(s.safeRadius);
            js["tags"] = stringsToJson(s.tags);
            spots.push_back(std::move(js));
        }

        j["fishingSpots"] = std::move(spots);
    }

    return j;
}

bool saveProfile(const GliderProfile& profile, const std::filesystem::path& path)
{
    return writeTextFile(path, profileToJson(profile).dump(2) + "\n");
}

bool saveLearnedObstacles(const std::filesystem::path& path, const LearnedObstacles& learned, bool backup, std::string* error)
{
    const auto fail = [&](std::string message)
    {
        if (error)
        {
            *error = std::move(message);
        }

        return false;
    };
    auto loaded = loadProfile(path);

    if (!loaded.ok())
    {
        return fail(loaded.errors.empty() ? std::format("cannot read {}", path.string()) : loaded.errors.front());
    }

    if (backup)
    {
        std::error_code ec;
        std::filesystem::copy_file(path, path.string() + ".bak", std::filesystem::copy_options::overwrite_existing, ec);

        if (ec)
        {
            return fail(std::format("cannot back up {}: {}", path.string(), ec.message()));
        }
    }

    loaded.profile->learned = learned;

    if (!saveProfile(*loaded.profile, path))
    {
        return fail(std::format("cannot write {}", path.string()));
    }

    return true;
}

bool saveProfileZones(const std::filesystem::path& path, const std::vector<std::uint32_t>& zoneIds, std::string* error)
{
    auto loaded = loadProfile(path);

    if (!loaded.ok())
    {
        if (error)
        {
            *error = loaded.errors.empty() ? std::format("cannot read {}", path.string()) : loaded.errors.front();
        }

        return false;
    }

    loaded.profile->zoneIds = zoneIds;

    if (!saveProfile(*loaded.profile, path))
    {
        if (error)
        {
            *error = std::format("cannot write {}", path.string());
        }

        return false;
    }

    return true;
}

ProfileValidation validateProfile(const GliderProfile& p)
{
    ProfileValidation v;

    if (p.name.empty())
    {
        v.warnings.push_back("profile has no name");
    }

    // A fishing profile stays at its spots (or where the character stands).
    if (p.options.fish)
    {
        if (p.waypoints.size() == 1)
        {
            v.errors.push_back("profile needs at least 2 waypoints, or none for fishing");
        }
    }
    else if (p.waypoints.empty())
    {
        v.errors.push_back("profile has no waypoints");
    }
    else if (p.waypoints.size() < 2)
    {
        v.errors.push_back("profile needs at least 2 waypoints");
    }

    if (p.minLevel > p.maxLevel)
    {
        v.errors.push_back(std::format("level.min ({}) is greater than level.max ({})", p.minLevel, p.maxLevel));
    }

    // No ghostWaypoints is normal: corpse runs find their own way.

    for (std::size_t i = 1; i < p.waypoints.size(); ++i)
    {
        const float d = distance(p.waypoints[i - 1], p.waypoints[i]);

        // Far apart is fine: the route outlines an area, and the way between
        // two points is planned.
        if (d < 0.5f)
        {
            v.warnings.push_back(std::format("waypoints {} and {} are duplicates", i - 1, i));
        }
    }

    return v;
}

} // namespace og
