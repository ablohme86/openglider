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
 *  File: Blacklist.h
 *
 *  Desc:
 *  Declarations and interface for Blacklist (common utility functions,
 *  math, logging, and data structures).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Vec3.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace og
{

// Time-limited blacklist keyed by an id (GUID, waypoint index, ...).
//
//   blacklist.add(target.guid, 2min, "Unreachable");
template <class Key> class Blacklist
{
public:
    struct Entry
    {
        TimePoint expires;
        std::string reason;
    };

    explicit Blacklist(const IClock& clock) : clock_(&clock)
    {
    }

    void add(const Key& key, Duration duration, std::string reason)
    {
        entries_[key] = Entry{clock_->now() + duration, std::move(reason)};
    }

    bool contains(const Key& key) const
    {
        auto it = entries_.find(key);
        return it != entries_.end() && it->second.expires > clock_->now();
    }

    std::optional<Entry> find(const Key& key) const
    {
        auto it = entries_.find(key);

        if (it == entries_.end() || it->second.expires <= clock_->now())
        {
            return std::nullopt;
        }

        return it->second;
    }

    void remove(const Key& key)
    {
        entries_.erase(key);
    }

    void clear()
    {
        entries_.clear();
    }

    void purgeExpired()
    {
        const auto now = clock_->now();
        std::erase_if(entries_, [now](const auto& kv) { return kv.second.expires <= now; });
    }

    std::size_t size() const
    {
        return entries_.size();
    }

    const std::map<Key, Entry>& entries() const
    {
        return entries_;
    }

private:
    const IClock* clock_;
    std::map<Key, Entry> entries_;
};

// Blacklisted circular areas ("too dangerous", "stuck loop here").
class AreaBlacklist
{
public:
    struct Area
    {
        Vec3 center;
        float radius = 0.0f;
        TimePoint expires;
        std::string reason;
    };

    explicit AreaBlacklist(const IClock& clock) : clock_(&clock)
    {
    }

    void add(const Vec3& center, float radius, Duration duration, std::string reason)
    {
        areas_.push_back(Area{center, radius, clock_->now() + duration, std::move(reason)});
    }

    bool contains(const Vec3& point) const
    {
        const auto now = clock_->now();

        for (const auto& a : areas_)
        {
            if (a.expires > now && distance2D(a.center, point) <= a.radius)
            {
                return true;
            }
        }

        return false;
    }

    void purgeExpired()
    {
        const auto now = clock_->now();
        std::erase_if(areas_, [now](const Area& a) { return a.expires <= now; });
    }

    void clear()
    {
        areas_.clear();
    }

    const std::vector<Area>& areas() const
    {
        return areas_;
    }

private:
    const IClock* clock_;
    std::vector<Area> areas_;
};

} // namespace og
