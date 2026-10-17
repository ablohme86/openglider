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
 *  File: SimWorld.h
 *
 *  Desc:
 *  Declarations and interface for SimWorld (deterministic world
 *  simulator for offline bot testing).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"
#include "og/common/Clock.h"
#include "og/common/Log.h"
#include "og/common/MathUtil.h"
#include "og/input/VirtualInputBackend.h"
#include "og/profiles/GliderProfile.h"
#include "og/state/GameSnapshot.h"
#include "og/state/IGameStateProvider.h"

#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace og
{

struct SimMobTemplate
{
    std::string name = "Sim Mob";
    std::uint32_t entry = 0;
    int level = 10;
    int maxHealth = 160;
    int minDamage = 5;
    int maxDamage = 9;
    float attackSpeed = 2.0f;
    bool aggressive = true;
    float aggroRadius = 10.0f;
    Classification classification = Classification::Normal;
    bool skinnable = true;
    float runSpeed = 6.5f;
    // Another player's: tapped by someone else from the start.
    bool tappedByOther = false;
    // Not to be attacked (a quest NPC, a guard).
    bool attackable = true;
};

struct SimSpawn
{
    SimMobTemplate mob;
    Vec3 position;
};

struct SimObstacle
{
    Vec3 center;
    float radius = 2.0f;
    bool low = false; // can be jumped over
};

struct SimConfig
{
    std::uint32_t seed = 1;

    std::string playerName = "Simwarrior";
    PlayerClass playerClass = PlayerClass::Warrior;
    int playerLevel = 12;
    int playerMaxHealth = 340;
    Vec3 playerStart;
    float playerHeading = 0.0f;
    Vec3 graveyard;

    float runSpeed = 7.0f;
    float backSpeed = 4.5f;
    float turnRate = kPi;
    float ghostSpeedFactor = 1.25f;
    float meleeRange = 5.0f;
    float swingTime = 2.6f;
    int minHit = 16;
    int maxHit = 28;

    Duration mobRespawn{60000};
    Duration corpseDespawn{90000};
    float visibilityRange = 100.0f;
    float leashRange = 60.0f;

    std::vector<SimSpawn> spawns;
    std::vector<SimObstacle> obstacles;
};

// Builds a scenario around a profile: player at the first waypoint, mobs from
// the profile's target list scattered along the route.
SimConfig simConfigFromProfile(const GliderProfile& profile, std::uint32_t seed, int obstacleCount = 0);

struct SimAura
{
    std::uint32_t spellId = 0;
    std::string name;
    bool harmful = false;
    TimePoint expires{};
    Guid caster = 0;
    Duration tickInterval{0};
    TimePoint nextTick{};
    int tickDamage = 0;
};

struct SimMob
{
    Guid guid = 0;
    SimMobTemplate tpl;
    Vec3 spawn;
    Vec3 position;
    float heading = 0.0f;
    int health = 0;
    bool dead = false;
    bool despawned = false;
    enum class Mode
    {
        Idle,
        Chasing,
        Evading
    } mode = Mode::Idle;
    TimePoint nextAttack{};
    TimePoint stunnedUntil{};
    TimePoint despawnAt{};
    TimePoint respawnAt{};
    bool tappedByPlayer = false;
    bool lootable = false;
    bool skinnable = false;
    std::vector<SimAura> auras;
};

struct SimPlayer
{
    Guid guid = 1;
    Vec3 position;
    float heading = 0.0f;
    int health = 0;
    int maxHealth = 0;
    int rage = 0;
    bool dead = false;
    bool ghost = false;
    std::optional<Vec3> corpse;
    Guid target = 0;
    bool autoAttack = false;
    bool sitting = false;
    bool moving = false;
    TimePoint nextSwing{};
    TimePoint jumpUntil{};
    TimePoint releasedAt{};
    TimePoint castEnd{};
    Guid castTarget = 0;
    std::vector<SimAura> auras;
    std::map<std::string, TimePoint> cooldowns;
};

struct SimCounters
{
    int mobsKilled = 0;
    int corpsesLooted = 0;
    int corpsesSkinned = 0;
    int playerDeaths = 0;
    int resurrections = 0;
    int actionsPerformed = 0;
    int blockedMoves = 0;
};

// A tiny deterministic stand-in for the game: moves the player according to
// held movement keys, runs simple mob AI and warrior-ish combat, and produces
// GameSnapshots. Input arrives through the same semantic action bindings the
// bot uses, so the whole Input -> Actions path is exercised.
class SimWorld
{
public:
    SimWorld(SimConfig config, ActionMap bindings, Logger& log);

    // Hook this to VirtualInputBackend::setListener.
    void onInput(const InputEvent& event);
    void step(Duration dt);

    GameSnapshot snapshot() const;
    TimePoint now() const
    {
        return now_;
    }

    const SimPlayer& player() const
    {
        return player_;
    }

    SimPlayer& player()
    {
        return player_;
    }

    const std::vector<SimMob>& mobs() const
    {
        return mobs_;
    }

    std::vector<SimMob>& mobs()
    {
        return mobs_;
    }

    const SimConfig& config() const
    {
        return config_;
    }

    const SimCounters& counters() const
    {
        return counters_;
    }

    bool playerInCombat() const;

    // Test helpers.
    SimMob& spawnMob(const SimMobTemplate& tpl, const Vec3& position);
    void killPlayer();

private:
    void performAction(const std::string& action);
    void updateMovement(float dt);
    void updatePlayer(float dt);
    void updateMobs(float dt);
    void updateAuras(std::vector<SimAura>& auras, SimMob* mob);
    void damageMob(SimMob& mob, int amount, const char* source);
    void damagePlayer(int amount, const SimMob& source);
    void aggro(SimMob& mob);
    void killMob(SimMob& mob);
    void playerDied();
    void standUp();
    void cancelCast();
    bool blocked(const Vec3& from, const Vec3& to) const;
    bool lineBlocked(const Vec3& from, const Vec3& to) const;
    SimMob* findMob(Guid guid);
    SimMob* targetMob();
    bool facing(const Vec3& pos) const;
    bool onCooldown(const std::string& action) const;
    void setCooldown(const std::string& action, Duration d);
    int random(int lo, int hi);
    void removeAura(std::vector<SimAura>& auras, std::string_view name);
    bool hasAura(const std::vector<SimAura>& auras, std::string_view name) const;

    SimConfig config_;
    ActionMap bindings_;
    Logger& log_;
    std::mt19937 rng_;
    TimePoint now_{Duration(1000)};
    SimPlayer player_;
    std::vector<SimMob> mobs_;
    std::set<std::string> heldMoves_;
    std::map<Key, std::vector<std::string>> heldKeyActions_;
    std::uint8_t heldMods_ = ModNone;
    Guid nextGuid_ = 0xF130000000001000ull;
    SimCounters counters_;
    TimePoint nextRegen_{};
};

class SimStateProvider final : public IGameStateProvider
{
public:
    explicit SimStateProvider(SimWorld& world) : world_(world)
    {
    }

    std::string name() const override
    {
        return "sim";
    }

    std::optional<GameSnapshot> capture() override
    {
        return world_.snapshot();
    }

    bool setTarget(Guid guid) override
    {
        world_.player().target = guid;
        return true;
    }

    Camera buildCamera() const
    {
        const auto snap = world_.snapshot();
        Camera cam;
        cam.position = snap.player.position - directionFromHeading(snap.player.heading) * 10.0f + Vec3{0.0f, 0.0f, 3.0f};
        const Vec3 toPlayer = (snap.player.position + Vec3{0.0f, 0.0f, 1.5f} - cam.position);
        const float len = toPlayer.length();

        if (len > 0.01f)
        {
            const Vec3 fwd = toPlayer / len;
            cam.matrix[2][0] = fwd.x;
            cam.matrix[2][1] = fwd.y;
            cam.matrix[2][2] = fwd.z;
            Vec3 right{-fwd.y, fwd.x, 0.0f};
            const float rLen = right.length();

            if (rLen > 0.01f)
            {
                right = right / rLen;
            }

            cam.matrix[0][0] = right.x;
            cam.matrix[0][1] = right.y;
            cam.matrix[0][2] = right.z;
            Vec3 up{right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x};
            cam.matrix[1][0] = up.x;
            cam.matrix[1][1] = up.y;
            cam.matrix[1][2] = up.z;
        }

        return cam;
    }

    std::optional<Camera> camera() const override
    {
        return buildCamera();
    }

    std::optional<ScreenPoint> worldToScreen(const Vec3& worldPos) override
    {
        return buildCamera().worldToScreen(worldPos);
    }

private:
    SimWorld& world_;
};

class SimClock final : public IClock
{
public:
    explicit SimClock(const SimWorld& world) : world_(world)
    {
    }

    TimePoint now() const override
    {
        return world_.now();
    }

private:
    const SimWorld& world_;
};

// The simulator has no action bars to read keys from: movement and the
// standing actions get their usual keys, and every action actions.yaml lists
// without one a free key of its own, with the settings listed for it.
void assignSimulatorKeys(ActionMap& map);

} // namespace og
