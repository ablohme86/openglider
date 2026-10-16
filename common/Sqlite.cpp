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
 *  File: Sqlite.cpp
 *
 *  Desc:
 *  Implementation of SqliteDb and SqliteStmt wrappers.
 *
 * ============================================================== *
 */
#include "og/common/Sqlite.h"

#include <sqlite3.h>

#include <utility>

namespace og
{

namespace
{
// SQLite takes file names as UTF-8; path::string() is the ANSI code page on Windows.
std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
} // namespace

SqliteDb::~SqliteDb()
{
    close();
}

SqliteDb::SqliteDb(SqliteDb&& other) noexcept : db_(std::exchange(other.db_, nullptr))
{
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept
{
    if (this != &other)
    {
        close();
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

bool SqliteDb::openReadOnly(const std::filesystem::path& path, std::string* error)
{
    close();
    const int rc = sqlite3_open_v2(utf8(path).c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK)
    {
        if (error)
        {
            *error = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        }
        close();
        return false;
    }
    return true;
}

bool SqliteDb::openReadWrite(const std::filesystem::path& path, bool create, std::string* error)
{
    close();
    int flags = SQLITE_OPEN_READWRITE;
    if (create)
    {
        flags |= SQLITE_OPEN_CREATE;
    }
    const int rc = sqlite3_open_v2(utf8(path).c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        if (error)
        {
            *error = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        }
        close();
        return false;
    }
    return true;
}

void SqliteDb::close()
{
    if (db_)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SqliteDb::execute(const std::string& sql, std::string* error)
{
    if (!db_)
    {
        if (error)
        {
            *error = "database not open";
        }
        return false;
    }
    char* errmsg = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK)
    {
        if (error)
        {
            *error = errmsg ? errmsg : "sqlite3_exec failed";
        }
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

SqliteStmt SqliteDb::prepare(const std::string& sql)
{
    if (!db_)
    {
        throw SqliteError("database not open");
    }
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        const std::string msg = sqlite3_errmsg(db_);
        throw SqliteError("failed to prepare statement: " + msg);
    }
    return SqliteStmt(stmt);
}

SqliteStmt::~SqliteStmt()
{
    if (stmt_)
    {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

SqliteStmt::SqliteStmt(SqliteStmt&& other) noexcept : stmt_(std::exchange(other.stmt_, nullptr))
{
}

SqliteStmt& SqliteStmt::operator=(SqliteStmt&& other) noexcept
{
    if (this != &other)
    {
        if (stmt_)
        {
            sqlite3_finalize(stmt_);
        }
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

void SqliteStmt::bindInt(int index, int val)
{
    if (!stmt_)
    {
        throw SqliteError("statement not initialized");
    }
    sqlite3_bind_int(stmt_, index, val);
}

void SqliteStmt::bindUint32(int index, std::uint32_t val)
{
    if (!stmt_)
    {
        throw SqliteError("statement not initialized");
    }
    sqlite3_bind_int64(stmt_, index, static_cast<std::int64_t>(val));
}

void SqliteStmt::bindInt64(int index, std::int64_t val)
{
    if (!stmt_)
    {
        throw SqliteError("statement not initialized");
    }
    sqlite3_bind_int64(stmt_, index, val);
}

void SqliteStmt::bindDouble(int index, double val)
{
    if (!stmt_)
    {
        throw SqliteError("statement not initialized");
    }
    sqlite3_bind_double(stmt_, index, val);
}

void SqliteStmt::bindText(int index, std::string_view text)
{
    if (!stmt_)
    {
        throw SqliteError("statement not initialized");
    }
    sqlite3_bind_text(stmt_, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

bool SqliteStmt::step()
{
    if (!stmt_)
    {
        throw SqliteError("statement not initialized");
    }
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)
    {
        return true;
    }
    if (rc == SQLITE_DONE)
    {
        return false;
    }
    throw SqliteError("sqlite3_step failed: " + std::string(sqlite3_errstr(rc)));
}

void SqliteStmt::reset()
{
    if (stmt_)
    {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
}

int SqliteStmt::columnInt(int col) const
{
    return sqlite3_column_int(stmt_, col);
}

std::uint32_t SqliteStmt::columnUint32(int col) const
{
    return static_cast<std::uint32_t>(sqlite3_column_int64(stmt_, col));
}

std::int64_t SqliteStmt::columnInt64(int col) const
{
    return sqlite3_column_int64(stmt_, col);
}

double SqliteStmt::columnDouble(int col) const
{
    return sqlite3_column_double(stmt_, col);
}

std::string_view SqliteStmt::columnText(int col) const
{
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
    const int bytes = sqlite3_column_bytes(stmt_, col);
    return text ? std::string_view(text, static_cast<std::size_t>(bytes)) : std::string_view{};
}

std::string SqliteStmt::columnString(int col) const
{
    const auto sv = columnText(col);
    return std::string(sv.data(), sv.size());
}

bool SqliteStmt::isNull(int col) const
{
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

} // namespace og
