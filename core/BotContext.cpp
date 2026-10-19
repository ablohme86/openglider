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
 *  File: BotContext.cpp
 *
 *  Desc:
 *  Implementation of BotContext (core bot engine, behaviors, context,
 *  and objective tracking).
 *
 *
 */
#include "og/core/BotContext.h"
#include "og/core/GatheringData.h"
#include "og/common/Camera.h"
#include "og/navigation/Avoidance.h"
#include "og/state/IGameStateProvider.h"
#include "og/actions/ActionBinding.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace og
{

bool BotContext::needsRest(const GameSnapshot& s) const
{
    if (!combatClass)
    {
        return false;
    }

    const auto& cs = combatClass->settings();

    if (s.player.healthPercent() < cs.restHealth)
    {
        return true;
    }

    return cs.usesMana && s.player.maxPowerOf(PowerType::Mana) > 0 && s.player.manaPercent() < cs.restMana;
}

bool BotContext::standUp(const GameSnapshot& s)
{
    if (!s.player.isSitting)
    {
        standingUpSince.reset();
        return false;
    }

    const TimePoint t = now();

    if (!standingUpSince)
    {
        standingUpSince = t;
    }

    if (t - *standingUpSince > Duration(3000))
    {
        return false; // still "sitting": go on regardless
    }

    if (lastStandUp != TimePoint{} && t - lastStandUp < Duration(1000))
    {
        return true;
    }

    lastStandUp = t;
    log.info(logcat::Rest, "standing up");

    if (provider && provider->supportsLua() && provider->luaIdle() && provider->executeLua("DoEmote('STAND')"))
    {
        return true;
    }

    actions.execute(actions::Jump); // any movement stands a character up
    return true;
}

bool BotContext::restComplete(const GameSnapshot& s) const
{
    if (!combatClass)
    {
        return true;
    }

    const auto& cs = combatClass->settings();

    if (s.player.healthPercent() < cs.restDoneHealth)
    {
        return false;
    }

    return !cs.usesMana || s.player.maxPowerOf(PowerType::Mana) == 0 || s.player.manaPercent() >= cs.restDoneMana;
}

bool BotContext::avoidPacked(const GameSnapshot& s, const UnitState& unit)
{
    if (!targets.packed(unit, s) || targets.packAllGrey(unit, s))
    {
        return false;
    }

    const auto pack = targets.packAround(unit, s);
    constexpr Duration kPackBlacklist{300000};
    targetBlacklist.add(unit.guid, kPackBlacklist, "packed");

    for (const UnitState* other : pack)
    {
        targetBlacklist.add(other->guid, kPackBlacklist, "in a pack");
    }

    ++stats.blacklisted;
    log.info(logcat::Target, "{} {} has {} hostile mobs within {:.0f} yd - leaving it and them alone for 5 minutes", unit.name, formatGuid(unit.guid), pack.size(), targets.config().addRadius);
    return true;
}

bool BotContext::questNeeds(std::uint32_t entry) const
{
    return entry != 0 && config.quest.enabled && std::any_of(questTargets.begin(), questTargets.end(), [&](const QuestTarget& q) { return !q.done && std::find(q.mobs.begin(), q.mobs.end(), entry) != q.mobs.end(); });
}

bool BotContext::hasQuestLootNearby(const GameSnapshot& s, float maxDistance) const
{
    if (!lootEnabled() || !config.quest.enabled || s.isDead())
    {
        return false;
    }

    for (const auto& p : pendingLoot)
    {
        if (p.skinOnly)
        {
            continue;
        }

        const UnitState* u = s.findUnit(p.guid);

        if (u && !u->isTappedByOther && (u->isLootable || config.lootWithoutFlag) && questNeeds(u->entry) && distance(s.player.position, u->position) <= maxDistance)
        {
            return true;
        }
    }

    for (const auto& u : s.units)
    {
        if (u.isAlive() || u.isPlayer || u.isCritter || u.isTappedByOther || u.entry == 0 || !u.isLootable)
        {
            continue;
        }

        if (questNeeds(u.entry) && distance(s.player.position, u.position) <= maxDistance)
        {
            return true;
        }
    }

    return false;
}

const FastRespawn* BotContext::fastRespawnAt(const Vec3& at, std::uint32_t entry) const
{
    for (const FastRespawn& f : fastRespawns)
    {
        if ((entry == 0 || f.entry == entry) && now() < f.until && distance2D(f.at, at) <= FastRespawn::kRadius)
        {
            return &f;
        }
    }

    return nullptr;
}

std::optional<AvoidZone> BotContext::dangerAt(const Vec3& point) const
{
    for (const AvoidZone& z : packZones)
    {
        if (!z.ignored && distance2D(point, z.center) <= z.radius)
        {
            return z;
        }
    }

    if (config.avoidStrongMobs)
    {
        for (const AvoidZone& z : strongMobs.zones(0.0f, combat.engaged))
        {
            if (distance2D(point, z.center) <= z.radius)
            {
                return z;
            }
        }
    }

    return std::nullopt;
}

std::optional<AvoidZone> BotContext::wayThroughDanger(const GameSnapshot& s, const UnitState& unit) const
{
    const Path& path = navigator.path();
    const auto& destination = navigator.destination();

    if (path.points.empty() || !destination || distance2D(*destination, unit.position) > 5.0f)
    {
        return std::nullopt;
    }

    std::vector<AvoidZone> zones;
    for (const auto& z : packZones)
    {
        if (!z.ignored)
        {
            zones.push_back(z);
        }
    }

    if (config.avoidStrongMobs)
    {
        auto strong = strongMobs.zones(0.0f, combat.engaged);
        zones.insert(zones.end(), strong.begin(), strong.end());
    }

    // Not one the character or the mob stands in: walked out of, or not
    // pulled at all (TargetSelector's danger zones).
    std::erase_if(zones, [&](const AvoidZone& z) { return distance2D(z.center, s.player.position) <= z.radius || distance2D(z.center, unit.position) <= z.radius; });
    Vec3 previous = s.player.position;

    for (std::size_t i = navigator.pathIndex(); i < path.points.size(); ++i)
    {
        for (const AvoidZone& z : zones)
        {
            if (!clearOfZones(previous, path.points[i], std::span(&z, 1)))
            {
                return z;
            }
        }

        previous = path.points[i];
    }

    return std::nullopt;
}

const UnitState* BotContext::joinsTheFight(const GameSnapshot& s, const UnitState& unit, float combatRange) const
{
    if (config.maxPackAdds <= 0)
    {
        return nullptr;
    }

    const float away = distance2D(s.player.position, unit.position);
    const Vec3 spot = away > combatRange && away > 0.1f ? unit.position + (s.player.position - unit.position) * (combatRange / away) : s.player.position;
    const UnitState* nearest = nullptr;
    float best = 0;
    int joining = 0;

    for (const UnitState& u : s.units)
    {
        if (u.guid == unit.guid || !u.isAlive() || u.isPlayer || u.isCritter || u.ownerGuid != 0 || u.reaction != Reaction::Hostile || u.inCombat || u.isTappedByOther || !u.isAttackable || u.name.starts_with("[DNT]"))
        {
            continue;
        }

        const float d = distance2D(spot, u.position);

        if (d > TargetSelector::aggroRadius(u.level > 0 ? u.level : s.player.level + 10, s.player.level, config.aggroRadiusBase))
        {
            continue;
        }

        ++joining;

        if (!nearest || d < best)
        {
            nearest = &u;
            best = d;
        }
    }

    // As many as the setting allows are taken on.
    return joining > config.maxPackAdds ? nearest : nullptr;
}

bool BotContext::isSafeToRest(const GameSnapshot& s) const
{
    return std::none_of(s.units.begin(), s.units.end(), [&](const UnitState& u) { return u.isAlive() && u.reaction == Reaction::Hostile && (u.targetGuid == s.player.guid || s.isAttacker(u)); });
}

bool BotContext::corpseGuarded(const GameSnapshot& s, const Vec3& corpse) const
{
    return guardOf(s, corpse) != nullptr;
}

const UnitState* BotContext::guardOf(const GameSnapshot& s, const Vec3& corpse) const
{
    if (config.lootDangerRadius <= 0.0f)
    {
        return nullptr;
    }

    const UnitState* nearest = nullptr;

    for (const UnitState* u : s.hostilesNear(corpse, config.lootDangerRadius))
    {
        // Busy with another player (a starting zone is full of them): it will
        // not turn on someone looting. Counting those, nearly every corpse in
        // a kobold camp was guarded and left for the next pull and the next.
        if (u->isTappedByOther || (u->inCombat && u->targetGuid != 0 && u->targetGuid != s.player.guid))
        {
            continue;
        }

        if (!nearest || distance(u->position, corpse) < distance(nearest->position, corpse))
        {
            nearest = u;
        }
    }

    return nearest;
}

bool BotContext::canSkin(const UnitState& unit, const PlayerState& player) const
{
    return skinEnabled() && canSkinUnit(unit, player);
}

bool BotContext::canRunScripts() const
{
    return provider && provider->supportsLua();
}

bool BotContext::scriptIdle() const
{
    return !provider || provider->luaIdle();
}

bool BotContext::runScript(std::string_view lua)
{
    return canRunScripts() && provider->executeLua(lua);
}

void BotContext::blacklistTarget(Guid guid, std::string_view name, const std::string& reason, std::optional<Duration> duration)
{
    const Duration expiry = duration.value_or(config.blacklistDuration);
    targetBlacklist.add(guid, expiry, reason);
    ++stats.blacklisted;
    log.warn(logcat::Target, "blacklisted {} {} for {:.0f}s: {}", name, formatGuid(guid), toSeconds(expiry), reason);

    if (combat.engaged == guid)
    {
        combat.engaged = 0;
    }
}

bool BotContext::setTarget(Guid guid)
{
    return provider ? provider->setTarget(guid) : false;
}

bool BotContext::clickTarget(const UnitState& unit, MouseButton button)
{
    if (!provider)
    {
        return false;
    }

    // Aim at torso height (approx 1 yard above base position)
    const Vec3 targetPoint = unit.position + Vec3{0.0f, 0.0f, 1.0f};
    const auto screen = provider->worldToScreen(targetPoint);

    if (!screen)
    {
        return false;
    }

    actions.mouseMove(screen->x, screen->y);
    actions.click(button);
    log.info(logcat::Target, "clicked {} {} at screen ({}, {})", unit.name, formatGuid(unit.guid), screen->x, screen->y);
    return true;
}

bool BotContext::uiFrameVisible(std::string_view name)
{
    if (!provider)
    {
        return false;
    }

    const auto frame = provider->uiFrame(name);
    return frame && frame->visible;
}

bool BotContext::clickUiFrame(std::string_view name, MouseButton button)
{
    // Why a click was not made, now and then: a Release Spirit that never
    // came on a Windows client said nothing of it.
    const auto notClicked = [&](std::string_view why)
    {
        if (now() - clickNotMadeSaidAt_ >= Duration(5000))
        {
            clickNotMadeSaidAt_ = now();
            log.warn(logcat::Input, "not clicking {}: {}", name, why);
        }

        return false;
    };

    // Held back just after a chat line: a click now would be dropped.
    if (!provider || !actions.mouseFree())
    {
        return false;
    }

    const auto frame = provider->uiFrame(name);

    if (!frame || !frame->visible)
    {
        return notClicked("it is not shown");
    }

    const auto size = actions.windowSize();

    if (!size)
    {
        return notClicked("the game window's size is not known");
    }

    const int x = static_cast<int>(frame->centerX() * static_cast<float>(size->first) + 0.5f);
    const int y = static_cast<int>(frame->centerY() * static_cast<float>(size->second) + 0.5f);
    actions.mouseMove(x, y);
    actions.click(button);
    log.info(logcat::Input, "clicked {} at ({}, {})", name, x, y);
    return true;
}

std::optional<std::string> BotContext::uiText(std::string_view frame, std::string_view fontString)
{
    return provider ? provider->uiText(frame, fontString) : std::nullopt;
}

namespace
{

bool containsNoCase(std::string_view text, std::string_view word)
{
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::search(text.begin(), text.end(), word.begin(), word.end(), [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

} // namespace

bool BotContext::clickDialog(std::string_view textHolds, std::string_view buttonReads)
{
    for (int i = 1; i <= 4; ++i)
    {
        const std::string popup = "StaticPopup" + std::to_string(i);

        if (!uiFrameVisible(popup))
        {
            continue;
        }

        const bool byText = !textHolds.empty() && containsNoCase(uiText(popup, popup + "Text").value_or(""), textHolds);
        const bool byButton = !buttonReads.empty() && [&]
        {
            const auto label = uiText(popup + "Button1", popup + "Button1Text");
            return label && label->size() == buttonReads.size() && containsNoCase(*label, buttonReads);
        }();

        if (byText || byButton)
        {
            return clickUiFrame(popup + "Button1");
        }
    }

    return false;
}

namespace
{

// Where the mouse looks, as fractions of the window, nearest the spot a corpse
// or herb right in front of the character shows first: below the middle.
const std::vector<std::pair<float, float>>& seekSpots()
{
    static const auto spots = []
    {
        std::vector<std::pair<float, float>> out;

        for (int y = 30; y <= 92; y += 4)
        {
            for (int x = 20; x <= 80; x += 4)
            {
                out.emplace_back(x / 100.0f, y / 100.0f);
            }
        }

        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b)
                  {
                      const auto d = [](const auto& p) { return std::hypot(p.first - 0.5f, (p.second - 0.6f) * 1.3f); };
                      return d(a) < d(b);
                  });
        return out;
    }();
    return spots;
}

} // namespace

BotContext::SeekSpots BotContext::buildTargetSeekSpots(const PlayerState& player, const Vec3& targetPos, float zOffset) const
{
    SeekSpots spots;

    const auto cam = provider ? provider->camera() : std::nullopt;
    const auto size = actions.windowSize();

    if (cam && size && size->first > 0 && size->second > 0)
    {
        std::vector<Vec3> testPoints = {targetPos};
        if (std::abs(zOffset) > 0.01f)
        {
            testPoints.push_back(targetPos + Vec3{0.0f, 0.0f, zOffset});
        }

        for (const int radius : {0, 4, 8, 12, 16, 24, 32, 48})
        {
            for (const auto& wp : testPoints)
            {
                if (const auto point = cam->worldToScreen(wp, size->first, size->second))
                {
                    if (radius == 0)
                    {
                        const float sx = std::clamp(static_cast<float>(point->x) / static_cast<float>(size->first), 0.0f, 0.999f);
                        const float sy = std::clamp(static_cast<float>(point->y) / static_cast<float>(size->second), 0.0f, 0.999f);
                        spots.emplace_back(sx, sy);
                    }
                    else
                    {
                        for (int dy = -radius; dy <= radius; dy += radius)
                        {
                            for (int dx = -radius; dx <= radius; dx += radius)
                            {
                                if (dx == 0 && dy == 0)
                                {
                                    continue;
                                }
                                const float sx = std::clamp(static_cast<float>(point->x + dx) / static_cast<float>(size->first), 0.0f, 0.999f);
                                const float sy = std::clamp(static_cast<float>(point->y + dy) / static_cast<float>(size->second), 0.0f, 0.999f);
                                spots.emplace_back(sx, sy);
                            }
                        }
                    }
                }
            }
        }
    }

    const float angle = std::clamp(headingDelta(player.heading, player.position, targetPos), -1.2f, 1.2f);
    const float across = std::clamp(0.5f - std::tan(angle) * 0.5f, 0.1f, 0.9f);
    SeekSpots directional;

    for (int dx = -4; dx <= 4; ++dx)
    {
        for (int y = 0; y <= 36; ++y)
        {
            directional.emplace_back(std::clamp(across + static_cast<float>(dx) * 0.015f, 0.02f, 0.98f), 0.25f + static_cast<float>(y) * 0.0135f);
        }
    }

    std::stable_sort(directional.begin(), directional.end(),
                     [&](const auto& a, const auto& b)
                     {
                         const auto d = [&](const auto& p) { return std::abs(p.first - across) * 3.0f + std::abs(p.second - 0.5f); };
                         return d(a) < d(b);
                     });

    spots.insert(spots.end(), directional.begin(), directional.end());

    SeekSpots filtered;
    filtered.reserve(spots.size());
    for (const auto& sp : spots)
    {
        if (filtered.empty() || filtered.back() != sp)
        {
            filtered.push_back(sp);
        }
    }

    return filtered;
}

BotContext::MouseSeek BotContext::seekUnderMouse(Guid guid, const SeekSpots* custom)
{
    if (!provider || guid == 0)
    {
        return MouseSeek::Unsupported;
    }

    const auto under = provider->mouseoverGuid();
    const auto size = under ? actions.windowSize() : std::nullopt;

    if (!under || !size)
    {
        return MouseSeek::Unsupported;
    }

    if (guid != seekGuid_)
    {
        seekGuid_ = guid;
        seekIndex_ = 0;
    }

    // Only where the seek put the mouse: over a window just clicked in (a
    // quest dialog) the client still names the unit last under it, and a
    // right-click there lands on the UI or empty ground, not on the NPC.
    if (*under == guid && seekIndex_ > 0)
    {
        return MouseSeek::Found;
    }

    if (!actions.mouseFree())
    {
        return MouseSeek::Searching;
    }

    const auto& spots = custom ? *custom : seekSpots();

    if (seekIndex_ >= spots.size())
    {
        return MouseSeek::NotFound;
    }

    const auto [x, y] = spots[seekIndex_++];
    // The client says what is under the mouse from its next frame on: looked
    // at on the next tick.
    actions.mouseMove(static_cast<int>(x * static_cast<float>(size->first)), static_cast<int>(y * static_cast<float>(size->second)));
    return MouseSeek::Searching;
}

BotContext::MouseSeek BotContext::seekUnderMouse(Guid guid, const PlayerState& player, const Vec3& targetPos, float zOffset)
{
    if (guid != seekGuid_ || targetSeekSpots_.empty())
    {
        targetSeekSpots_ = buildTargetSeekSpots(player, targetPos, zOffset);
    }
    return seekUnderMouse(guid, &targetSeekSpots_);
}

namespace
{

constexpr int kTurnSteps = 8;
// Left alone this long, the same corpse or node is a new try.
constexpr Duration kTurnForgetAfter{30000};
constexpr Duration kTurnStepWait{3000};

// About the character, where something within reach of it shows whichever
// way it faces: the nearest of the seek's spots.
const BotContext::SeekSpots& turnSpots()
{
    static const BotContext::SeekSpots spots(seekSpots().begin(), seekSpots().begin() + std::min<std::size_t>(48, seekSpots().size()));
    return spots;
}

} // namespace

BotContext::MouseSeek BotContext::seekTurningRound(const PlayerState& player, Guid guid, std::optional<Vec3> targetPos, float zOffset)
{
    const TimePoint at = now();

    if (guid != turnGuid_ || at - turnLastSeek_ > kTurnForgetAfter)
    {
        turnGuid_ = guid;
        turnFirstSeek_ = at;
        turnStep_ = 0;
        turnSpotsCache_.clear();
        turnZoomTried_ = false;
        turnTargetPos_ = targetPos;
        turnZOffset_ = zOffset;
    }

    if (targetPos)
    {
        turnTargetPos_ = targetPos;
        turnZOffset_ = zOffset;
    }

    turnLastSeek_ = at;

    if (turnStep_ >= kTurnSteps)
    {
        return MouseSeek::NotFound;
    }

    if (turnStep_ == 0)
    {
        // The camera on its way in: looked for again once it is there.
        if (cameraZoomedIn_ && at < cameraSettledAt_)
        {
            return MouseSeek::Searching;
        }

        const SeekSpots* spotsPtr = nullptr;
        if (turnTargetPos_)
        {
            if (turnSpotsCache_.empty())
            {
                turnSpotsCache_ = buildTargetSeekSpots(player, *turnTargetPos_, turnZOffset_);
            }
            spotsPtr = &turnSpotsCache_;
        }

        const auto seek = seekUnderMouse(guid, spotsPtr);

        if (seek == MouseSeek::Unsupported || (seek != MouseSeek::NotFound && at - turnFirstSeek_ < config.turnRoundAfter))
        {
            return seek;
        }

        // Zoomed in first, and the same while straight on again before turning;
        // that no good either, back out to turn round.
        if (!std::exchange(turnZoomTried_, true) && zoomInBeforeTurning())
        {
            turnFirstSeek_ = at;
            turnSpotsCache_.clear();
            return MouseSeek::Searching;
        }

        zoomOutToTurn();
        turnFrom_ = player.heading;
        turnStep_ = 1;
        turnStepSince_ = at;
        stopMouseSeek();
        log.info(logcat::Input, "not got hold of it in {:.0f} s - turning round once to find it under the mouse", toSeconds(at - turnFirstSeek_));
    }

    // The camera on its way out: the turn's wait runs from when it is there.
    if (at < cameraSettledAt_)
    {
        turnStepSince_ = at;
        return MouseSeek::Searching;
    }

    // A turn that does not come about (the keys held back) is not waited on:
    // the mouse looks from where the character got to.
    if (at - turnStepSince_ > kTurnStepWait)
    {
        movement.stopTurning();
    }
    else if (!movement.faceHeading(player, normalizeAngle(turnFrom_ + static_cast<float>(turnStep_) * kTwoPi / static_cast<float>(kTurnSteps)), 0.15f))
    {
        return MouseSeek::Searching;
    }

    const auto seek = seekUnderMouse(guid, &turnSpots());

    if (seek != MouseSeek::NotFound)
    {
        return seek;
    }

    stopMouseSeek();
    turnStepSince_ = at;

    if (++turnStep_ >= kTurnSteps)
    {
        movement.stopTurning();
        log.info(logcat::Input, "turned right round without it coming under the mouse - giving it up");
        return MouseSeek::NotFound;
    }

    return MouseSeek::Searching;
}

namespace
{

// Either way of facing the NPC, a little and then a little more: never so far
// that it leaves the screen.
constexpr std::array<float, BotContext::kNpcTurnSteps> kNpcTurns{0.35f, -0.35f, 0.7f, -0.7f};

} // namespace

BotContext::MouseSeek BotContext::seekNpcTurning(const PlayerState& player, Guid guid, const Vec3& npcPos, float zOffset)
{
    const TimePoint at = now();

    if (guid != npcTurnGuid_ || at - npcTurnLastSeek_ > kTurnForgetAfter)
    {
        npcTurnGuid_ = guid;
        npcTurnStep_ = 0;
        npcTurnLooking_ = false;
        npcTurnZoomTried_ = false;
    }

    npcTurnLastSeek_ = at;

    if (npcTurnStep_ > static_cast<int>(kNpcTurns.size()))
    {
        return MouseSeek::NotFound;
    }

    // The camera on its way in or out: looked for again once it is there.
    if (at < cameraSettledAt_)
    {
        npcTurnStepSince_ = at;
        return MouseSeek::Searching;
    }

    if (npcTurnStep_ > 0 && !npcTurnLooking_)
    {
        // A turn that does not come about is not waited on: the mouse looks
        // from where the character got to.
        if (at - npcTurnStepSince_ > kTurnStepWait)
        {
            movement.stopTurning();
        }
        else if (!movement.faceHeading(player, normalizeAngle(headingTo(player.position, npcPos) + kNpcTurns[npcTurnStep_ - 1]), 0.15f))
        {
            return MouseSeek::Searching;
        }

        // The spots anew, where the NPC shows from here.
        stopMouseSeek();
        npcTurnLooking_ = true;
    }

    const auto seek = seekUnderMouse(guid, player, npcPos, zOffset);

    if (seek == MouseSeek::Found)
    {
        npcTurnGuid_ = 0;
        npcTurnStep_ = 0;
        return seek;
    }

    if (seek != MouseSeek::NotFound)
    {
        return seek;
    }

    stopMouseSeek();

    // Zoomed in first, and looked for straight on again before turning; that
    // no good either, back out to turn.
    if (npcTurnStep_ == 0)
    {
        if (!std::exchange(npcTurnZoomTried_, true) && zoomInBeforeTurning())
        {
            return MouseSeek::Searching;
        }

        zoomOutToTurn();
    }

    npcTurnLooking_ = false;
    npcTurnStepSince_ = at;

    if (++npcTurnStep_ > static_cast<int>(kNpcTurns.size()))
    {
        movement.stopTurning();
        log.info(logcat::Input, "turned a little either way without the NPC coming under the mouse - giving it up");
        return MouseSeek::NotFound;
    }

    log.info(logcat::Input, "the NPC is not under the mouse - turning a little to find it ({} of {})", npcTurnStep_, kNpcTurns.size());
    return MouseSeek::Searching;
}

namespace
{

// The client keeps how far out the camera was (_cz) and goes back there:
// GetCameraZoom is in every client that says what is under the mouse.
constexpr std::string_view kCameraIn = "if GetCameraZoom then _cz=GetCameraZoom() CameraZoomIn(50) end";
constexpr std::string_view kCameraBack = "if _cz then CameraZoomOut(_cz) _cz=nil end";

} // namespace

bool BotContext::zoomInForClick(int clicksMade)
{
    // Turning round goes with the camera out. Not one left off half way (a
    // fight, the NPC gone): that would hold the camera out for every corpse after.
    const TimePoint at = now();

    if ((turningRound(turnGuid_) && at - turnLastSeek_ <= kTurnForgetAfter) || (turningRound(npcTurnGuid_) && at - npcTurnLastSeek_ <= kTurnForgetAfter))
    {
        return false;
    }

    if (cameraZoomedIn_)
    {
        return now() < cameraSettledAt_;
    }

    if (config.zoomInAfterClicks <= 0 || clicksMade < config.zoomInAfterClicks || !provider || !provider->mouseoverGuid())
    {
        return false;
    }

    if (!(canRunScripts() ? runScript(kCameraIn) : actions.chatScript(kCameraIn)))
    {
        return false;
    }

    cameraZoomedIn_ = true;
    cameraSettledAt_ = now() + config.cameraZoomSettle;
    // What the mouse was over is somewhere else on the screen now.
    stopMouseSeek();
    log.info(logcat::Input, "{} right-click(s) did nothing - camera all the way in for the next", clicksMade);
    return true;
}

bool BotContext::zoomInBeforeTurning()
{
    if (cameraZoomedIn_ || config.zoomInAfterClicks <= 0 || !provider || !provider->mouseoverGuid())
    {
        return false;
    }

    if (!(canRunScripts() ? runScript(kCameraIn) : actions.chatScript(kCameraIn)))
    {
        return false;
    }

    cameraZoomedIn_ = true;
    cameraSettledAt_ = now() + config.cameraZoomSettle;
    stopMouseSeek();
    log.info(logcat::Input, "not got hold of it under the mouse - camera all the way in before turning round");
    return true;
}

void BotContext::zoomOutToTurn()
{
    if (!cameraZoomedIn_)
    {
        return;
    }

    restoreCamera();
    cameraSettledAt_ = now() + config.cameraZoomSettle;
    stopMouseSeek();
}

void BotContext::restoreCamera()
{
    if (!cameraZoomedIn_ || !(canRunScripts() ? runScript(kCameraBack) : actions.chatScript(kCameraBack)))
    {
        return;
    }

    cameraZoomedIn_ = false;
    log.info(logcat::Input, "camera back out");
}

bool BotContext::interactUnit(Guid guid, const Vec3& position)
{
    return provider ? provider->interactUnit(guid, position) : false;
}

bool BotContext::interactNpc(Guid guid, const Vec3& position)
{
    return provider ? provider->interactNpc(guid, position) : false;
}

void BotContext::cancelInteraction()
{
    if (provider)
    {
        provider->cancelInteraction();
    }
}

Duration BotContext::randomBetween(Duration low, Duration high)
{
    if (high <= low)
    {
        return low;
    }

    if (randomState == 0)
    {
        randomState = 0x9E3779B97F4A7C15ull;
    }

    randomState ^= randomState >> 12;
    randomState ^= randomState << 25;
    randomState ^= randomState >> 27;
    const std::uint64_t value = randomState * 0x2545F4914F6CDD1Dull;
    const auto span = static_cast<std::uint64_t>((high - low).count()) + 1;

    if (!config.humanizer.enabled)
    {
        return low + Duration(static_cast<Duration::rep>((value >> 11) % span));
    }

    // Squared, an even draw falls mostly low: half the waits in the first
    // quarter of the range.
    const double even = static_cast<double>(value >> 11) / static_cast<double>(1ull << 53);
    double wait = static_cast<double>((high - low).count()) * even * even;

    // One wait in sixteen goes on past the range, by up to as much again.
    if (((value >> 5) & 0xF) == 0)
    {
        wait += static_cast<double>((high - low).count()) * (0.25 + static_cast<double>((value >> 9) & 0xFF) / 340.0);
    }

    return std::max(low, Duration(static_cast<Duration::rep>((static_cast<double>(low.count()) + wait) * humanPace())));
}

namespace
{

// splitmix64: one well mixed number from another.
std::uint64_t mixed(std::uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

double share(std::uint64_t x)
{
    return static_cast<double>(mixed(x) >> 11) / static_cast<double>(1ull << 53);
}

} // namespace

double BotContext::humanPace()
{
    if (humanSeed == 0)
    {
        humanSeed = mixed(randomState) | 1;
    }

    return 0.85 + 0.45 * share(humanSeed);
}

float BotContext::humanReach(float range, std::uint64_t key)
{
    if (!config.humanizer.enabled)
    {
        return range;
    }

    humanPace(); // draws the seed
    return range * static_cast<float>(0.6 + 0.4 * share(humanSeed ^ mixed(key)));
}

} // namespace og
