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
 *  File: Playstyle.cpp
 *
 *  Desc:
 *  Implementation of Playstyle (bot engine, behaviors, and core state
 *  machine).
 *
 *
 */
#include "og/core/Playstyle.h"

#include "og/core/BotConfig.h"
#include "og/state/GameSnapshot.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <tuple>

namespace og
{

namespace
{

// The spread's scale: a bucket is half as wide again as the one before it.
constexpr double kSpreadFrom = 0.05;
constexpr double kSpreadStep = 1.5;

// What the samples are kept for, and the stretches a speed is taken over.
constexpr Duration kSamplesKept{400};
constexpr Duration kSpeedOver{100};
constexpr Duration kClimbOver{50};
// The bot's last key is still in the character's legs for a moment.
constexpr Duration kKeysSettle{1500};
// A cast on a mob this long before the fight was the pull.
constexpr Duration kPullWithin{8000};

constexpr float kMoving = 1.0f;   // yards a second
constexpr float kAhead = 2.0f;
constexpr float kAside = 2.5f;
constexpr float kBacking = -1.5f;
constexpr float kStill = 0.5f;
// The facing's change in a tenth of a second under which it is steady.
constexpr float kSteady = 0.06f;
// A stop on the way comes after this much running, and goes on the same way.
constexpr double kOnTheWay = 5.0;
constexpr float kSameWay = 0.8f;
// A jump leaves the ground at 8 yards a second, all at once, and gravity has
// most of that back a moment later. The foot of a ramp kicks up as fast, but
// goes on climbing.
constexpr float kJumpKick = 4.5f;
constexpr float kJumpClimb = 3.0f;
constexpr float kJumpSlowed = 2.5f;
constexpr Duration kJumpSlowedAfter{200};
constexpr Duration kJumpApart{600};

// What the counts mean. 2: a sidestep is counted with the facing steady only,
// a stop only on the way; what 1 counted of them is not gone by.
constexpr int kVersion = 2;

template <std::size_t N>
std::uint32_t total(const std::array<std::uint32_t, N>& buckets)
{
    std::uint32_t sum = 0;

    for (const auto n : buckets)
    {
        sum += n;
    }

    return sum;
}

// The bucket a share `q` of the count falls in; N when nothing was counted.
template <std::size_t N>
std::size_t bucketAt(const std::array<std::uint32_t, N>& buckets, double q)
{
    const std::uint32_t sum = total(buckets);

    if (sum == 0)
    {
        return N;
    }

    const double wanted = std::clamp(q, 0.0, 1.0) * static_cast<double>(sum);
    double seen = 0.0;

    for (std::size_t i = 0; i < N; ++i)
    {
        seen += static_cast<double>(buckets[i]);

        if (buckets[i] != 0 && seen >= wanted)
        {
            return i;
        }
    }

    return N - 1;
}

// Trailing zeros are left out: most of a spread is empty.
template <std::size_t N>
Json bucketsToJson(const std::array<std::uint32_t, N>& buckets)
{
    std::size_t used = N;

    while (used > 0 && buckets[used - 1] == 0)
    {
        --used;
    }

    Json out = Json::array();

    for (std::size_t i = 0; i < used; ++i)
    {
        out.push_back(Json(buckets[i]));
    }

    return out;
}

template <std::size_t N>
void bucketsFromJson(const Json* json, std::array<std::uint32_t, N>& buckets)
{
    buckets = {};

    if (!json || !json->isArray())
    {
        return;
    }

    const auto& list = json->asArray();

    for (std::size_t i = 0; i < std::min(N, list.size()); ++i)
    {
        buckets[i] = static_cast<std::uint32_t>(std::max<std::int64_t>(0, list[i].asInt()));
    }
}

double tenths(double value)
{
    return std::round(value * 10.0) / 10.0;
}

double numberOf(const Json& json, std::string_view key)
{
    const Json* value = json.find(key);
    return value && value->isNumber() ? std::max(0.0, value->asDouble()) : 0.0;
}

std::uint32_t countOf(const Json& json, std::string_view key)
{
    return static_cast<std::uint32_t>(numberOf(json, key));
}

float shortestTurn(float from, float to)
{
    const float two = 2.0f * std::numbers::pi_v<float>;
    float d = std::fmod(to - from, two);

    if (d > std::numbers::pi_v<float>)
    {
        d -= two;
    }
    else if (d < -std::numbers::pi_v<float>)
    {
        d += two;
    }

    return d;
}

// Mana where the character has any, else what it fights with (rage, energy).
float powerPercent(const PlayerState& p)
{
    if (p.maxPowerOf(PowerType::Mana) > 0)
    {
        return p.powerPercentOf(PowerType::Mana);
    }

    for (std::size_t i = 0; i < p.maxPowers.size(); ++i)
    {
        if (p.maxPowers[i] > 0)
        {
            return 100.0f * static_cast<float>(p.powers[i]) / static_cast<float>(p.maxPowers[i]);
        }
    }

    return 0.0f;
}

bool hasAura(const UnitState& u, std::string_view name)
{
    return std::any_of(u.auras.begin(), u.auras.end(), [&](const Aura& a) { return a.name == name; });
}

// Calls `line` with each line of a text, without its line end.
template <class F>
void eachLine(std::string_view text, F&& line)
{
    while (!text.empty())
    {
        const auto end = text.find('\n');
        std::string_view one = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

        if (!one.empty() && one.back() == '\r')
        {
            one.remove_suffix(1);
        }

        line(one);
    }
}

} // namespace

void PlaystyleSpread::add(double seconds)
{
    if (!(seconds > 0.0))
    {
        return;
    }

    const double steps = std::log(std::max(seconds, kSpreadFrom) / kSpreadFrom) / std::log(kSpreadStep);
    ++buckets[std::min(kBuckets - 1, static_cast<std::size_t>(steps))];
}

std::uint32_t PlaystyleSpread::count() const
{
    return total(buckets);
}

double PlaystyleSpread::quantile(double q) const
{
    const std::size_t at = bucketAt(buckets, q);
    // The middle of the bucket, on its own scale.
    return at == kBuckets ? 0.0 : kSpreadFrom * std::pow(kSpreadStep, static_cast<double>(at) + 0.5);
}

void PlaystyleLevels::add(double percent)
{
    if (std::isnan(percent))
    {
        return;
    }

    const double clamped = std::clamp(percent, 0.0, 100.0);
    ++buckets[std::min(kBuckets - 1, static_cast<std::size_t>(clamped / 5.0))];
}

std::uint32_t PlaystyleLevels::count() const
{
    return total(buckets);
}

double PlaystyleLevels::quantile(double q) const
{
    const std::size_t at = bucketAt(buckets, q);
    return at == kBuckets ? 0.0 : static_cast<double>(at) * 5.0 + 2.5;
}

Json PlaystyleProfile::toJson() const
{
    Json out = Json::object();
    out["v"] = Json(kVersion);
    out["active"] = Json(tenths(activeSeconds));
    out["travel"] = Json(tenths(travelSeconds));
    out["combat"] = Json(tenths(combatSeconds));
    out["combatMoving"] = Json(tenths(combatMovingSeconds));
    out["combatStrafing"] = Json(tenths(combatStrafingSeconds));
    out["combatBacking"] = Json(tenths(combatBackingSeconds));
    out["fights"] = Json(fights);
    out["ground"] = Json(groundSamples);
    out["road"] = Json(roadSamples);
    out["wallPass"] = bucketsToJson(wallPass.buckets);
    out["jumpEvery"] = bucketsToJson(jumpEvery.buckets);
    out["strafeEvery"] = bucketsToJson(strafeEvery.buckets);
    out["strafeFor"] = bucketsToJson(strafeFor.buckets);
    out["pauseEvery"] = bucketsToJson(pauseEvery.buckets);
    out["pauseFor"] = bucketsToJson(pauseFor.buckets);
    out["fightFor"] = bucketsToJson(fightFor.buckets);
    out["restHealth"] = bucketsToJson(restHealth.buckets);
    out["restMana"] = bucketsToJson(restMana.buckets);
    out["restDoneHealth"] = bucketsToJson(restDoneHealth.buckets);
    out["restDoneMana"] = bucketsToJson(restDoneMana.buckets);

    Json all = Json::object();

    for (const auto& [name, s] : spells)
    {
        Json one = Json::object();
        one["n"] = Json(s.casts);
        one["opened"] = Json(s.opened);
        one["first"] = Json(s.openedFirst);
        one["fight"] = Json(s.inCombat);
        one["idle"] = Json(s.outOfCombat);
        one["target"] = Json(s.onTarget);
        one["refreshed"] = Json(s.refreshed);
        one["debuffed"] = Json(s.debuffed);
        one["restored"] = Json(s.restored);
        one["order"] = Json(static_cast<std::int64_t>(s.orderSum));
        one["range"] = Json(tenths(s.rangeSum));
        one["power"] = bucketsToJson(s.power.buckets);
        one["health"] = bucketsToJson(s.health.buckets);
        one["targetHealth"] = bucketsToJson(s.targetHealth.buckets);
        all[name] = std::move(one);
    }

    out["spells"] = std::move(all);
    return out;
}

std::optional<PlaystyleProfile> PlaystyleProfile::fromJson(const Json& json)
{
    // A later version's counts may mean something else: left alone, not guessed at.
    if (!json.isObject() || countOf(json, "v") != kVersion)
    {
        return std::nullopt;
    }

    PlaystyleProfile p;
    p.activeSeconds = numberOf(json, "active");
    p.travelSeconds = numberOf(json, "travel");
    p.combatSeconds = numberOf(json, "combat");
    p.combatMovingSeconds = numberOf(json, "combatMoving");
    p.combatStrafingSeconds = numberOf(json, "combatStrafing");
    p.combatBackingSeconds = numberOf(json, "combatBacking");
    p.fights = countOf(json, "fights");
    p.groundSamples = countOf(json, "ground");
    p.roadSamples = countOf(json, "road");
    bucketsFromJson(json.find("wallPass"), p.wallPass.buckets);
    bucketsFromJson(json.find("jumpEvery"), p.jumpEvery.buckets);
    bucketsFromJson(json.find("strafeEvery"), p.strafeEvery.buckets);
    bucketsFromJson(json.find("strafeFor"), p.strafeFor.buckets);
    bucketsFromJson(json.find("pauseEvery"), p.pauseEvery.buckets);
    bucketsFromJson(json.find("pauseFor"), p.pauseFor.buckets);
    bucketsFromJson(json.find("fightFor"), p.fightFor.buckets);
    bucketsFromJson(json.find("restHealth"), p.restHealth.buckets);
    bucketsFromJson(json.find("restMana"), p.restMana.buckets);
    bucketsFromJson(json.find("restDoneHealth"), p.restDoneHealth.buckets);
    bucketsFromJson(json.find("restDoneMana"), p.restDoneMana.buckets);

    if (const Json* all = json.find("spells"); all && all->isObject())
    {
        for (const auto& [name, one] : all->asObject())
        {
            if (name.empty() || !one.isObject())
            {
                continue;
            }

            PlaystyleSpell s;
            s.casts = countOf(one, "n");
            s.opened = countOf(one, "opened");
            s.openedFirst = countOf(one, "first");
            s.inCombat = countOf(one, "fight");
            s.outOfCombat = countOf(one, "idle");
            s.onTarget = countOf(one, "target");
            s.refreshed = countOf(one, "refreshed");
            s.debuffed = countOf(one, "debuffed");
            s.restored = countOf(one, "restored");
            s.orderSum = static_cast<std::uint64_t>(numberOf(one, "order"));
            s.rangeSum = numberOf(one, "range");
            bucketsFromJson(one.find("power"), s.power.buckets);
            bucketsFromJson(one.find("health"), s.health.buckets);
            bucketsFromJson(one.find("targetHealth"), s.targetHealth.buckets);
            p.spells.emplace(name, std::move(s));
        }
    }

    return p;
}

PlaystyleRecorder::PlaystyleRecorder(SpellName spellName) : spellName_(std::move(spellName))
{
}

void PlaystyleRecorder::seed(PlaystyleProfile profile)
{
    profile_ = std::move(profile);
    forget();
}

void PlaystyleRecorder::setSpellName(SpellName spellName)
{
    spellName_ = std::move(spellName);
}

void PlaystyleRecorder::setGround(Ground ground)
{
    ground_ = std::move(ground);
}

// On the way, the ground under the player: road or not, and how near they let
// a wall come as they pass it.
void PlaystyleRecorder::ground(const GameSnapshot& s, TimePoint now)
{
    // Near a wall the pass is over in a second: looked at more often.
    if (!ground_ || now - groundAt_ < (passing_ ? Duration(150) : Duration(500)))
    {
        return;
    }

    groundAt_ = now;
    const auto at = ground_(s.player.position);

    if (!at)
    {
        passing_.reset();
        return;
    }

    ++profile_.groundSamples;

    if (at->first)
    {
        ++profile_.roadSamples;
    }

    if (at->second < kWallFar * 0.75f)
    {
        passing_ = std::min(passing_.value_or(at->second), at->second);
    }
    else if (passing_)
    {
        profile_.wallPass.add(std::max(0.05f, *passing_));
        passing_.reset();
    }
}

void PlaystyleRecorder::forget()
{
    samples_.clear();
    kick_.reset();
    passing_.reset();
    keysSince_.reset();
    last_.reset();
    strafeSince_.reset();
    pauseSince_.reset();
    travelled_ = false;
    aheadFor_ = 0.0;
    lastCast_ = 0;
    openCasts_.clear();
    idleCasts_.clear();
    inCombat_ = false;
    fightOurs_ = false;
    eating_ = false;
    drinking_ = false;
    debuffsOn_ = 0;
    debuffsSeen_.clear();
}

std::optional<PlaystyleRecorder::Motion> PlaystyleRecorder::motion(TimePoint now)
{
    // The latest sample at least `ago` behind the newest one.
    const auto before = [this](TimePoint at, Duration ago) -> const Sample*
    {
        for (auto it = samples_.rbegin(); it != samples_.rend(); ++it)
        {
            if (at - it->at >= ago)
            {
                return &*it;
            }
        }

        return nullptr;
    };

    const Sample& newest = samples_.back();
    const Sample* then = before(newest.at, kSpeedOver);

    if (!then)
    {
        return std::nullopt;
    }

    const float dt = static_cast<float>(toSeconds(newest.at - then->at));
    const float vx = (newest.position.x - then->position.x) / dt;
    const float vy = (newest.position.y - then->position.y) / dt;
    // Turning, the facing at either end would make a run look like a sidestep.
    const float facing = then->heading + shortestTurn(then->heading, newest.heading) / 2.0f;

    Motion m;
    m.turned = std::abs(shortestTurn(then->heading, newest.heading));
    m.speed = std::hypot(vx, vy);
    m.ahead = vx * std::cos(facing) + vy * std::sin(facing);
    m.aside = -vx * std::sin(facing) + vy * std::cos(facing);

    // The climb now against the climb a moment ago.
    const Sample* a = before(newest.at, kClimbOver);
    const Sample* b = a ? before(a->at, kClimbOver) : nullptr;
    const Sample* c = b ? before(b->at, kClimbOver) : nullptr;

    if (a && b && c)
    {
        const float climbing = (newest.position.z - a->position.z) / static_cast<float>(toSeconds(newest.at - a->at));
        const float climbed = (b->position.z - c->position.z) / static_cast<float>(toSeconds(b->at - c->at));

        if (kick_)
        {
            // The kick is caught on its way up: the fastest of it counts.
            kick_->second = std::max(kick_->second, climbing);

            if (now - kick_->first >= kJumpSlowedAfter)
            {
                m.jumped = climbing <= kick_->second - kJumpSlowed;
                kick_.reset();
            }
        }
        else if (now - lastJump_ >= kJumpApart && climbing >= kJumpClimb && climbing - climbed >= kJumpKick)
        {
            kick_ = {now, climbing};
        }
    }

    return m;
}

void PlaystyleRecorder::observe(const GameSnapshot& s, TimePoint now, bool playerHasKeys)
{
    const PlayerState& p = s.player;

    if (!playerHasKeys || p.isDeadOrGhost())
    {
        forget();
        return;
    }

    double dt = last_ ? toSeconds(now - *last_) : 0.0;
    last_ = now;

    // A loading screen, a stall: nothing is made of the leap across it.
    if (dt > 0.5)
    {
        samples_.clear();
        kick_.reset();
        strafeSince_.reset();
        pauseSince_.reset();
        dt = 0.0;
    }

    samples_.push_back({now, p.position, p.heading});

    while (now - samples_.front().at > kSamplesKept)
    {
        samples_.pop_front();
    }

    if (!keysSince_)
    {
        keysSince_ = now;
    }

    if (now - *keysSince_ < kKeysSettle)
    {
        // A fight or a cast the bot began is not the player's.
        lastCast_ = p.castingSpell;
        inCombat_ = s.inCombat();
        fightOurs_ = false;
        return;
    }

    fights(s, now);
    casts(s, now);
    debuffs(s, now);
    rest(s);

    const auto m = motion(now);

    if (!m || dt <= 0.0)
    {
        return;
    }

    const bool moving = m->speed > kMoving;

    if (moving || s.inCombat())
    {
        profile_.activeSeconds += dt;
    }

    if (m->jumped)
    {
        lastJump_ = now;
    }

    if (s.inCombat())
    {
        profile_.combatSeconds += dt;

        if (moving)
        {
            profile_.combatMovingSeconds += dt;
        }

        if (std::abs(m->aside) > kAside)
        {
            profile_.combatStrafingSeconds += dt;
        }

        if (m->ahead < kBacking)
        {
            profile_.combatBackingSeconds += dt;
        }

        strafeSince_.reset();
        pauseSince_.reset();
        travelled_ = false;
        aheadFor_ = 0.0;
        return;
    }

    travel(s, *m, now, dt);
}

void PlaystyleRecorder::travel(const GameSnapshot& s, const Motion& m, TimePoint now, double dt)
{
    const PlayerState& p = s.player;
    const bool busy = p.isCasting || p.castingSpell != 0 || p.isSitting;
    const bool ahead = m.ahead > kAhead;
    // With the facing steady only: swung round by the mouse on the run, the
    // character goes on the way it was going for a moment, which looks like a
    // step aside - and the bot, taught so, sidestepped every few seconds.
    const bool aside = m.speed > kMoving && std::abs(m.aside) > kAside && m.turned < kSteady;

    if (ahead)
    {
        ground(s, now);
        profile_.travelSeconds += dt;
        sinceJump_ += dt;
        sinceStrafe_ += dt;
        sincePause_ += dt;
    }

    if (m.jumped && ahead)
    {
        profile_.jumpEvery.add(sinceJump_);
        sinceJump_ = 0.0;
    }

    if (aside)
    {
        if (!strafeSince_)
        {
            strafeSince_ = now;
            strafeAfter_ = sinceStrafe_;
        }
    }
    else if (strafeSince_)
    {
        const double lasted = toSeconds(now - *strafeSince_);

        // On the way only: a step aside from a standstill is no habit of travel.
        if (lasted >= 0.3 && travelled_)
        {
            profile_.strafeFor.add(lasted);
            profile_.strafeEvery.add(strafeAfter_);
            sinceStrafe_ = 0.0;
        }

        strafeSince_.reset();
    }

    if (m.speed < kStill)
    {
        if (busy)
        {
            // Stopped to do something: not a stop for its own sake.
            pauseSince_.reset();
            travelled_ = false;
        }
        else if (travelled_ && !pauseSince_ && aheadFor_ >= kOnTheWay)
        {
            pauseSince_ = now;
            pauseHeading_ = p.heading;
        }

        aheadFor_ = 0.0;
    }
    else if (pauseSince_ && m.speed > kMoving)
    {
        const double lasted = toSeconds(now - *pauseSince_);

        // Longer than that the player was away, or at something the bot cannot
        // see. And on the same way after it: stopped to loot, to talk, to turn
        // back, the player went off another way.
        if (lasted >= 0.4 && lasted <= 20.0 && std::abs(shortestTurn(pauseHeading_, p.heading)) <= kSameWay)
        {
            profile_.pauseFor.add(lasted);
            profile_.pauseEvery.add(sincePause_);
            sincePause_ = 0.0;
        }

        pauseSince_.reset();
    }

    if (ahead)
    {
        travelled_ = true;
        aheadFor_ += dt;
    }
}

void PlaystyleRecorder::fights(const GameSnapshot& s, TimePoint now)
{
    const bool combat = s.inCombat();

    if (combat && !inCombat_)
    {
        ++profile_.fights;
        fightOurs_ = true;
        fightSince_ = now;
        fightOrder_ = 0;
        bool first = true;

        for (const OpenCast& cast : openCasts_)
        {
            PlaystyleSpell& spell = profile_.spells[cast.spell];
            ++spell.opened;

            if (first)
            {
                ++spell.openedFirst;
                first = false;
            }
        }

        openCasts_.clear();
    }
    else if (!combat && inCombat_)
    {
        if (fightOurs_)
        {
            profile_.fightFor.add(toSeconds(now - fightSince_));
        }

        fightOurs_ = false;
    }

    inCombat_ = combat;

    // No fight came of it: a cast for its own sake.
    std::erase_if(openCasts_, [&](const OpenCast& cast)
    {
        if (now - cast.at <= kPullWithin)
        {
            return false;
        }

        ++profile_.spells[cast.spell].outOfCombat;
        return true;
    });
}

void PlaystyleRecorder::casts(const GameSnapshot& s, TimePoint now)
{
    const PlayerState& p = s.player;
    const std::uint32_t casting = p.castingSpell;
    const bool began = casting != 0 && casting != lastCast_;
    lastCast_ = casting;

    // Mana well up a moment after a cast between fights: what it was cast
    // for. Regeneration alone does not give that much that fast.
    std::erase_if(idleCasts_, [&](const IdleCast& cast)
    {
        if (now - cast.at < Duration(1500))
        {
            return false;
        }

        if (!s.inCombat() && powerPercent(p) >= cast.power + 8.0f)
        {
            ++profile_.spells[cast.spell].restored;
        }

        return true;
    });

    if (!began || !spellName_)
    {
        return;
    }

    const std::string name = spellName_(casting);

    // Taken over in the middle of a fight, the casts have no place in it to
    // count from: left out, rather than put the rotation in the wrong order.
    if (name.empty() || (s.inCombat() && !fightOurs_))
    {
        return;
    }

    PlaystyleSpell& spell = profile_.spells[name];
    ++spell.casts;
    spell.power.add(powerPercent(p));
    spell.health.add(p.healthPercent());

    const UnitState* target = s.target();
    const bool onMob = target && !target->isPlayer && target->isAttackable && target->isAlive();

    if (onMob)
    {
        ++spell.onTarget;
        spell.targetHealth.add(target->healthPercent());
        spell.rangeSum += distance(p.position, target->position);

        if (std::any_of(target->auras.begin(), target->auras.end(), [&](const Aura& a) { return a.name == name && a.caster == p.guid; }))
        {
            ++spell.refreshed;
        }
    }

    if (s.inCombat())
    {
        ++spell.inCombat;
        spell.orderSum += fightOrder_++;
    }
    else if (onMob)
    {
        openCasts_.push_back({now, name});
    }
    else
    {
        ++spell.outOfCombat;
        idleCasts_.push_back({now, name, powerPercent(p)});
    }
}

// A spell of ours that shows on the target as a debuff of its own name is
// one to keep up: Corruption, Rend. Looked for a few times a second.
void PlaystyleRecorder::debuffs(const GameSnapshot& s, TimePoint now)
{
    if (now - debuffsAt_ < Duration(200))
    {
        return;
    }

    debuffsAt_ = now;
    const UnitState* target = s.target();
    const std::uint64_t on = target ? target->guid : 0;
    std::vector<std::string> seen;

    if (target)
    {
        for (const Aura& a : target->auras)
        {
            if (a.harmful && a.caster == s.player.guid && !a.name.empty())
            {
                seen.push_back(a.name);
            }
        }
    }

    // On the same target only: one switched to may have had them for a while.
    if (on != 0 && on == debuffsOn_)
    {
        for (const std::string& name : seen)
        {
            if (std::find(debuffsSeen_.begin(), debuffsSeen_.end(), name) == debuffsSeen_.end())
            {
                if (const auto spell = profile_.spells.find(name); spell != profile_.spells.end())
                {
                    ++spell->second.debuffed;
                }
            }
        }
    }

    debuffsOn_ = on;
    debuffsSeen_ = std::move(seen);
}

void PlaystyleRecorder::rest(const GameSnapshot& s)
{
    const PlayerState& p = s.player;
    const bool eating = !s.inCombat() && hasAura(p, "Food");
    const bool drinking = !s.inCombat() && hasAura(p, "Drink");

    if (eating != eating_)
    {
        (eating ? profile_.restHealth : profile_.restDoneHealth).add(p.healthPercent());
        eating_ = eating;
    }

    if (drinking != drinking_)
    {
        (drinking ? profile_.restMana : profile_.restDoneMana).add(p.powerPercentOf(PowerType::Mana));
        drinking_ = drinking;
    }
}

namespace
{

// Fewer than this and the spread is a handful of chances.
constexpr std::uint32_t kHabit = 5;
// Running this long with a kind of move seen less than once in ten minutes: not the player's way.
constexpr double kNeverAfter = 300.0;
constexpr double kNeverEvery = 600.0;

enum class Habit
{
    Unknown,
    Never,
    Known
};

Habit habitOf(const PlaystyleProfile& profile, const PlaystyleSpread& every)
{
    if (every.count() >= kHabit)
    {
        return Habit::Known;
    }

    return profile.travelSeconds >= kNeverAfter && static_cast<double>(every.count()) * kNeverEvery < profile.travelSeconds ? Habit::Never : Habit::Unknown;
}

// The range most of the spread lies in, no narrower than a fifth of its low end.
std::pair<Duration, Duration> rangeOf(const PlaystyleSpread& spread, double least, double most)
{
    const double low = std::clamp(spread.quantile(0.15), least, most);
    const double high = std::clamp(spread.quantile(0.85), low * 1.2, std::max(most, low * 1.2));
    return {seconds(low), seconds(high)};
}

} // namespace

std::vector<std::string> applyPlaystyle(const PlaystyleProfile& profile, HumanizerConfig& humanizer)
{
    std::vector<std::string> said;

    if (!profile.movementReady())
    {
        return said;
    }

    const auto set = [&](std::string_view what, const PlaystyleSpread& every, bool& on, Duration& minInterval, Duration& maxInterval)
    {
        switch (habitOf(profile, every))
        {
        case Habit::Known:
            on = true;
            std::tie(minInterval, maxInterval) = rangeOf(every, 2.0, 900.0);
            said.push_back(std::format("a {} every {:.0f}-{:.0f} s", what, toSeconds(minInterval), toSeconds(maxInterval)));
            return true;
        case Habit::Never:
            on = false;
            said.push_back(std::format("no {}s", what));
            return false;
        case Habit::Unknown:
            break;
        }

        return false;
    };

    set("jump", profile.jumpEvery, humanizer.jumps, humanizer.jumpMinInterval, humanizer.jumpMaxInterval);

    if (set("sidestep", profile.strafeEvery, humanizer.strafes, humanizer.strafeMinInterval, humanizer.strafeMaxInterval))
    {
        std::tie(humanizer.strafeMinDuration, humanizer.strafeMaxDuration) = rangeOf(profile.strafeFor, 0.15, 3.0);
    }

    if (set("stop", profile.pauseEvery, humanizer.pauses, humanizer.pauseMinInterval, humanizer.pauseMaxInterval))
    {
        std::tie(humanizer.pauseMinDuration, humanizer.pauseMaxDuration) = rangeOf(profile.pauseFor, 0.4, 10.0);
    }

    // The player's own habits are used whether or not the settings had any on.
    if (!said.empty())
    {
        humanizer.enabled = humanizer.jumps || humanizer.strafes || humanizer.pauses;
    }

    return said;
}

PlaystyleTravel travelOf(const PlaystyleProfile& profile)
{
    // Five minutes of running looked at, and a score of walls passed.
    constexpr std::uint32_t kGroundKnown = 600;
    constexpr std::uint32_t kWallsKnown = 20;
    PlaystyleTravel travel;

    if (!profile.movementReady())
    {
        return travel;
    }

    if (profile.groundSamples >= kGroundKnown)
    {
        const double share = static_cast<double>(profile.roadSamples) / static_cast<double>(profile.groundSamples);

        // In between says more of where the roads were than of the player.
        if (share >= 0.5)
        {
            travel.useRoads = true;
            travel.said.push_back(std::format("on the roads ({:.0f}% of the way)", share * 100.0));
        }
        else if (share <= 0.15)
        {
            travel.useRoads = false;
            travel.said.push_back(std::format("off the roads ({:.0f}% of the way on one)", share * 100.0));
        }
    }

    if (profile.wallPass.count() >= kWallsKnown)
    {
        travel.wallClearance = static_cast<float>(std::clamp(profile.wallPass.quantile(0.5), 0.5, 6.0));
        travel.said.push_back(std::format("{:.1f} yd wide of walls", *travel.wallClearance));
    }

    return travel;
}

std::optional<PlaystyleProfile> readTrained(std::string_view ogc)
{
    std::string json;
    eachLine(ogc, [&](std::string_view line)
    {
        if (line.starts_with(kTrainedMarker))
        {
            json += line.substr(kTrainedMarker.size());
        }
    });

    if (json.empty())
    {
        return std::nullopt;
    }

    try
    {
        return PlaystyleProfile::fromJson(Json::parse(json));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::string withoutTrained(std::string_view ogc)
{
    std::string out;
    eachLine(ogc, [&](std::string_view line)
    {
        if (!line.starts_with(kTrainedMarker))
        {
            out += line;
            out += '\n';
        }
    });

    // The blank lines that set the profile apart go with it.
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
    {
        out.pop_back();
    }

    return out;
}

std::string trainedLines(std::string_view ogc)
{
    std::string out;
    eachLine(ogc, [&](std::string_view line)
    {
        if (line.starts_with(kTrainedMarker))
        {
            out += line;
            out += '\n';
        }
    });
    return out;
}

std::string withTrainedLines(std::string_view script, std::string_view lines)
{
    std::string out = withoutTrained(script);

    if (lines.empty())
    {
        return out;
    }

    if (!out.empty() && out.back() != '\n')
    {
        out += '\n';
    }

    out += '\n';
    out += lines;

    if (out.back() != '\n')
    {
        out += '\n';
    }

    return out;
}

std::string withTrained(std::string_view ogc, const PlaystyleProfile& profile)
{
    return withTrainedLines(ogc, std::string(kTrainedMarker) + profile.toJson().dump());
}

} // namespace og
