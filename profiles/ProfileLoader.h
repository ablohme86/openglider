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
 *  File: ProfileLoader.h
 *
 *  Desc:
 *  Declarations and interface for ProfileLoader (glider profile
 *  parsing, validation, and recording).
 *
 *
 */
#pragma once

#include "og/common/Json.h"
#include "og/profiles/GliderProfile.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace og
{

struct ProfileLoadResult
{
    std::optional<GliderProfile> profile;
    std::vector<std::string> errors;
    bool ok() const
    {
        return profile.has_value() && errors.empty();
    }
};

ProfileLoadResult parseProfile(const std::string& jsonText);
ProfileLoadResult loadProfile(const std::filesystem::path& path);

Json profileToJson(const GliderProfile& profile);
bool saveProfile(const GliderProfile& profile, const std::filesystem::path& path);
// Writes `learned` into the profile file at `path`, keeping everything else as
// it is on disk rather than as some copy in memory has it. With `backup` the
// file is first copied to <path>.bak.
bool saveLearnedObstacles(const std::filesystem::path& path, const LearnedObstacles& learned, bool backup, std::string* error = nullptr);

// Writes the zone ids into the profile file at `path`, keeping everything else
// as it is on disk.
bool saveProfileZones(const std::filesystem::path& path, const std::vector<std::uint32_t>& zoneIds, std::string* error = nullptr);

struct ProfileValidation
{
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool ok() const
    {
        return errors.empty();
    }
};

ProfileValidation validateProfile(const GliderProfile& profile);

} // namespace og
