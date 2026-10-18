/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-17
 *  File: TaxiTrip.h
 *
 *  Desc:
 *  A flight on the way: to the flight master, talked to, the flight
 *  point on the flight map clicked, flown, landed.
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/state/GameSnapshot.h"
#include "og/state/IGameStateProvider.h"
#include "og/world/TaxiNetwork.h"

#include <optional>
#include <string>
#include <vector>

namespace og
{

class BotContext;

// Which button on the flight map is a flight point: the buttons shown are the
// flight points learnt, where the continent's map has them. `buttons` are the
// TaxiButtonN frames shown (N, where), `map` the map's frame; the flight
// points learnt on `mapId` are placed on the continent's map and matched to
// the nearest. nullopt where `target`'s button is not the one nearest it.
std::optional<int> taxiButtonFor(std::uint32_t target, std::uint32_t mapId, const std::vector<std::pair<int, UiFrame>>& buttons, const UiFrame& map, const TaxiNetwork& network, const std::vector<std::uint32_t>& known);

class TaxiTrip
{
public:
    enum class Status
    {
        Running,
        Landed,
        Failed
    };

    TaxiTrip(const TaxiPlan& plan, std::uint32_t mapId) : plan_(plan), mapId_(mapId)
    {
    }

    Status tick(const GameSnapshot& s, BotContext& ctx);

    const TaxiPlan& plan() const
    {
        return plan_;
    }

    bool flying() const
    {
        return stage_ == Stage::Flying && takenOff_;
    }

    const std::string& why() const
    {
        return why_;
    }

private:
    enum class Stage
    {
        ToMaster,
        Talking,
        Flying
    };

    Status fail(BotContext& ctx, std::string why);

    TaxiPlan plan_;
    std::uint32_t mapId_ = 0;
    Stage stage_ = Stage::ToMaster;
    TimePoint started_{}, stageAt_{}, talkedAt_{}, clickedAt_{}, stillSince_{}, mapSeenAt_{};
    Guid master_ = 0;
    int talks_ = 0, clicks_ = 0, rightClicks_ = 0;
    bool mouseTried_ = false, closeIn_ = false, gossipChosen_ = false;
    bool takenOff_ = false;
    std::optional<Vec3> lastPosition_;
    std::string why_;
};

} // namespace og
