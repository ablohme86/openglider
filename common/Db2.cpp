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
 *  File: Db2.cpp
 *
 *  Desc:
 *  Implementation of Db2 (shared utilities: logging, configuration,
 *  and helpers).
 *
 *
 */
#include "og/common/Db2.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string_view>
#include <unordered_map>

namespace og
{

namespace
{

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes)
    {
    }

    bool has(std::size_t at, std::size_t size) const
    {
        return at <= bytes_.size() && size <= bytes_.size() - at;
    }

    template <typename T>
    T get(std::size_t at) const
    {
        T value{};
        std::memcpy(&value, bytes_.data() + at, sizeof(T));
        return value;
    }

    // `count` bits from `bit` on, in the record at `at` (little-endian, up to 32).
    std::uint32_t bits(std::size_t at, std::uint32_t bit, std::uint32_t count) const
    {
        std::uint64_t value = 0;
        const std::size_t first = at + bit / 8;
        const std::size_t bytes = (bit % 8 + count + 7) / 8;

        for (std::size_t i = 0; i < bytes && i < 8 && first + i < bytes_.size(); ++i)
        {
            value |= static_cast<std::uint64_t>(bytes_[first + i]) << (8 * i);
        }

        value >>= bit % 8;
        return count >= 32 ? static_cast<std::uint32_t>(value) : static_cast<std::uint32_t>(value & ((std::uint64_t{1} << count) - 1));
    }

private:
    std::span<const std::uint8_t> bytes_;
};

struct Section
{
    std::uint64_t tactKey = 0;
    std::uint32_t fileOffset = 0;
    std::uint32_t recordCount = 0;
    std::uint32_t stringTableSize = 0;
    std::uint32_t offsetRecordsEnd = 0;
    std::uint32_t idListSize = 0;
    std::uint32_t relationshipSize = 0;
    std::uint32_t offsetMapCount = 0;
    std::uint32_t copyTableCount = 0;
};

struct FieldInfo
{
    std::uint16_t offsetBits = 0;
    std::uint16_t sizeBits = 0;
    std::uint32_t additionalSize = 0;
    std::uint32_t storage = 0;
    std::uint32_t value1 = 0; // bit-packed: offset; common: the default
    std::uint32_t value2 = 0; // bit-packed and pallets: the size in bits
    std::uint32_t value3 = 0; // a pallet array: its length
};

enum Storage : std::uint32_t
{
    None = 0,
    Bitpacked = 1,
    Common = 2,
    Pallet = 3,
    PalletArray = 4,
    BitpackedSigned = 5
};

} // namespace

std::optional<Db2Table> Db2Table::parse(std::span<const std::uint8_t> bytes, std::string& error)
{
    const Reader r(bytes);

    if (!r.has(0, 4))
    {
        error = "not a DB2 file";
        return std::nullopt;
    }

    const std::string_view magic(reinterpret_cast<const char*>(bytes.data()), 4);

    // WDC5 puts a version and a 128-byte schema name before the header WDC3 and WDC4 start with.
    std::size_t at = 0;

    if (magic == "WDC5")
    {
        at = 4 + 4 + 128;
    }
    else if (magic == "WDC3" || magic == "WDC4")
    {
        at = 4;
    }
    else
    {
        error = "unsupported DB2 format " + std::string(magic);
        return std::nullopt;
    }

    constexpr std::size_t kHeader = 9 * 4 + 2 * 2 + 7 * 4;

    if (!r.has(at, kHeader))
    {
        error = "truncated DB2 header";
        return std::nullopt;
    }

    Db2Table table;
    const auto recordSize = r.get<std::uint32_t>(at + 8);
    const auto fieldCount = r.get<std::uint32_t>(at + 4);
    table.tableHash_ = r.get<std::uint32_t>(at + 16);
    table.layoutHash_ = r.get<std::uint32_t>(at + 20);
    const auto flags = r.get<std::uint16_t>(at + 36);
    const auto idIndex = r.get<std::uint16_t>(at + 38);
    const auto commonSize = r.get<std::uint32_t>(at + 56);
    const auto palletSize = r.get<std::uint32_t>(at + 60);
    const auto sectionCount = r.get<std::uint32_t>(at + 64);
    at += kHeader;

    if ((flags & 0x01) != 0)
    {
        error = "sparse DB2 tables are not read";
        return std::nullopt;
    }

    std::vector<Section> sections(sectionCount);

    for (auto& s : sections)
    {
        if (!r.has(at, 40))
        {
            error = "truncated DB2 sections";
            return std::nullopt;
        }

        s = {r.get<std::uint64_t>(at), r.get<std::uint32_t>(at + 8), r.get<std::uint32_t>(at + 12), r.get<std::uint32_t>(at + 16), r.get<std::uint32_t>(at + 20), r.get<std::uint32_t>(at + 24), r.get<std::uint32_t>(at + 28), r.get<std::uint32_t>(at + 32), r.get<std::uint32_t>(at + 36)};
        at += 40;
    }

    at += 4 * fieldCount; // the field structure: what the storage info says again
    std::vector<FieldInfo> fields(fieldCount);

    for (auto& f : fields)
    {
        if (!r.has(at, 24))
        {
            error = "truncated DB2 field info";
            return std::nullopt;
        }

        f = {r.get<std::uint16_t>(at), r.get<std::uint16_t>(at + 2), r.get<std::uint32_t>(at + 4), r.get<std::uint32_t>(at + 8), r.get<std::uint32_t>(at + 12), r.get<std::uint32_t>(at + 16), r.get<std::uint32_t>(at + 20)};
        at += 24;
    }

    // Each pallet field's values, and each common field's values by record id.
    std::vector<std::size_t> palletAt(fieldCount, 0);
    std::vector<std::unordered_map<std::uint32_t, std::uint32_t>> common(fieldCount);
    std::size_t pallet = at;
    std::size_t commonAt = at + palletSize;

    if (!r.has(at, palletSize) || !r.has(commonAt, commonSize))
    {
        error = "truncated DB2 pallet or common data";
        return std::nullopt;
    }

    for (std::uint32_t i = 0; i < fieldCount; ++i)
    {
        const FieldInfo& f = fields[i];

        if (f.storage == Pallet || f.storage == PalletArray)
        {
            palletAt[i] = pallet;
            pallet += f.additionalSize;
        }
        else if (f.storage == Common)
        {
            for (std::uint32_t k = 0; k + 1 < f.additionalSize / 4; k += 2)
            {
                common[i][r.get<std::uint32_t>(commonAt + 4 * k)] = r.get<std::uint32_t>(commonAt + 4 * k + 4);
            }

            commonAt += f.additionalSize;
        }
    }

    for (const Section& s : sections)
    {
        // An encrypted section the file does not carry.
        if (s.fileOffset >= bytes.size() || !r.has(s.fileOffset, std::size_t{s.recordCount} * recordSize))
        {
            continue;
        }

        const std::size_t records = s.fileOffset;
        std::size_t p = records + std::size_t{s.recordCount} * recordSize + s.stringTableSize;
        const std::size_t ids = p;
        p += s.idListSize;
        p += std::size_t{s.copyTableCount} * 8;
        p += std::size_t{s.offsetMapCount} * 6;
        std::map<std::uint32_t, std::uint32_t> relation; // record index -> foreign id

        if (s.relationshipSize >= 12 && r.has(p, s.relationshipSize))
        {
            const auto entries = r.get<std::uint32_t>(p);

            for (std::uint32_t k = 0; k < entries && r.has(p + 12 + 8 * k, 8); ++k)
            {
                relation[r.get<std::uint32_t>(p + 12 + 8 * k + 4)] = r.get<std::uint32_t>(p + 12 + 8 * k);
            }
        }

        for (std::uint32_t i = 0; i < s.recordCount; ++i)
        {
            const std::size_t record = records + std::size_t{i} * recordSize;
            Row row;

            for (std::uint32_t k = 0; k < fieldCount; ++k)
            {
                const FieldInfo& f = fields[k];

                switch (f.storage)
                {
                case None:
                    // An inline array is wider than one value: 32 bits each.
                    if (f.sizeBits <= 32)
                    {
                        row.fields.push_back(r.bits(record, f.offsetBits, f.sizeBits));
                    }
                    else
                    {
                        for (std::uint32_t bit = 0; bit < f.sizeBits; bit += 32)
                        {
                            row.fields.push_back(r.bits(record, f.offsetBits + bit, 32));
                        }
                    }

                    break;
                case Bitpacked:
                case BitpackedSigned:
                {
                    std::uint32_t value = r.bits(record, f.offsetBits, f.value2);

                    // Signed: the top bit of the packed value spread up.
                    if (f.storage == BitpackedSigned && f.value2 > 0 && f.value2 < 32 && (value >> (f.value2 - 1)) & 1)
                    {
                        value |= ~((std::uint32_t{1} << f.value2) - 1);
                    }

                    row.fields.push_back(value);
                    break;
                }
                case Common:
                    row.fields.push_back(0); // filled in below, by the record's id
                    break;
                case Pallet:
                {
                    const std::uint32_t index = r.bits(record, f.offsetBits, f.value2);
                    row.fields.push_back(r.has(palletAt[k] + 4 * std::size_t{index}, 4) ? r.get<std::uint32_t>(palletAt[k] + 4 * std::size_t{index}) : 0);
                    break;
                }
                case PalletArray:
                {
                    const std::uint32_t index = r.bits(record, f.offsetBits, f.value2);

                    for (std::uint32_t e = 0; e < f.value3; ++e)
                    {
                        const std::size_t value = palletAt[k] + 4 * (std::size_t{index} * f.value3 + e);
                        row.fields.push_back(r.has(value, 4) ? r.get<std::uint32_t>(value) : 0);
                    }

                    break;
                }
                default:
                    row.fields.push_back(0);
                    break;
                }
            }

            row.id = s.idListSize >= 4 * (std::size_t{i} + 1) ? r.get<std::uint32_t>(ids + 4 * std::size_t{i}) : idIndex < row.fields.size() ? row.fields[idIndex] : 0;

            // Common fields after the id is known: the field's value for it, else its default.
            for (std::uint32_t k = 0, column = 0; k < fieldCount; ++k)
            {
                const FieldInfo& f = fields[k];

                if (f.storage == Common)
                {
                    const auto found = common[k].find(row.id);
                    row.fields[column] = found == common[k].end() ? f.value1 : found->second;
                }

                column += f.storage == PalletArray ? f.value3 : f.storage == None && f.sizeBits > 32 ? (f.sizeBits + 31) / 32 : 1;
            }

            if (const auto found = relation.find(i); found != relation.end())
            {
                row.relation = found->second;
            }

            table.rows_.push_back(std::move(row));
        }
    }

    return table;
}

} // namespace og
