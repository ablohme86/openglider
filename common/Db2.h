/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-04
 *  File: Db2.h
 *
 *  Desc:
 *  Declarations and interface for Db2 (shared utilities: logging,
 *  configuration, and helpers).
 *
 *
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace og
{

// A client database table (DB2) of the 64-bit clients, as WDC3, WDC4 or WDC5:
// each record's fields as 32-bit values, unpacked whatever way the file packs
// them (bit-packed, common data, pallets). What a field means is the table's
// layout's (WoWDBDefs); the reader only knows where its bits are. Sections
// whose data the file does not hold (encrypted ones) are left out.
class Db2Table
{
public:
    struct Row
    {
        std::uint32_t id = 0;
        // One value to a field; an array field gives as many as it holds.
        std::vector<std::uint32_t> fields;
        // The foreign key a non-inline relation column gives the record
        // (SpellPower's SpellID), where the table has one.
        std::optional<std::uint32_t> relation;
    };

    static std::optional<Db2Table> parse(std::span<const std::uint8_t> bytes, std::string& error);

    std::uint32_t tableHash() const
    {
        return tableHash_;
    }

    std::uint32_t layoutHash() const
    {
        return layoutHash_;
    }

    const std::vector<Row>& rows() const
    {
        return rows_;
    }

private:
    std::uint32_t tableHash_ = 0;
    std::uint32_t layoutHash_ = 0;
    std::vector<Row> rows_;
};

} // namespace og
