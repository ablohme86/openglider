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
 *  File: FileUtil.cpp
 *
 *  Desc:
 *  Implementation of FileUtil (common utility functions, math, logging,
 *  and data structures).
 *
 *
 */
#include "og/common/FileUtil.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace og
{

namespace
{

std::string& appName()
{
    static std::string name = OG_APP_NAME;
    return name;
}

std::filesystem::path& dataDirectory()
{
    static std::filesystem::path directory = OG_DATA_DIR;
    return directory;
}

} // namespace

std::filesystem::path resolveDataFile(const std::string& path)
{
    const std::filesystem::path p(path);

    if (p.is_absolute() || std::filesystem::exists(p))
    {
        return p;
    }

    if (auto cfg = defaultConfigDirectory() / p; std::filesystem::exists(cfg))
    {
        return cfg;
    }

    // Beside the binary, so a release package - an AppImage especially, which
    // mounts somewhere different every run - finds the data it ships whatever
    // the working directory is.
    if (const auto exeDir = executableDirectory(); !exeDir.empty())
    {
        for (const auto& relative : {std::filesystem::path("data"), std::filesystem::path("..") / "data", std::filesystem::path("..") / "share" / applicationDirectoryName() / "data"})
        {
            if (auto beside = exeDir / relative / p; std::filesystem::exists(beside))
            {
                return beside;
            }
        }
    }

    if (auto data = dataDirectory() / p; std::filesystem::exists(data))
    {
        return data;
    }

    return p;
}

std::optional<std::string> readTextFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);

    if (!in)
    {
        return std::nullopt;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeTextFile(const std::filesystem::path& path, const std::string& content)
{
    std::error_code ec;

    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);

    if (!out)
    {
        return false;
    }

    out << content;
    return static_cast<bool>(out);
}

std::string_view applicationName()
{
    return appName();
}

void setApplicationName(std::string name)
{
    if (!name.empty())
    {
        appName() = std::move(name);
    }
}

void setDataDirectory(std::filesystem::path directory)
{
    if (!directory.empty())
    {
        dataDirectory() = std::move(directory);
    }
}

std::string applicationDirectoryName()
{
    std::string name(applicationName());
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

std::filesystem::path defaultConfigDirectory()
{
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA"))
    {
        return std::filesystem::path(appData) / applicationName();
    }

    return std::filesystem::path(applicationName());
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    {
        return std::filesystem::path(xdg) / applicationDirectoryName();
    }

    if (const char* home = std::getenv("HOME"))
    {
        return std::filesystem::path(home) / ".config" / applicationDirectoryName();
    }

    return std::filesystem::path("." + applicationDirectoryName());
#endif
}

std::filesystem::path executableDirectory()
{
#ifdef _WIN32
    std::wstring buffer(4096, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

    if (length == 0 || length >= buffer.size())
    {
        return {};
    }

    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#else
    // AppImages mount somewhere different on every run, so the path has to be
    // asked for rather than remembered.
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec)
    {
        return {};
    }

    return self.parent_path();
#endif
}

std::string textLocaleOf(std::string_view configWtf)
{
    // Line by line: std::regex's multiline is C++17's, and MSVC has none.
    static const std::regex line(R"re([ \t]*SET[ \t]+textLocale[ \t]+"(\w{4})".*)re", std::regex::icase);

    for (std::size_t start = 0; start < configWtf.size();)
    {
        const auto end = (std::min)(configWtf.find('\n', start), configWtf.size()); // windows.h's min macro
        // A Windows line's \r is no part of it.
        const auto last = end > start && configWtf[end - 1] == '\r' ? end - 1 : end;
        std::match_results<std::string_view::const_iterator> m;

        if (std::regex_match(configWtf.begin() + start, configWtf.begin() + last, m, line))
        {
            return m[1].str();
        }

        start = end + 1;
    }

    return "enUS";
}

} // namespace og
