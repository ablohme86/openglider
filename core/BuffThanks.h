/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-29
 *  File: BuffThanks.h
 *
 *  Desc:
 *  Declarations and interface for BuffThanks (core bot engine,
 *  behaviors, context, and objective tracking).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Log.h"
#include "og/core/BotConfig.h"
#include "og/state/GameSnapshot.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace og
{

// Answers a buff from another friendly player with a chat command
// (BuffThanksConfig), once per player per while.
class BuffThanks
{
public:
    // `delay` draws the wait before the command; `chat` types it and says
    // whether it went. Buffs already on us the first time are not news.
    void update(const GameSnapshot& s, const BuffThanksConfig& config, TimePoint now, const std::function<Duration()>& delay, const std::function<bool(const std::string&)>& chat, Logger& log);

private:
    // Helpful auras on us from someone else: (spell, caster).
    std::set<std::pair<std::uint32_t, Guid>> seen_;
    bool primed_ = false;
    // Who is due a command, and from when; when each was last answered.
    std::optional<std::pair<Guid, TimePoint>> pending_;
    std::string pendingName_;
    std::unordered_map<Guid, TimePoint> answered_;
};

} // namespace og
