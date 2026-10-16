/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-13
 *  File: Playstyle.h
 *
 *  Desc:
 *  Declarations and interface for Playstyle (bot engine, behaviors, and
 *  core state machine).
 *
 *
 */
#pragma once

#include "og/common/Clock.h"
#include "og/common/Json.h"
#include "og/common/Vec3.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace og
{

struct GameSnapshot;
struct HumanizerConfig;

// How long things take or how far apart they come, in seconds: counted on a
// logarithmic scale from 50 ms to a quarter of an hour, so a few hundred
// jumps and one long stand-still both keep their place.
struct PlaystyleSpread
{
    static constexpr std::size_t kBuckets = 24;
    std::array<std::uint32_t, kBuckets> buckets{};

    void add(double seconds);
    std::uint32_t count() const;
    // The value a share `q` (0-1) of what was counted lies under; 0 when empty.
    double quantile(double q) const;
    bool operator==(const PlaystyleSpread&) const = default;
};

// Percentages (health, mana, a target's health), in steps of five.
struct PlaystyleLevels
{
    static constexpr std::size_t kBuckets = 20;
    std::array<std::uint32_t, kBuckets> buckets{};

    void add(double percent);
    std::uint32_t count() const;
    double quantile(double q) const;
    bool operator==(const PlaystyleLevels&) const = default;
};

// One spell as the player uses it, by its name: ranks come and go.
struct PlaystyleSpell
{
    std::uint32_t casts = 0;
    std::uint32_t opened = 0;      // cast on a mob shortly before the fight began: a pull
    std::uint32_t openedFirst = 0; // the first of those
    std::uint32_t inCombat = 0;
    std::uint32_t outOfCombat = 0; // no fight followed: a buff, or something at rest
    std::uint32_t onTarget = 0;    // with a mob targeted
    std::uint32_t refreshed = 0;   // the target already had it on from us
    std::uint32_t debuffed = 0;    // times it was seen to go on a target as a debuff
    std::uint32_t restored = 0;    // cast out of a fight, and mana was up at once: Life Tap
    std::uint64_t orderSum = 0;    // its places in the fights' casts, added up
    double rangeSum = 0.0;         // yards to the target, added up
    PlaystyleLevels power;         // the player's mana (rage, energy) when cast
    PlaystyleLevels health;
    PlaystyleLevels targetHealth;
    bool operator==(const PlaystyleSpell&) const = default;
};

// What was learned of how the player plays, kept in the combat class file.
struct PlaystyleProfile
{
    // Less than this of moving or fighting says too little to go by.
    static constexpr double kMinActiveSeconds = 600.0;
    static constexpr std::uint32_t kMinFights = 10;

    double activeSeconds = 0.0; // moving, or in a fight
    double travelSeconds = 0.0; // running ahead, out of a fight
    double combatSeconds = 0.0;
    double combatMovingSeconds = 0.0;
    double combatStrafingSeconds = 0.0;
    double combatBackingSeconds = 0.0;
    std::uint32_t fights = 0;
    // The ground under the player on the way, looked at twice a second where
    // the map tells: how often it was road.
    std::uint32_t groundSamples = 0;
    std::uint32_t roadSamples = 0;
    // Yards kept from a wall or a corner when passing one, at the nearest.
    PlaystyleSpread wallPass;

    // On the way: the running between two jumps, sidesteps or stops, and how
    // long a sidestep or a stop lasts.
    PlaystyleSpread jumpEvery;
    PlaystyleSpread strafeEvery;
    PlaystyleSpread strafeFor;
    PlaystyleSpread pauseEvery;
    PlaystyleSpread pauseFor;
    PlaystyleSpread fightFor;

    // Health and mana when the player sat down to eat or drink, and got up.
    PlaystyleLevels restHealth;
    PlaystyleLevels restMana;
    PlaystyleLevels restDoneHealth;
    PlaystyleLevels restDoneMana;

    std::map<std::string, PlaystyleSpell> spells;

    bool movementReady() const
    {
        return activeSeconds >= kMinActiveSeconds;
    }

    bool combatReady() const
    {
        return movementReady() && fights >= kMinFights;
    }

    Json toJson() const;
    static std::optional<PlaystyleProfile> fromJson(const Json& json);
    bool operator==(const PlaystyleProfile&) const = default;
};

// Watches the game while the player has the keys, and counts what they do.
// It never looks at what the bot did: a moment the bot steers is passed over,
// and so is the second after it lets go.
class PlaystyleRecorder
{
public:
    // The name of a spell by its id; empty for one not known.
    using SpellName = std::function<std::string(std::uint32_t spellId)>;

    // The ground at a point: whether it is road, and the yards to the nearest
    // wall (kWallFar where none is near); nullopt where the map does not tell.
    using Ground = std::function<std::optional<std::pair<bool, float>>(const Vec3& at)>;
    static constexpr float kWallFar = 12.0f;

    explicit PlaystyleRecorder(SpellName spellName = {});

    // Carries on from what an earlier session learned.
    void seed(PlaystyleProfile profile);
    void setSpellName(SpellName spellName);
    void setGround(Ground ground);
    void observe(const GameSnapshot& s, TimePoint now, bool playerHasKeys);

    const PlaystyleProfile& profile() const
    {
        return profile_;
    }

private:
    struct Sample
    {
        TimePoint at;
        Vec3 position;
        float heading = 0.0f;
    };

    struct Motion
    {
        float speed = 0.0f;
        float ahead = 0.0f; // along the facing; negative backwards
        float aside = 0.0f; // across it
        float turned = 0.0f; // the facing's change over the same stretch
        bool jumped = false;
    };

    struct OpenCast
    {
        TimePoint at;
        std::string spell;
    };

    // A cast out of a fight, and the mana it was cast at: more of it a moment
    // later says what the spell is for.
    struct IdleCast
    {
        TimePoint at;
        std::string spell;
        float power = 0.0f;
    };

    void forget();
    std::optional<Motion> motion(TimePoint now);
    void travel(const GameSnapshot& s, const Motion& m, TimePoint now, double dt);
    void casts(const GameSnapshot& s, TimePoint now);
    void fights(const GameSnapshot& s, TimePoint now);
    void rest(const GameSnapshot& s);
    void debuffs(const GameSnapshot& s, TimePoint now);
    void ground(const GameSnapshot& s, TimePoint now);

    SpellName spellName_;
    Ground ground_;
    TimePoint groundAt_{};
    // The nearest the wall being passed has come; unset away from walls.
    std::optional<float> passing_;
    PlaystyleProfile profile_;
    std::deque<Sample> samples_;
    std::optional<TimePoint> keysSince_;
    std::optional<TimePoint> last_;
    TimePoint lastJump_{};
    // The height kicking up, and how fast: a jump if it has slowed a moment on.
    std::optional<std::pair<TimePoint, float>> kick_;

    double sinceJump_ = 0.0;
    double sinceStrafe_ = 0.0;
    double sincePause_ = 0.0;
    std::optional<TimePoint> strafeSince_;
    double strafeAfter_ = 0.0;
    std::optional<TimePoint> pauseSince_;
    float pauseHeading_ = 0.0f;
    double aheadFor_ = 0.0; // running ahead without a break
    bool travelled_ = false; // running ahead since the last stop or fight

    std::uint32_t lastCast_ = 0;
    std::vector<OpenCast> openCasts_; // on a mob, out of a fight: a pull, if one follows
    std::vector<IdleCast> idleCasts_;
    bool inCombat_ = false;
    // Whether the fight under way began with the player at the keys: one the
    // bot began has no start to time it from, nor a first cast to count from.
    bool fightOurs_ = false;
    TimePoint fightSince_{};
    std::uint32_t fightOrder_ = 0;
    bool eating_ = false;
    bool drinking_ = false;
    // Our debuffs on the target as last looked at, to tell a new one.
    std::uint64_t debuffsOn_ = 0;
    std::vector<std::string> debuffsSeen_;
    TimePoint debuffsAt_{};
};

// Where the player travels: on the roads or off them, and how wide of walls.
// Each is unset until enough was seen of it.
struct PlaystyleTravel
{
    std::optional<bool> useRoads;
    std::optional<float> wallClearance; // yards, for NavigatorConfig::wallClearance
    std::vector<std::string> said;
};

PlaystyleTravel travelOf(const PlaystyleProfile& profile);

// Sets the humanizer to travel the way the player does: jumps, sidesteps and
// stops as far apart and as long as theirs, and none of a kind they never do.
// Nothing is touched before the profile has enough to go by, nor a kind seen
// too little of. Returns what it set, a line each, for the log.
std::vector<std::string> applyPlaystyle(const PlaystyleProfile& profile, HumanizerConfig& humanizer);

// The profile lives at the end of the combat class file, on lines the script
// parser takes for comments and the editor does not show:
//
//   #@trained {"v":1,...}
inline constexpr std::string_view kTrainedMarker = "#@trained ";

std::optional<PlaystyleProfile> readTrained(std::string_view ogc);
// The script as the player wrote it, without those lines.
std::string withoutTrained(std::string_view ogc);
// The lines alone, as they stand, for whoever rewrites the script to put back.
std::string trainedLines(std::string_view ogc);
std::string withTrainedLines(std::string_view script, std::string_view lines);
std::string withTrained(std::string_view ogc, const PlaystyleProfile& profile);

} // namespace og
