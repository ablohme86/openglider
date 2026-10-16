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
 *  File: SimWorld.cpp
 *
 *  Desc:
 *  Implementation of SimWorld (deterministic world simulator for
 *  offline bot testing).
 *
 *
 */
#include "og/sim/SimWorld.h"

#include <algorithm>
#include <cmath>

namespace og
{

namespace
{

constexpr std::uint32_t kSpellRend = 772;
constexpr std::uint32_t kSpellFood = 433;
constexpr std::uint32_t kSpellDrink = 430;
constexpr std::uint32_t kSpellBattleShout = 6673;

bool endsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::uint8_t modifierBit(Key k)
{
    switch (k)
    {
    case Key::Shift:
        return ModShift;
    case Key::Control:
        return ModCtrl;
    case Key::Alt:
        return ModAlt;
    default:
        return ModNone;
    }
}

} // namespace

SimConfig simConfigFromProfile(const GliderProfile& profile, std::uint32_t seed, int obstacleCount)
{
    SimConfig c;
    c.seed = seed;
    std::mt19937 rng(seed);
    auto uniform = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    auto uniformInt = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    const auto& wp = profile.waypoints;

    if (!wp.empty())
    {
        c.playerStart = wp.front();
    }

    if (wp.size() >= 2)
    {
        c.playerHeading = headingTo(wp[0], wp[1]);
    }

    c.graveyard = !profile.ghostWaypoints.empty() ? profile.ghostWaypoints.front() : c.playerStart + Vec3{-90, 0, 0};

    const int minLevel = std::clamp(profile.minLevel, 1, 80);
    const int maxLevel = std::clamp(profile.maxLevel == 255 ? minLevel + 2 : profile.maxLevel, minLevel, 80);
    c.playerLevel = std::clamp((minLevel + maxLevel) / 2 + 1, 1, 80);
    c.playerMaxHealth = 60 + c.playerLevel * 24;
    c.minHit = 6 + c.playerLevel;
    c.maxHit = 12 + static_cast<int>(c.playerLevel * 1.4f);

    std::vector<std::string> names = profile.targets;

    if (names.empty())
    {
        names.push_back("Sim Mob");
    }

    const int count = std::max<int>(8, static_cast<int>(wp.size()) * 2);

    for (int i = 0; i < count && !wp.empty(); ++i)
    {
        Vec3 base = wp.front();
        Vec3 perp{0, 1, 0};

        if (wp.size() >= 2)
        {
            const std::size_t seg = static_cast<std::size_t>(uniformInt(0, static_cast<int>(wp.size()) - 1));
            const Vec3& a = wp[seg];
            const Vec3& b = wp[(seg + 1) % wp.size()];
            base = a + (b - a) * uniform(0.0f, 1.0f);
            const Vec3 d = b - a;
            const float len = std::max(0.001f, d.length2D());
            perp = {-d.y / len, d.x / len, 0.0f};
        }

        const float side = uniformInt(0, 1) ? 1.0f : -1.0f;
        SimSpawn spawn;
        spawn.position = base + perp * (side * uniform(12.0f, 28.0f));
        spawn.position.z = base.z;
        auto& m = spawn.mob;
        const std::size_t nameIndex = static_cast<std::size_t>(i) % names.size();
        m.name = names[nameIndex];
        m.entry = 1000 + static_cast<std::uint32_t>(nameIndex);
        m.level = uniformInt(minLevel, maxLevel);
        m.maxHealth = 40 + m.level * 14;
        m.minDamage = 1 + static_cast<int>(m.level * 0.55f);
        m.maxDamage = 2 + static_cast<int>(m.level * 0.9f);
        m.aggressive = i % 3 != 0;
        m.aggroRadius = 9.0f;
        c.spawns.push_back(std::move(spawn));
    }

    for (int k = 0; k < obstacleCount && wp.size() >= 2; ++k)
    {
        const std::size_t seg = static_cast<std::size_t>(k) % wp.size();
        const Vec3& a = wp[seg];
        const Vec3& b = wp[(seg + 1) % wp.size()];
        c.obstacles.push_back(SimObstacle{a + (b - a) * 0.5f, 2.5f, false});
    }

    return c;
}

SimWorld::SimWorld(SimConfig config, ActionMap bindings, Logger& log) : config_(std::move(config)), bindings_(std::move(bindings)), log_(log), rng_(config_.seed)
{
    player_.position = config_.playerStart;
    player_.heading = config_.playerHeading;
    player_.maxHealth = config_.playerMaxHealth;
    player_.health = player_.maxHealth;

    for (const auto& s : config_.spawns)
    {
        spawnMob(s.mob, s.position);
    }
}

int SimWorld::random(int lo, int hi)
{
    return std::uniform_int_distribution<int>(lo, std::max(lo, hi))(rng_);
}

SimMob& SimWorld::spawnMob(const SimMobTemplate& tpl, const Vec3& position)
{
    SimMob m;
    m.guid = nextGuid_++;
    m.tpl = tpl;
    m.spawn = position;
    m.position = position;
    m.heading = std::uniform_real_distribution<float>(0.0f, kTwoPi)(rng_);
    m.health = tpl.maxHealth;
    mobs_.push_back(std::move(m));
    return mobs_.back();
}

SimMob* SimWorld::findMob(Guid guid)
{
    for (auto& m : mobs_)
    {
        if (m.guid == guid && !m.despawned)
        {
            return &m;
        }
    }

    return nullptr;
}

SimMob* SimWorld::targetMob()
{
    return player_.target ? findMob(player_.target) : nullptr;
}

bool SimWorld::facing(const Vec3& pos) const
{
    return std::fabs(headingDelta(player_.heading, player_.position, pos)) < kPi / 2.0f;
}

bool SimWorld::onCooldown(const std::string& action) const
{
    auto it = player_.cooldowns.find(action);
    return it != player_.cooldowns.end() && now_ < it->second;
}

void SimWorld::setCooldown(const std::string& action, Duration d)
{
    player_.cooldowns[action] = now_ + d;
}

bool SimWorld::hasAura(const std::vector<SimAura>& auras, std::string_view name) const
{
    return std::any_of(auras.begin(), auras.end(), [&](const SimAura& a) { return a.name == name; });
}

void SimWorld::removeAura(std::vector<SimAura>& auras, std::string_view name)
{
    std::erase_if(auras, [&](const SimAura& a) { return a.name == name; });
}

bool SimWorld::playerInCombat() const
{
    if (player_.dead)
    {
        return false;
    }

    return std::any_of(mobs_.begin(), mobs_.end(), [](const SimMob& m) { return !m.dead && !m.despawned && m.mode == SimMob::Mode::Chasing; });
}

void SimWorld::onInput(const InputEvent& e)
{
    if (e.type == InputEvent::Type::KeyDown)
    {
        if (isModifierKey(e.key))
        {
            heldMods_ |= modifierBit(e.key);
            return;
        }

        const std::string prefix = std::string(toString(config_.playerClass)) + ".";
        const auto action = bindings_.actionForChord(KeyChord{e.key, heldMods_}, prefix);

        if (!action)
        {
            log_.trace(logcat::Sim, "unbound key {}", keyName(e.key));
            return;
        }

        if (action->starts_with("Move.") && *action != actions::Jump)
        {
            heldMoves_.insert(*action);
            heldKeyActions_[e.key].push_back(*action);
            return;
        }

        performAction(*action);
    }
    else if (e.type == InputEvent::Type::KeyUp)
    {
        if (isModifierKey(e.key))
        {
            heldMods_ &= static_cast<std::uint8_t>(~modifierBit(e.key));
            return;
        }

        if (auto it = heldKeyActions_.find(e.key); it != heldKeyActions_.end())
        {
            for (const auto& a : it->second)
            {
                heldMoves_.erase(a);
            }

            heldKeyActions_.erase(it);
        }
    }
}

void SimWorld::standUp()
{
    player_.sitting = false;
    removeAura(player_.auras, "Food");
    removeAura(player_.auras, "Drink");
}

void SimWorld::cancelCast()
{
    player_.castEnd = TimePoint{};
    player_.castTarget = 0;
}

void SimWorld::performAction(const std::string& name)
{
    ++counters_.actionsPerformed;
    log_.trace(logcat::Sim, "action {}", name);

    if (name == actions::TargetNext)
    {
        if (player_.dead)
        {
            return;
        }

        std::vector<std::pair<float, Guid>> candidates;

        for (const auto& m : mobs_)
        {
            if (m.dead || m.despawned)
            {
                continue;
            }

            const float d = distance(m.position, player_.position);

            if (d > 40.0f)
            {
                continue;
            }

            if (std::fabs(headingDelta(player_.heading, player_.position, m.position)) > 1.2f)
            {
                continue;
            }

            candidates.emplace_back(d, m.guid);
        }

        if (candidates.empty())
        {
            return;
        }

        std::sort(candidates.begin(), candidates.end());
        auto it = std::find_if(candidates.begin(), candidates.end(), [&](const auto& c) { return c.second == player_.target; });

        if (it == candidates.end() || std::next(it) == candidates.end())
        {
            player_.target = candidates.front().second;
        }
        else
        {
            player_.target = std::next(it)->second;
        }

        return;
    }

    if (name == actions::TargetClear)
    {
        player_.target = 0;
        return;
    }

    if (name == actions::Jump)
    {
        player_.jumpUntil = now_ + Duration(700);
        return;
    }

    if (name == actions::ReleaseSpirit)
    {
        if (player_.dead && !player_.ghost)
        {
            player_.ghost = true;
            player_.position = config_.graveyard;
            player_.health = 1;
            player_.releasedAt = now_;
            player_.target = 0;
            log_.debug(logcat::Sim, "player released spirit, ghost at graveyard {}", config_.graveyard);
        }

        return;
    }

    if (name == actions::RetrieveCorpse)
    {
        if (player_.ghost && player_.corpse && distance2D(player_.position, *player_.corpse) <= 39.0f && now_ - player_.releasedAt >= Duration(2000))
        {
            player_.dead = false;
            player_.ghost = false;
            player_.health = player_.maxHealth / 2;
            player_.rage = 0;
            player_.corpse.reset();
            ++counters_.resurrections;
            log_.debug(logcat::Sim, "player resurrected at {}", player_.position);
        }

        return;
    }

    if (player_.dead)
    {
        return; // everything below requires being alive
    }

    if (name == actions::Interact)
    {
        SimMob* best = nullptr;
        float bestDist = 5.0f;

        for (auto& m : mobs_)
        {
            if (!m.dead || m.despawned || !(m.lootable || m.skinnable))
            {
                continue;
            }

            const float d = distance(m.position, player_.position);

            // Interact With Target uses the selected corpse before soft targeting.
            if (m.guid == player_.target && d <= 5.0f)
            {
                best = &m;
                break;
            }

            if (d <= bestDist)
            {
                best = &m;
                bestDist = d;
            }
        }

        if (!best)
        {
            return;
        }

        if (best->lootable)
        {
            best->lootable = false;
            ++counters_.corpsesLooted;
            log_.debug(logcat::Sim, "looted {}", best->tpl.name);
        }
        else if (best->skinnable && player_.castEnd == TimePoint{})
        {
            player_.castEnd = now_ + Duration(1500);
            player_.castTarget = best->guid;
        }

        return;
    }

    SimMob* target = targetMob();
    const bool validTarget = target && !target->dead;
    const float targetDist = validTarget ? distance(player_.position, target->position) : 1e9f;
    const bool inMelee = validTarget && targetDist <= config_.meleeRange && facing(target->position);

    if (endsWith(name, ".AutoAttack"))
    {
        if (validTarget)
        {
            player_.autoAttack = true; // like /startattack: never toggles off
        }

        return;
    }

    if (name == "Warrior.Charge")
    {
        if (validTarget && !playerInCombat() && !onCooldown(name) && targetDist >= 8.0f && targetDist <= 25.0f && !lineBlocked(player_.position, target->position))
        {
            const Vec3 d = target->position - player_.position;
            const float len = std::max(0.001f, d.length2D());
            player_.position = target->position - Vec3{d.x / len, d.y / len, 0.0f} * 2.0f;
            player_.heading = headingTo(player_.position, target->position);
            player_.rage = std::min(100, player_.rage + 15);
            player_.autoAttack = true;
            target->stunnedUntil = now_ + Duration(1500);
            target->tappedByPlayer = true;
            aggro(*target);
            setCooldown(name, Duration(15000));
        }

        return;
    }

    if (name == "Warrior.Rend")
    {
        if (inMelee && player_.rage >= 10)
        {
            player_.rage -= 10;
            removeAura(target->auras, "Rend");
            target->auras.push_back(SimAura{kSpellRend, "Rend", true, now_ + Duration(15000), player_.guid, Duration(3000), now_ + Duration(3000), random(7, 10)});
            target->tappedByPlayer = true;
            aggro(*target);
        }

        return;
    }

    if (name == "Warrior.HeroicStrike")
    {
        if (inMelee && player_.rage >= 15)
        {
            player_.rage -= 15;
            damageMob(*target, random(config_.minHit, config_.maxHit) + 12, "Heroic Strike");
        }

        return;
    }

    if (name == "Warrior.BattleShout")
    {
        if (player_.rage >= 10)
        {
            player_.rage -= 10;
            removeAura(player_.auras, "Battle Shout");
            player_.auras.push_back(SimAura{kSpellBattleShout, "Battle Shout", false, now_ + Duration(120000)});
        }

        return;
    }

    if (name == "Player.HealthPotion")
    {
        if (!onCooldown(name))
        {
            player_.health = std::min(player_.maxHealth, player_.health + player_.maxHealth * 35 / 100);
            setCooldown(name, Duration(60000));
        }

        return;
    }

    if (name == "Player.Eat" || name == "Player.Drink")
    {
        if (!playerInCombat() && !player_.moving)
        {
            const bool eat = name == "Player.Eat";
            player_.sitting = true;
            removeAura(player_.auras, eat ? "Food" : "Drink");
            player_.auras.push_back(SimAura{eat ? kSpellFood : kSpellDrink, eat ? "Food" : "Drink", false, now_ + Duration(20000), 0, Duration(1000), now_ + Duration(1000), 0});
        }

        return;
    }

    if (name == actions::Sit)
    {
        player_.sitting = !player_.sitting;
        return;
    }

    log_.debug(logcat::Sim, "action '{}' has no effect in the simulator", name);
}

bool SimWorld::blocked(const Vec3& from, const Vec3& to) const
{
    const bool jumping = now_ < player_.jumpUntil;

    for (const auto& o : config_.obstacles)
    {
        if (o.low && jumping)
        {
            continue;
        }

        const float dTo = distance2D(to, o.center);

        if (dTo < o.radius && dTo < distance2D(from, o.center))
        {
            return true;
        }
    }

    return false;
}

bool SimWorld::lineBlocked(const Vec3& from, const Vec3& to) const
{
    const Vec3 d = to - from;
    const float len2 = d.x * d.x + d.y * d.y;

    for (const auto& o : config_.obstacles)
    {
        if (o.low)
        {
            continue;
        }

        float t = len2 > 0.0f ? ((o.center.x - from.x) * d.x + (o.center.y - from.y) * d.y) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const Vec3 closest = from + d * t;

        if (distance2D(closest, o.center) < o.radius)
        {
            return true;
        }
    }

    return false;
}

void SimWorld::updateMovement(float dt)
{
    player_.moving = false;

    if (player_.dead && !player_.ghost)
    {
        return;
    }

    auto held = [&](std::string_view a) { return heldMoves_.contains(std::string(a)) ? 1 : 0; };
    const int fwd = held(actions::MoveForward) - held(actions::MoveBackward);
    const int turn = held(actions::TurnLeft) - held(actions::TurnRight);
    const int strafe = held(actions::StrafeLeft) - held(actions::StrafeRight);

    if (turn != 0)
    {
        player_.heading = normalizeHeading(player_.heading + static_cast<float>(turn) * config_.turnRate * dt);
    }

    if (fwd == 0 && strafe == 0)
    {
        return;
    }

    if (player_.sitting)
    {
        standUp();
    }

    if (player_.castEnd != TimePoint{})
    {
        cancelCast();
    }

    Vec3 v = directionFromHeading(player_.heading) * (fwd > 0 ? config_.runSpeed : fwd < 0 ? -config_.backSpeed : 0.0f);
    v += directionFromHeading(player_.heading + kPi / 2.0f) * (static_cast<float>(strafe) * config_.runSpeed);
    const float maxSpeed = (fwd < 0 && strafe == 0) ? config_.backSpeed : config_.runSpeed;
    const float len = v.length2D();

    if (len > maxSpeed)
    {
        v = v * (maxSpeed / len);
    }

    if (player_.ghost)
    {
        v = v * config_.ghostSpeedFactor;
    }

    Vec3 next = player_.position + v * dt;

    if (blocked(player_.position, next))
    {
        // Slide along the obstacle like the game does: drop the velocity
        // component that points into it.
        const bool jumping = now_ < player_.jumpUntil;

        for (const auto& o : config_.obstacles)
        {
            if (o.low && jumping)
            {
                continue;
            }

            if (distance2D(next, o.center) >= o.radius)
            {
                continue;
            }

            Vec3 n = player_.position - o.center;
            n.z = 0.0f;
            const float nLen = n.length2D();

            if (nLen < 0.001f)
            {
                continue;
            }

            n = n / nLen;
            const float inward = -(v.x * n.x + v.y * n.y);

            if (inward > 0.0f)
            {
                v += n * inward;
            }
        }

        next = player_.position + v * dt;

        if (blocked(player_.position, next) || v.length2D() < 0.1f)
        {
            ++counters_.blockedMoves;
            return;
        }
    }

    player_.position = next;
    player_.moving = true;
}

void SimWorld::updateAuras(std::vector<SimAura>& auras, SimMob* mob)
{
    int dotDamage = 0;

    for (auto& a : auras)
    {
        if (a.tickInterval.count() <= 0)
        {
            continue;
        }

        while (now_ >= a.nextTick && a.nextTick <= a.expires)
        {
            a.nextTick += a.tickInterval;

            if (mob)
            {
                dotDamage += a.tickDamage;
            }
            else if (a.name == "Food" && player_.sitting)
            {
                player_.health = std::min(player_.maxHealth, player_.health + player_.maxHealth * 6 / 100);
            }
        }
    }

    std::erase_if(auras, [&](const SimAura& a) { return now_ >= a.expires; });

    if (mob && dotDamage > 0)
    {
        damageMob(*mob, dotDamage, "Rend");
    }
}

void SimWorld::updatePlayer(float)
{
    if (player_.dead)
    {
        return;
    }

    updateAuras(player_.auras, nullptr);

    // Skinning cast.
    if (player_.castEnd != TimePoint{} && now_ >= player_.castEnd)
    {
        if (SimMob* m = findMob(player_.castTarget); m && m->skinnable)
        {
            m->skinnable = false;
            m->despawnAt = std::min(m->despawnAt, now_ + Duration(3000));
            ++counters_.corpsesSkinned;
            log_.debug(logcat::Sim, "skinned {}", m->tpl.name);
        }

        cancelCast();
    }

    // Auto attack.
    if (player_.autoAttack)
    {
        SimMob* t = targetMob();

        if (!t || t->dead)
        {
            player_.autoAttack = false;
        }
        else if (distance(player_.position, t->position) <= config_.meleeRange && facing(t->position) && now_ >= player_.nextSwing)
        {
            int dmg = random(config_.minHit, config_.maxHit);

            if (hasAura(player_.auras, "Battle Shout"))
            {
                dmg = dmg * 11 / 10;
            }

            player_.nextSwing = now_ + seconds(config_.swingTime);
            player_.rage = std::min(100, player_.rage + random(6, 9));
            damageMob(*t, dmg, "melee");
        }
    }

    // Out of combat regeneration.
    if (now_ >= nextRegen_)
    {
        nextRegen_ = now_ + Duration(1000);

        if (!playerInCombat())
        {
            if (!player_.sitting)
            {
                player_.health = std::min(player_.maxHealth, player_.health + std::max(1, player_.maxHealth / 100));
            }

            player_.rage = std::max(0, player_.rage - 2);
        }
    }
}

void SimWorld::aggro(SimMob& mob)
{
    if (mob.dead || mob.mode == SimMob::Mode::Chasing)
    {
        return;
    }

    if (player_.dead)
    {
        return;
    }

    mob.mode = SimMob::Mode::Chasing;
    mob.nextAttack = now_ + Duration(800);
    log_.debug(logcat::Sim, "{} {} aggro", mob.tpl.name, formatGuid(mob.guid));
}

void SimWorld::damageMob(SimMob& mob, int amount, const char* source)
{
    if (mob.dead || mob.mode == SimMob::Mode::Evading)
    {
        return;
    }

    mob.health -= amount;
    mob.tappedByPlayer = true;
    log_.trace(logcat::Sim, "{} hits {} for {} ({} left)", source, mob.tpl.name, amount, std::max(0, mob.health));

    if (mob.health <= 0)
    {
        killMob(mob);
        return;
    }

    aggro(mob);
}

void SimWorld::killMob(SimMob& mob)
{
    mob.dead = true;
    mob.health = 0;
    mob.mode = SimMob::Mode::Idle;
    mob.auras.clear();
    mob.lootable = mob.tappedByPlayer;
    mob.skinnable = mob.tappedByPlayer && mob.tpl.skinnable;
    mob.despawnAt = now_ + config_.corpseDespawn;
    mob.respawnAt = std::max(now_ + config_.mobRespawn, mob.despawnAt);
    ++counters_.mobsKilled;

    if (player_.target == mob.guid)
    {
        player_.autoAttack = false;
    }

    log_.debug(logcat::Sim, "{} {} died", mob.tpl.name, formatGuid(mob.guid));
}

void SimWorld::damagePlayer(int amount, const SimMob& source)
{
    if (player_.dead)
    {
        return;
    }

    player_.health -= amount;
    player_.rage = std::min(100, player_.rage + random(1, 3));

    if (player_.sitting)
    {
        standUp();
    }

    if (player_.castEnd != TimePoint{})
    {
        cancelCast();
    }

    log_.trace(logcat::Sim, "{} hits player for {} ({} left)", source.tpl.name, amount, std::max(0, player_.health));

    if (player_.health <= 0)
    {
        playerDied();
    }
}

void SimWorld::killPlayer()
{
    playerDied();
}

void SimWorld::playerDied()
{
    player_.dead = true;
    player_.ghost = false;
    player_.health = 0;
    player_.corpse = player_.position;
    player_.autoAttack = false;
    player_.sitting = false;
    player_.auras.clear();
    cancelCast();
    ++counters_.playerDeaths;

    for (auto& m : mobs_)
    {
        if (m.mode == SimMob::Mode::Chasing)
        {
            m.mode = SimMob::Mode::Evading;
        }
    }

    log_.debug(logcat::Sim, "player died at {}", player_.position);
}

void SimWorld::updateMobs(float dt)
{
    for (auto& m : mobs_)
    {
        if (m.despawned)
        {
            if (now_ >= m.respawnAt)
            {
                const SimMobTemplate tpl = m.tpl;
                const Vec3 spawn = m.spawn;
                m = SimMob{};
                m.guid = nextGuid_++;
                m.tpl = tpl;
                m.spawn = spawn;
                m.position = spawn;
                m.health = tpl.maxHealth;
                log_.trace(logcat::Sim, "{} respawned {}", tpl.name, formatGuid(m.guid));
            }

            continue;
        }

        if (m.dead)
        {
            if (now_ >= m.despawnAt)
            {
                m.despawned = true;
            }

            continue;
        }

        updateAuras(m.auras, &m);

        if (m.dead)
        {
            continue;
        }

        const bool playerAlive = !player_.dead;
        const float distToPlayer = distance(m.position, player_.position);

        switch (m.mode)
        {
        case SimMob::Mode::Idle:
            if (m.tpl.aggressive && playerAlive && distToPlayer <= m.tpl.aggroRadius)
            {
                aggro(m);
            }

            break;
        case SimMob::Mode::Chasing:
        {
            if (!playerAlive || distance(m.position, m.spawn) > config_.leashRange)
            {
                m.mode = SimMob::Mode::Evading;
                break;
            }

            if (now_ < m.stunnedUntil)
            {
                break;
            }

            m.heading = headingTo(m.position, player_.position);

            if (distToPlayer > 2.5f)
            {
                const float stepLen = std::min(distToPlayer - 2.0f, m.tpl.runSpeed * dt);
                m.position += directionFromHeading(m.heading) * stepLen;
            }
            else if (now_ >= m.nextAttack)
            {
                m.nextAttack = now_ + seconds(m.tpl.attackSpeed);
                damagePlayer(random(m.tpl.minDamage, m.tpl.maxDamage), m);
            }

            break;
        }

        case SimMob::Mode::Evading:
        {
            const float d = distance2D(m.position, m.spawn);

            if (d < 1.0f)
            {
                m.mode = SimMob::Mode::Idle;
                m.health = m.tpl.maxHealth;
                m.auras.clear();
                m.tappedByPlayer = false;
            }
            else
            {
                m.heading = headingTo(m.position, m.spawn);
                m.position += directionFromHeading(m.heading) * std::min(d, m.tpl.runSpeed * 1.3f * dt);
            }

            break;
        }
        }
    }
}

void SimWorld::step(Duration dt)
{
    Duration remaining = dt;

    while (remaining.count() > 0)
    {
        const Duration sub = std::min(remaining, Duration(50));
        now_ += sub;
        remaining -= sub;
        const float s = static_cast<float>(toSeconds(sub));
        updateMovement(s);
        updatePlayer(s);
        updateMobs(s);
    }
}

GameSnapshot SimWorld::snapshot() const
{
    GameSnapshot s;
    s.timestamp = now_;
    s.mapId = 0;
    s.zoneId = 40;

    auto toAura = [&](const SimAura& a)
    {
        Aura out;
        out.spellId = a.spellId;
        out.name = a.name;
        out.harmful = a.harmful;
        out.caster = a.caster;
        out.remaining = static_cast<float>(toSeconds(a.expires - now_));
        return out;
    };

    PlayerState& p = s.player;
    p.guid = player_.guid;
    p.name = config_.playerName;
    p.level = config_.playerLevel;
    p.playerClass = config_.playerClass;
    p.health = std::max(0, player_.health);
    p.maxHealth = player_.maxHealth;
    p.powerType = PowerType::Rage;
    p.power = player_.rage;
    p.maxPower = 100;
    p.setPower(PowerType::Rage, player_.rage, 100);
    p.position = player_.position;
    p.heading = player_.heading;
    p.reaction = Reaction::Friendly;
    p.isPlayer = true;
    p.isDead = player_.dead && !player_.ghost;
    p.isGhost = player_.ghost;
    p.inCombat = playerInCombat();
    p.corpsePosition = player_.corpse;
    p.isMoving = player_.moving;
    p.speed = player_.moving ? config_.runSpeed : 0.0f;
    p.isSitting = player_.sitting;
    p.isAutoAttacking = player_.autoAttack;
    p.isCasting = player_.castEnd != TimePoint{};
    p.targetGuid = player_.target;
    p.freeBagSlots = 16;

    if (counters_.corpsesSkinned > 0)
    {
        p.bagItems.push_back(ItemStack{2318, "Light Leather", counters_.corpsesSkinned});
    }

    for (const auto& a : player_.auras)
    {
        p.auras.push_back(toAura(a));
    }

    for (const auto& m : mobs_)
    {
        if (m.despawned)
        {
            continue;
        }

        if (distance(m.position, player_.position) > config_.visibilityRange)
        {
            continue;
        }

        UnitState u;
        u.guid = m.guid;
        u.entry = m.tpl.entry;
        u.name = m.tpl.name;
        u.level = m.tpl.level;
        u.health = std::max(0, m.health);
        u.maxHealth = m.tpl.maxHealth;
        u.powerType = PowerType::Mana;
        u.position = m.position;
        u.heading = m.heading;
        u.reaction = m.tpl.aggressive ? Reaction::Hostile : Reaction::Neutral;
        u.classification = m.tpl.classification;
        u.isDead = m.dead;
        u.inCombat = !m.dead && m.mode == SimMob::Mode::Chasing;
        u.targetGuid = u.inCombat ? player_.guid : 0;
        u.isLootable = m.dead && m.lootable;
        u.isSkinnable = m.dead && m.skinnable;

        for (const auto& a : m.auras)
        {
            u.auras.push_back(toAura(a));
        }

        s.units.push_back(std::move(u));
    }

    s.targetGuid = player_.target;
    return s;
}

void assignSimulatorKeys(ActionMap& map)
{
    const auto bindAs = [&](const std::string& name, const KeyChord& chord)
    {
        const ActionBinding* settings = map.settingsFor(name);
        ActionBinding b = settings ? *settings : ActionBinding{name, chord, Duration(0), false};
        b.name = name;
        b.chord = chord;
        map.add(std::move(b));
    };
    static constexpr std::pair<const char*, const char*> kStanding[] = {
        {"Move.Forward", "W"}, {"Move.Backward", "S"}, {"Move.TurnLeft", "A"}, {"Move.TurnRight", "D"}, {"Move.StrafeLeft", "Q"}, {"Move.StrafeRight", "E"}, {"Move.Jump", "SPACE"}, {"Target.Next", "TAB"}, {"Target.Clear", "0"}, {"Player.Interact", "F11"}, {"Player.ReleaseSpirit", "-"}, {"Player.RetrieveCorpse", "="},
    };

    for (const auto& [name, key] : kStanding)
    {
        if (!map.find(name))
        {
            if (const auto chord = KeyChord::parse(key))
            {
                bindAs(name, *chord);
            }
        }
    }

    std::vector<KeyChord> used;

    for (const auto& [name, binding] : map.bindings())
    {
        if (binding.chord.valid())
        {
            used.push_back(binding.chord);
        }
    }

    std::vector<std::string> pool;

    for (const char* mod : {"", "SHIFT+", "CTRL+", "ALT+"})
    {
        for (char digit = '1'; digit <= '9'; ++digit)
        {
            pool.push_back(std::string(mod) + digit);
        }
    }

    for (int f = 1; f <= 10; ++f)
    {
        pool.push_back("F" + std::to_string(f));
    }

    for (const char* mod : {"", "SHIFT+", "CTRL+", "ALT+", "CTRL+SHIFT+"})
    {
        for (char letter = 'B'; letter <= 'Z'; ++letter)
        {
            if (std::string_view("WASDQE").find(letter) == std::string_view::npos)
            {
                pool.push_back(std::string(mod) + letter);
            }
        }
    }

    std::size_t next = 0;

    for (const auto& [name, info] : map.info())
    {
        if (map.find(name))
        {
            continue;
        }

        for (; next < pool.size(); ++next)
        {
            const auto chord = KeyChord::parse(pool[next]);

            if (!chord || std::find(used.begin(), used.end(), *chord) != used.end())
            {
                continue;
            }

            used.push_back(*chord);
            bindAs(name, *chord);
            ++next;
            break;
        }
    }
}

} // namespace og
