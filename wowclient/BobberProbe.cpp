/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-21
 *  File: BobberProbe.cpp
 *
 *  Desc:
 *  Implementation of BobberProbe (WoW client memory reading, process
 *  inspection, and layout definitions).
 *
 *
 */
#include "og/wowclient/BobberProbe.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace og::wowclient
{

void BobberProbe::sample(Duration at, std::span<const std::uint8_t> bytes)
{
    if (!samples_.empty() && bytes.size() != samples_.front().bytes.size())
    {
        return;
    }

    samples_.push_back({at, {bytes.begin(), bytes.end()}});
}

namespace
{
template <class Samples> auto settled(const Samples& samples, Duration settle)
{
    return std::find_if(samples.begin(), samples.end(), [&](const auto& s) { return s.at >= settle; });
}

} // namespace

std::vector<std::uint32_t> BobberProbe::createdByCandidates(Duration settle) const
{
    std::vector<std::uint32_t> out;
    const auto first = settled(samples_, settle);

    if (first == samples_.end() || playerGuid_.empty())
    {
        return out;
    }

    const auto& bytes = first->bytes;

    for (std::size_t at = 0; at + playerGuid_.size() <= bytes.size(); at += 8)
    {
        if (std::memcmp(bytes.data() + at, playerGuid_.data(), playerGuid_.size()) == 0)
        {
            out.push_back(static_cast<std::uint32_t>(at));
        }
    }

    return out;
}

std::vector<BobberProbe::BiteCandidate> BobberProbe::biteCandidates(Duration settle, Duration tail, Duration before, Duration after) const
{
    std::vector<BiteCandidate> out;
    const auto first = settled(samples_, settle);

    if (first == samples_.end())
    {
        return out;
    }

    const Duration end = samples_.back().at - tail;
    const auto last = std::find_if(first, samples_.end(), [&](const Sample& s) { return s.at > end; });

    if (std::distance(first, last) < 3)
    {
        return out;
    }

    const std::size_t size = first->bytes.size();

    for (std::size_t offset = 0; offset < size; ++offset)
    {
        BiteCandidate c;
        c.offset = static_cast<std::uint32_t>(offset);
        c.before = first->bytes[offset];
        std::uint8_t current = c.before;

        for (auto s = first; s != last; ++s)
        {
            if (s->bytes[offset] == current)
            {
                continue;
            }

            current = s->bytes[offset];

            if (c.history.size() < 8)
            {
                c.history.emplace_back(s->at, current);
            }

            if (c.changes++ == 0)
            {
                c.after = current;
                c.changedAt = s->at;
            }
        }

        if (c.changes == 0)
        {
            continue;
        }

        if (bite_ && (c.changedAt < *bite_ - before || c.changedAt > *bite_ + after))
        {
            continue;
        }

        out.push_back(c);
    }

    const auto score = [&](const BiteCandidate& c)
    {
        const Duration mark = bite_.value_or(c.changedAt);
        const auto gap = c.changedAt > mark ? c.changedAt - mark : mark - c.changedAt;
        return std::tuple{!(c.before == 0 && c.after == 1), c.changes, gap};
    };
    std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) { return score(a) < score(b); });
    return out;
}

std::vector<std::uint32_t> commonOffsets(const std::vector<std::vector<std::uint32_t>>& lists)
{
    if (lists.empty())
    {
        return {};
    }

    std::vector<std::uint32_t> common = lists.front();
    std::sort(common.begin(), common.end());

    for (std::size_t i = 1; i < lists.size(); ++i)
    {
        std::vector<std::uint32_t> next = lists[i];
        std::sort(next.begin(), next.end());
        std::vector<std::uint32_t> both;
        std::set_intersection(common.begin(), common.end(), next.begin(), next.end(), std::back_inserter(both));
        common = std::move(both);
    }

    return common;
}

} // namespace og::wowclient
