#pragma once

// Reactor Run's rules (S07): one fixed tick of the level from the player's command. No
// input devices, no UI and no audio here, so tests drive exactly what the game runs.
//
// Order inside a tick, which is also the precedence when things happen at once:
//   1. the player moves, aims, reloads, fires and uses what's in reach
//   2. enemies move and fire (their aim was worked out by the "Demo enemies" system)
//   3. pickups the player touches are collected
//   4. what reached 0 health dies: targets and enemies are destroyed, the player goes down
//   5. only if the player is still standing: objectives progress, gates open, the exit
//      can be used
// So a death wins over a victory in the same tick: you have to be standing to win, and
// an objective never completes on the tick its player went down. Completing an
// objective sets its saved `complete` flag once; finishing the level records the
// campaign completion once (CampaignState::complete), so no reward is given twice.

#include "DemoComponents.h"

#include <Engine/Components.h>
#include <Engine/World.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace shockwave {
class PhysicsWorld;
}

namespace demo {

// The player's input for one tick, sampled from the devices by the GameMode.
struct PlayerCommand {
    glm::vec2 move{0};   // x right, y forward, each -1..1
    float yaw = 0, pitch = 0;   // the aim, radians
    bool fire = false;   // held
    bool sprint = false;   // held
    bool reload = false;   // pressed since the last tick
    bool use = false;   // pressed since the last tick
};

// What happened in a tick, for sounds, the HUD and the GameMode's flow.
struct TickEvents {
    int shots = 0;
    bool hit = false, kill = false;
    bool reloadStarted = false, reloaded = false, dryFire = false;
    bool hurt = false;
    std::vector<std::string> pickups;   // items collected
    std::vector<std::string> completed;   // objective ids completed
    std::vector<std::string> opened;   // gates opened (their objective ids)
    bool won = false, down = false;
};

class DemoRules {
public:
    DemoRules(shockwave::World& world, shockwave::PhysicsWorld* physics, float tickRate);

    // What's wrong with a level's content, one line each; empty when it can play. A
    // level with no DemoPlayer and no CampaignProgress (the menu, the ending) is fine.
    static std::vector<std::string> validate(const shockwave::World& world);
    static bool isGameplayLevel(const shockwave::World& world);

    // Finds the player and the level, counts what Destroy and Collect objectives start
    // with, and applies gates. After beginPlay and after a save is loaded.
    void scan();
    TickEvents tick(const PlayerCommand& command);

    // The "Demo enemies" system: works out each enemy's move and whether it fires this
    // tick from where the player stood after the last one. Any thread, between steps.
    void think(shockwave::Entity enemy, DemoEnemy& ai, const shockwave::Transform& at, const Health& health) const;

    shockwave::Entity player() const { return m_player; }
    shockwave::Entity level() const { return m_level; }
    CampaignProgress* progress() const;
    // The first objective that isn't complete (null when all are, or there are none).
    shockwave::Entity activeObjective() const;
    std::vector<shockwave::Entity> objectives() const;   // in order
    bool allComplete() const;
    // What the player can use right now: in reach, its objective active (or an Exit with
    // everything complete). kNullEntity when nothing.
    shockwave::Entity usable() const;
    // Health of `group` still standing.
    int alive(const std::string& group) const;
    glm::vec3 eye() const;   // the player's, from its Transform and eye height
    // What the crosshair is on: the entity with Health the eye's ray meets first, within
    // the Loadout's range. kNullEntity when nothing (or a wall).
    shockwave::Entity aimedAt(float yaw, float pitch) const;
    int ticksFor(float seconds) const;

private:
    void movePlayer(const PlayerCommand& c, TickEvents& events);
    void fire(const PlayerCommand& c, TickEvents& events);
    void enemies(TickEvents& events);
    void pickups(TickEvents& events);
    void deaths(TickEvents& events);
    void objectivesStep(TickEvents& events);
    void applyGates(TickEvents* events);
    void useExit(TickEvents& events);
    shockwave::Entity healthOwner(shockwave::Entity hit) const;   // the entity or a parent with Health
    glm::vec3 positionOf(shockwave::Entity e) const;
    bool usableNow(shockwave::Entity e) const;

    shockwave::World& m_world;
    shockwave::PhysicsWorld* m_physics;
    float m_tickRate;
    shockwave::Entity m_player = shockwave::kNullEntity, m_level = shockwave::kNullEntity;
    glm::vec3 m_playerEye{0};   // as of the last tick, for think()
    bool m_useHandled = false;
    std::vector<shockwave::Entity> m_dead;   // this tick's
    shockwave::Entity m_killedBy = shockwave::kNullEntity;
};

}   // namespace demo
