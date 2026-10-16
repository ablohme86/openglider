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
 *  File: Replay.cpp
 *
 *  Desc:
 *  Implementation of Replay (game snapshot recording and deterministic
 *  replay provider).
 *
 *
 */
#include "og/replay/Replay.h"

#include "og/actions/ActionService.h"
#include "og/common/Json.h"
#include "og/core/Events.h"
#include "og/state/SnapshotJson.h"

#include <algorithm>
#include <format>

namespace og
{

SnapshotRecorder::SnapshotRecorder(const std::filesystem::path& path, std::string providerName)
{
    std::error_code ec;

    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    out_.open(path, std::ios::trunc);

    if (!out_)
    {
        return;
    }

    Json header = Json::object();
    header["type"] = "header";
    header["version"] = 1;
    header["provider"] = std::move(providerName);
    out_ << header.dump() << '\n';
}

void SnapshotRecorder::attach(EventBus& events)
{
    subscriptions_.push_back(events.subscribe<ActionExecutedEvent>([this](const ActionExecutedEvent& e) { recordAction(e.action, e.time); }));
    subscriptions_.push_back(events.subscribe<BehaviorChangedEvent>([this](const BehaviorChangedEvent& e) { recordBehavior(e.from, e.to); }));
}

void SnapshotRecorder::recordSnapshot(const GameSnapshot& snapshot)
{
    if (!out_)
    {
        return;
    }

    Json line = Json::object();
    line["type"] = "snapshot";
    line["i"] = count_;
    line["data"] = toJson(snapshot);
    out_ << line.dump() << '\n';
    lastTime_ = snapshot.timestamp;
    ++count_;
}

void SnapshotRecorder::recordAction(std::string_view action, TimePoint time)
{
    if (!out_)
    {
        return;
    }

    Json line = Json::object();
    line["type"] = "action";
    line["i"] = currentIndex();
    line["t"] = toMillis(time);
    line["name"] = action;
    out_ << line.dump() << '\n';
}

void SnapshotRecorder::recordBehavior(std::string_view from, std::string_view to)
{
    if (!out_)
    {
        return;
    }

    Json line = Json::object();
    line["type"] = "behavior";
    line["i"] = currentIndex();
    line["t"] = toMillis(lastTime_);
    line["from"] = from;
    line["to"] = to;
    out_ << line.dump() << '\n';
}

ReplayLoadResult loadReplay(const std::filesystem::path& path)
{
    ReplayLoadResult result;
    std::ifstream in(path);

    if (!in)
    {
        result.errors.push_back(std::format("cannot open {}", path.string()));
        return result;
    }

    std::string line;
    std::size_t lineNo = 0;

    while (std::getline(in, line))
    {
        ++lineNo;

        if (line.empty())
        {
            continue;
        }

        try
        {
            const Json j = Json::parse(line);
            const std::string& type = j["type"].asString();

            if (type == "header")
            {
                result.data.provider = j["provider"].asString();
            }
            else if (type == "snapshot")
            {
                result.data.snapshots.push_back(snapshotFromJson(j["data"]));
            }
            else if (type == "action")
            {
                result.data.actions.push_back({static_cast<std::size_t>(j["i"].asInt()), j["t"].asInt(), j["name"].asString()});
            }
            else if (type == "behavior")
            {
                result.data.behaviors.push_back({static_cast<std::size_t>(j["i"].asInt()), j["t"].asInt(), j["from"].asString(), j["to"].asString()});
            }
        }
        catch (const JsonError& e)
        {
            result.errors.push_back(std::format("line {}: {}", lineNo, e.what()));

            if (result.errors.size() > 20)
            {
                break;
            }
        }
    }

    return result;
}

std::optional<GameSnapshot> ReplayStateProvider::capture()
{
    if (next_ >= snapshots_.size())
    {
        return std::nullopt;
    }

    GameSnapshot s = snapshots_[next_++];
    clock_.set(s.timestamp);
    return s;
}

bool ReplayStateProvider::setTarget(Guid guid)
{
    return next_ < snapshots_.size() && snapshots_[next_].targetGuid == guid;
}

ReplayComparison compareActions(const std::vector<ReplayActionRecord>& a, const std::vector<ReplayActionRecord>& b)
{
    ReplayComparison c;
    c.recorded = a.size();
    c.replayed = b.size();

    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        if (!(a[i] == b[i]))
        {
            c.firstDivergence = i;
            break;
        }
    }

    if (!c.firstDivergence && a.size() != b.size())
    {
        c.firstDivergence = std::min(a.size(), b.size());
    }

    // LCS over (snapshot index, action) pairs - O(n*m) memory-light version.
    std::vector<std::size_t> prev(b.size() + 1, 0), cur(b.size() + 1, 0);

    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            cur[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] + 1 : std::max(prev[j], cur[j - 1]);
        }

        std::swap(prev, cur);
    }

    c.matching = prev[b.size()];
    return c;
}

} // namespace og
