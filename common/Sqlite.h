/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-06
 *  File: Sqlite.h
 *
 *  Desc:
 *  Modern RAII wrappers for SQLite database connections and
 *  prepared statements.
 *
 * ============================================================== *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace og
{

class SqliteError : public std::runtime_error
{
public:
    explicit SqliteError(const std::string& message) : std::runtime_error(message)
    {
    }
};

class SqliteStmt;

class SqliteDb
{
public:
    SqliteDb() = default;
    ~SqliteDb();

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    SqliteDb(SqliteDb&& other) noexcept;
    SqliteDb& operator=(SqliteDb&& other) noexcept;

    bool openReadOnly(const std::filesystem::path& path, std::string* error = nullptr);
    bool openReadWrite(const std::filesystem::path& path, bool create = false, std::string* error = nullptr);
    void close();

    bool isOpen() const
    {
        return db_ != nullptr;
    }

    bool execute(const std::string& sql, std::string* error = nullptr);
    SqliteStmt prepare(const std::string& sql);

    sqlite3* handle() const
    {
        return db_;
    }

private:
    sqlite3* db_ = nullptr;
};

class SqliteStmt
{
public:
    SqliteStmt() = default;
    explicit SqliteStmt(sqlite3_stmt* stmt) : stmt_(stmt)
    {
    }
    ~SqliteStmt();

    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;

    SqliteStmt(SqliteStmt&& other) noexcept;
    SqliteStmt& operator=(SqliteStmt&& other) noexcept;

    bool isValid() const
    {
        return stmt_ != nullptr;
    }

    // 1-based parameter indexing
    void bindInt(int index, int val);
    void bindUint32(int index, std::uint32_t val);
    void bindInt64(int index, std::int64_t val);
    void bindDouble(int index, double val);
    void bindText(int index, std::string_view text);

    // Step execution: returns true on row, false on done
    bool step();
    void reset();

    // 0-based column indexing
    int columnInt(int col) const;
    std::uint32_t columnUint32(int col) const;
    std::int64_t columnInt64(int col) const;
    double columnDouble(int col) const;
    std::string_view columnText(int col) const;
    std::string columnString(int col) const;
    bool isNull(int col) const;

    sqlite3_stmt* handle() const
    {
        return stmt_;
    }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace og
