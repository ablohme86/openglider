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
 *  File: Replay.h
 *
 *  Desc:
 *  Declarations and interface for Replay (game snapshot recording and
 *  deterministic replay provider).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/EventBus.h"
#include "og/state/GameSnapshot.h"
#include "og/state/IGameStateProvider.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace og
{

// .glr replay files are JSON lines:
//
//   {"type":"header","version":1,"provider":"sim"}
//   {"type":"snapshot","i":0,"data":{...GameSnapshot...}}
//   {"type":"action","i":0,"t":1234,"name":"Warrior.Rend"}
//   {"type":"behavior","i":0,"t":1234,"from":"Pull","to":"Combat"}
//
// "i" is the index of the snapshot during whose tick the record happened.

struct ReplayActionRecord
{
    std::size_t snapshotIndex = 0;
    std::int64_t time = 0;
    std::string name;
    bool operator==(const ReplayActionRecord& o) const
    {
        return snapshotIndex == o.snapshotIndex && name == o.name;
    }
};

struct ReplayBehaviorRecord
{
    std::size_t snapshotIndex = 0;
    std::int64_t time = 0;
    std::string from;
    std::string to;
};

struct ReplayData
{
    std::string provider;
    std::vector<GameSnapshot> snapshots;
    std::vector<ReplayActionRecord> actions;
    std::vector<ReplayBehaviorRecord> behaviors;
};

struct ReplayLoadResult
{
    ReplayData data;
    std::vector<std::string> errors;
    bool ok() const
    {
        return errors.empty();
    }
};

ReplayLoadResult loadReplay(const std::filesystem::path& path);

// Records snapshots plus the decisions (actions, behavior switches) made on them.
class SnapshotRecorder
{
public:
    SnapshotRecorder(const std::filesystem::path& path, std::string providerName);

    bool isOpen() const
    {
        return out_.is_open() && static_cast<bool>(out_);
    }

    // Subscribes to ActionExecutedEvent and BehaviorChangedEvent on `events`.
    void attach(EventBus& events);

    void recordSnapshot(const GameSnapshot& snapshot);
    void recordAction(std::string_view action, TimePoint time);
    void recordBehavior(std::string_view from, std::string_view to);
    void flush()
    {
        out_.flush();
    }

    std::size_t snapshotCount() const
    {
        return count_;
    }

private:
    std::size_t currentIndex() const
    {
        return count_ == 0 ? 0 : count_ - 1;
    }

    std::ofstream out_;
    std::size_t count_ = 0;
    TimePoint lastTime_{};
    std::vector<EventBus::Subscription> subscriptions_;
};

// Feeds a recording back to the engine and drives the clock from the
// recorded timestamps, so decisions can be debugged offline.
class ReplayStateProvider final : public IGameStateProvider
{
public:
    ReplayStateProvider(std::vector<GameSnapshot> snapshots, ManualClock& clock) : snapshots_(std::move(snapshots)), clock_(clock)
    {
    }

    std::string name() const override
    {
        return "replay";
    }

    std::optional<GameSnapshot> capture() override;
    // What the recorded session's client did: the target set when the next
    // snapshot has it. Without, a bot that could select by GUID when recorded
    // pressed Target.Next on replay.
    bool setTarget(Guid guid) override;

    bool finished() const
    {
        return next_ >= snapshots_.size();
    }

    // Index of the snapshot returned by the last capture().
    std::size_t index() const
    {
        return next_ == 0 ? 0 : next_ - 1;
    }

    std::size_t size() const
    {
        return snapshots_.size();
    }

private:
    std::vector<GameSnapshot> snapshots_;
    ManualClock& clock_;
    std::size_t next_ = 0;
};

struct ReplayComparison
{
    std::size_t recorded = 0;
    std::size_t replayed = 0;
    std::size_t matching = 0;                   // longest common subsequence length
    std::optional<std::size_t> firstDivergence; // index into the recorded list
    bool identical() const
    {
        return recorded == replayed && matching == recorded;
    }
};

ReplayComparison compareActions(const std::vector<ReplayActionRecord>& recorded, const std::vector<ReplayActionRecord>& replayed);

} // namespace og
