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
 *  File: Log.h
 *
 *  Desc:
 *  Declarations and interface for Log (common utility functions, math,
 *  logging, and data structures).
 *
 *
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

enum class LogLevel
{
    Trace,
    Debug,
    Information,
    Warning,
    Error,
    Critical,
    Off
};

std::string_view toString(LogLevel level);
std::string_view shortName(LogLevel level);
std::optional<LogLevel> parseLogLevel(std::string_view text);

// Well-known log categories. Categories are free-form strings; these are the
// ones used by the engine so output looks like:
//   12:32:01.224 INF STATE   Grinding -> Pull
namespace logcat
{
inline constexpr std::string_view Engine = "ENGINE";
inline constexpr std::string_view State = "STATE";
inline constexpr std::string_view Target = "TARGET";
inline constexpr std::string_view Move = "MOVE";
inline constexpr std::string_view Action = "ACTION";
inline constexpr std::string_view Combat = "COMBAT";
inline constexpr std::string_view Loot = "LOOT";
inline constexpr std::string_view Fish = "FISH";
inline constexpr std::string_view Rest = "REST";
inline constexpr std::string_view Death = "DEATH";
inline constexpr std::string_view Stuck = "STUCK";
inline constexpr std::string_view Objective = "OBJECT";
inline constexpr std::string_view Profile = "PROFILE";
inline constexpr std::string_view Script = "SCRIPT";
inline constexpr std::string_view Input = "INPUT";
inline constexpr std::string_view Game = "GAME";
inline constexpr std::string_view Chat = "CHAT"; // in-game chat lines, message "[Say] Name: text"
inline constexpr std::string_view Sim = "SIM";
inline constexpr std::string_view Replay = "REPLAY";
} // namespace logcat

struct LogRecord
{
    std::string time;
    LogLevel level = LogLevel::Information;
    std::string category;
    std::string message;
    std::string instance;
};

std::string formatLogRecord(const LogRecord& record);

class ILogSink
{
public:
    virtual ~ILogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

class ConsoleLogSink final : public ILogSink
{
public:
    explicit ConsoleLogSink(bool color = true) : color_(color)
    {
    }

    void write(const LogRecord& record) override;

private:
    bool color_;
};

class FileLogSink final : public ILogSink
{
public:
    explicit FileLogSink(const std::string& path);
    bool isOpen() const
    {
        return out_.is_open();
    }

    void write(const LogRecord& record) override;

private:
    std::ofstream out_;
};

// A text file of at most `maxLines` lines: started empty when opened, and
// emptied and started over when it is full. The GUI's Event.log.
class CappedLogFile
{
public:
    CappedLogFile(std::string path, std::size_t maxLines);
    bool isOpen() const
    {
        return out_.is_open();
    }

    void writeLine(std::string_view line);

private:
    std::string path_;
    std::size_t maxLines_;
    std::size_t lines_ = 0;
    std::ofstream out_;
};

class CallbackLogSink final : public ILogSink
{
public:
    explicit CallbackLogSink(std::function<void(const LogRecord&)> fn) : fn_(std::move(fn))
    {
    }

    void write(const LogRecord& record) override
    {
        fn_(record);
    }

private:
    std::function<void(const LogRecord&)> fn_;
};

// Keeps the last N records in memory (UI log view, daemon /logs endpoint).
class RingBufferLogSink final : public ILogSink
{
public:
    explicit RingBufferLogSink(std::size_t capacity = 2000) : capacity_(capacity)
    {
    }

    void write(const LogRecord& record) override;
    std::vector<LogRecord> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<LogRecord> records_;
};

// Thread-safe logger. One Logger per bot instance so that multi-instance
// output can be separated.
class Logger
{
public:
    using TimeSource = std::function<std::string()>;

    explicit Logger(std::string instance = {});

    void setLevel(LogLevel level)
    {
        level_ = level;
    }

    LogLevel level() const
    {
        return level_;
    }

    bool enabled(LogLevel level) const
    {
        return level != LogLevel::Off && level >= level_;
    }

    void addSink(std::shared_ptr<ILogSink> sink);
    void clearSinks();

    // Overrides how timestamps are produced. The default is wall clock time of
    // day; simulation and replay use their own timeline.
    void setTimeSource(TimeSource source);

    const std::string& instance() const
    {
        return instance_;
    }

    void log(LogLevel level, std::string_view category, std::string message);

    template <class... Args> void trace(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
    {
        logf(LogLevel::Trace, cat, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void debug(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
    {
        logf(LogLevel::Debug, cat, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void info(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
    {
        logf(LogLevel::Information, cat, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void warn(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
    {
        logf(LogLevel::Warning, cat, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void error(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
    {
        logf(LogLevel::Error, cat, fmt, std::forward<Args>(args)...);
    }

    template <class... Args> void critical(std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
    {
        logf(LogLevel::Critical, cat, fmt, std::forward<Args>(args)...);
    }

    static std::string wallClockTime();

private:
    template <class... Args> void logf(LogLevel level, std::string_view cat, std::format_string<Args...> fmt, Args&&... args)
    {
        if (!enabled(level))
        {
            return;
        }

        log(level, cat, std::format(fmt, std::forward<Args>(args)...));
    }

    std::string instance_;
    std::atomic<LogLevel> level_{LogLevel::Information};
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ILogSink>> sinks_;
    TimeSource timeSource_;
};

} // namespace og
