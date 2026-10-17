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
 *  File: TaxiNetwork.h
 *
 *  Desc:
 *  The flight paths: where each flight point is, the flights between
 *  them, and whether flying beats walking to a far destination.
 *
 */
#pragma once

#include "og/common/Vec3.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace og
{

// A flight point (TaxiNodes id): where its flight master's flights start.
struct TaxiNode
{
    std::uint32_t id = 0;
    std::uint32_t mapId = 0;
    Vec3 position;
};

// A flight from one flight point to another, as the game flies it.
struct TaxiFlight
{
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    std::uint32_t cost = 0; // copper
    float length = 0.0f;    // yards along the way flown
};

// Flying to a destination: walked to a flight point, flown to another,
// walked on from there.
struct TaxiPlan
{
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    Vec3 fromPosition, toPosition;
    float seconds = 0.0f;     // the whole way, walks and flight
    float walkSeconds = 0.0f; // walking all the way instead
    std::uint32_t cost = 0;   // copper, every flight of it
};

class TaxiNetwork
{
public:
    // From the client's TaxiPath and TaxiPathNode tables (DB2): the first
    // point of a flight is where its flight point is. nullopt, `error` saying
    // why, where either is not one the bot knows.
    static std::optional<TaxiNetwork> fromDb2(std::span<const std::uint8_t> taxiPath, std::span<const std::uint8_t> taxiPathNode, std::string& error);
    static constexpr std::uint32_t kTaxiPathFileDataId = 1067802;
    static constexpr std::uint32_t kTaxiPathNodeFileDataId = 1000437;

    void addNode(TaxiNode node);
    void addFlight(TaxiFlight flight);

    const std::map<std::uint32_t, TaxiNode>& nodes() const
    {
        return nodes_;
    }

    const TaxiNode* node(std::uint32_t id) const;

    // Yards and copper of the shortest flight from `from` to `to` through the
    // flight points `known` (both ends among them); nullopt without one.
    struct Route
    {
        float length = 0.0f;
        std::uint32_t cost = 0;
    };
    std::optional<Route> route(std::uint32_t from, std::uint32_t to, const std::vector<std::uint32_t>& known) const;

    // Whether to fly from `at` to `destination` on `mapId`, by the flight
    // points `known`: walking is guessed at `walkFactor` times the straight
    // line, at `runSpeed`; flying at kFlightSpeed with kFlightOverhead for
    // getting there, talking, rising and landing. A plan only where it saves
    // a minute and a tenth of the time at least, and `money` (copper; -1 not
    // known) pays for it.
    static constexpr float kFlightSpeed = 30.0f;
    static constexpr float kFlightOverhead = 30.0f;
    std::optional<TaxiPlan> plan(std::uint32_t mapId, const Vec3& at, const Vec3& destination, const std::vector<std::uint32_t>& known, std::int64_t money, float runSpeed = 7.0f, float walkFactor = 1.3f) const;

private:
    std::map<std::uint32_t, TaxiNode> nodes_;
    std::vector<TaxiFlight> flights_;
};

} // namespace og
