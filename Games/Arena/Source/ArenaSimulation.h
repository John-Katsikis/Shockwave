#pragma once

// Arena's rules, as the authority runs them (the host, or offline play): movement from
// each pawn's InputCommand, weapon and respawn timers in whole fixed ticks, shots,
// damage, teams, scores and the round. Nothing here reads a device, draws, or knows
// whether the game is online; what it reports goes through ArenaSimHooks.
//
// Every timer is ceil(seconds * tick_rate) ticks, counted down once per tick, so ammo
// use and fire cadence depend on the tick rate only, never on the frame rate.

#include "ArenaGame.h"

#include <Engine/World.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace shockwave {
class PhysicsWorld;
}

namespace arena {

inline constexpr int kTeamSize = 6;   // six a side, enforced by the authority
inline constexpr float kRespawnSeconds = 3, kProtectionSeconds = 1;
inline constexpr float kPlayerEye = 1.55f, kBotEye = 1.25f;
inline constexpr float kSprint = 1.55f;

int ticksFor(float seconds, uint32_t tickRate);

// What the rules tell the rest of the game.
struct ArenaSimHooks {
    // A shot was fired: tracer, sound. victim is null for a miss.
    std::function<void(shockwave::Entity shooter, glm::vec3 origin, glm::vec3 end, shockwave::Entity victim)> shot;
    std::function<void(shockwave::Entity shooter, shockwave::Entity victim)> hit;
    std::function<void(shockwave::Entity killer, shockwave::Entity victim)> killed;
    std::function<void(shockwave::Entity pawn)> reloaded;   // a reload began
    // A pawn was moved to its spawn point: a new life, or a teleport (team change).
    std::function<void(shockwave::Entity pawn)> respawned;
};

struct ArenaSim {
    shockwave::World& world;
    shockwave::PhysicsWorld& physics;
    shockwave::Entity rules;
    uint32_t tickRate = 60;
    ArenaSimHooks hooks;
    float time = 0;   // seconds of play, for the bots' patrol

    ArenaRules& match() const { return *world.get<ArenaRules>(rules); }
    // A pawn is a fighter a player controls (the offline player, or a network player).
    static bool isPawn(const ArenaFighter& f) { return f.player || f.slot != 0; }

    // Before physics: timers, respawns, the bots' patrol, and each pawn's movement and
    // reload from fighter.input.
    void stepControls(float dt);
    // After physics: pawns' weapons (fire held, at the weapon's cadence), the bots' shots,
    // the round clock and its end.
    void stepCombat();

    // Starts a round: scores and counters cleared, everyone alive at their spawn point.
    void startRound();
    void respawn(shockwave::Entity e, ArenaFighter& f);
    void reload(shockwave::Entity pawn);
    // Dead fighters are hidden and their collision is off.
    void setVisible(shockwave::Entity e, bool visible);

    glm::vec3 eye(shockwave::Entity e) const;
    shockwave::Entity nearestEnemy(shockwave::Entity shooter) const;
    // Autoplay: aim at the nearest enemy and hold fire, standing still.
    shockwave::net::InputCommand autoCommand(shockwave::Entity pawn, uint32_t incarnation) const;

private:
    void shoot(shockwave::Entity from, glm::vec3 origin, glm::vec3 direction, float damage);
};

// The team with room and the fewest players (blue on a tie), or -1 when both are full.
int teamWithRoom(shockwave::World& world, shockwave::Entity except = shockwave::kNullEntity);
int teamCount(shockwave::World& world, int team, shockwave::Entity except = shockwave::kNullEntity);
// The team's spawn point (NetworkSpawnPoint) to use: the first in order with no living
// fighter within a metre, else the first. False if the team has none.
bool spawnPoint(shockwave::World& world, int team, shockwave::Entity except, glm::vec3& out);

}   // namespace arena
