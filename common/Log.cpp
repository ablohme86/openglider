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
 *  File: Log.cpp
 *
 *  Desc:
 *  Implementation of Log (common utility functions, math, logging, and
 *  data structures).
 *
 *
 */
#include "og/common/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace og
{

std::string_view toString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "Trace";
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Information:
        return "Information";
    case LogLevel::Warning:
        return "Warning";
    case LogLevel::Error:
        return "Error";
    case LogLevel::Critical:
        return "Critical";
    case LogLevel::Off:
        return "Off";
    }

    return "?";
}

std::string_view shortName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "TRC";
    case LogLevel::Debug:
        return "DBG";
    case LogLevel::Information:
        return "INF";
    case LogLevel::Warning:
        return "WRN";
    case LogLevel::Error:
        return "ERR";
    case LogLevel::Critical:
        return "CRT";
    case LogLevel::Off:
        return "OFF";
    }

    return "???";
}

std::optional<LogLevel> parseLogLevel(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "trace")
    {
        return LogLevel::Trace;
    }

    if (lower == "debug")
    {
        return LogLevel::Debug;
    }

    if (lower == "info" || lower == "information")
    {
        return LogLevel::Information;
    }

    if (lower == "warn" || lower == "warning")
    {
        return LogLevel::Warning;
    }

    if (lower == "error")
    {
        return LogLevel::Error;
    }

    if (lower == "critical")
    {
        return LogLevel::Critical;
    }

    if (lower == "off" || lower == "none")
    {
        return LogLevel::Off;
    }

    return std::nullopt;
}

std::string formatLogRecord(const LogRecord& r)
{
    if (r.instance.empty())
    {
        return std::format("{} {} {:<7} {}", r.time, shortName(r.level), r.category, r.message);
    }

    return std::format("{} {} [{}] {:<7} {}", r.time, shortName(r.level), r.instance, r.category, r.message);
}

void ConsoleLogSink::write(const LogRecord& record)
{
    const std::string line = formatLogRecord(record);
    const char* colorCode = nullptr;

    if (color_)
    {
        switch (record.level)
        {
        case LogLevel::Trace:
            colorCode = "\x1b[90m";
            break;
        case LogLevel::Debug:
            colorCode = "\x1b[37m";
            break;
        case LogLevel::Warning:
            colorCode = "\x1b[33m";
            break;
        case LogLevel::Error:
        case LogLevel::Critical:
            colorCode = "\x1b[31m";
            break;
        default:
            break;
        }
    }

    if (colorCode)
    {
        std::fprintf(stderr, "%s%s\x1b[0m\n", colorCode, line.c_str());
    }
    else
    {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

FileLogSink::FileLogSink(const std::string& path) : out_(path, std::ios::app)
{
}

void FileLogSink::write(const LogRecord& record)
{
    if (!out_)
    {
        return;
    }

    out_ << formatLogRecord(record) << '\n';
    out_.flush();
}

CappedLogFile::CappedLogFile(std::string path, std::size_t maxLines) : path_(std::move(path)), maxLines_(maxLines), out_(path_, std::ios::trunc)
{
}

void CappedLogFile::writeLine(std::string_view line)
{
    if (!out_)
    {
        return;
    }

    if (lines_ >= maxLines_)
    {
        out_.close();
        out_.open(path_, std::ios::trunc);
        lines_ = 0;

        if (!out_)
        {
            return;
        }
    }

    out_ << line << '\n';
    out_.flush();
    ++lines_;
}

void RingBufferLogSink::write(const LogRecord& record)
{
    std::lock_guard lock(mutex_);
    records_.push_back(record);

    while (records_.size() > capacity_)
    {
        records_.pop_front();
    }
}

std::vector<LogRecord> RingBufferLogSink::snapshot() const
{
    std::lock_guard lock(mutex_);
    return {records_.begin(), records_.end()};
}

Logger::Logger(std::string instance) : instance_(std::move(instance))
{
}

void Logger::addSink(std::shared_ptr<ILogSink> sink)
{
    std::lock_guard lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::clearSinks()
{
    std::lock_guard lock(mutex_);
    sinks_.clear();
}

void Logger::setTimeSource(TimeSource source)
{
    std::lock_guard lock(mutex_);
    timeSource_ = std::move(source);
}

std::string Logger::wallClockTime()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:02}:{:02}:{:02}.{:03}", tm.tm_hour, tm.tm_min, tm.tm_sec, ms.count());
}

void Logger::log(LogLevel level, std::string_view category, std::string message)
{
    if (!enabled(level))
    {
        return;
    }

    std::lock_guard lock(mutex_);
    LogRecord record;
    record.time = timeSource_ ? timeSource_() : wallClockTime();
    record.level = level;
    record.category = std::string(category);
    record.message = std::move(message);
    record.instance = instance_;

    for (auto& sink : sinks_)
    {
        sink->write(record);
    }
}

} // namespace og
