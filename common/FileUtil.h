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
 *  File: FileUtil.h
 *
 *  Desc:
 *  Declarations and interface for FileUtil (common utility functions,
 *  math, logging, and data structures).
 *
 *
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace og
{

std::optional<std::string> readTextFile(const std::filesystem::path& path);
bool writeTextFile(const std::filesystem::path& path, const std::string& content);

// The application OpenGlider runs in, as shown to the user, and in lower case
// as the name of its directories: what the application said
// (setApplicationName), else OG_APP_NAME as built ("OpenGlider" unless the
// parent project sets it). A shared engine built on its own cannot know it.
std::string_view applicationName();
std::string applicationDirectoryName();
// Before anything reads a file: the application's name, and its development
// data directory (looked in last, resolveDataFile).
void setApplicationName(std::string name);
void setDataDirectory(std::filesystem::path directory);

// ~/.config/<app> on Linux, %APPDATA%/<App> on Windows.
std::filesystem::path defaultConfigDirectory();

// The directory the running executable sits in, so a package can find the data
// it ships whatever the working directory is. Empty when it cannot be
// determined, which callers must treat as "no such directory".
std::filesystem::path executableDirectory();

// As given, user config, packaged data beside the executable, then the
// configured development data directory. Shared by apps and data readers.
std::filesystem::path resolveDataFile(const std::string& path);

// The client's language from its WTF/Config.wtf: SET textLocale "deDE".
// enUS where it says none.
std::string textLocaleOf(std::string_view configWtf);

} // namespace og
