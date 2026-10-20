#pragma once

#include <GameFramework/GameMode.h>
#include <Network/Messages.h>

#include <cstdint>

namespace arena {
// Authored settings appear in the Inspector, scene schema, and --check range validation.
struct ArenaRules {
    int scoreLimit = 20;
    float roundSeconds = 180;
    bool autoPlay = false;
    // Runtime state, available to integration tests and the HUD. Timers count fixed ticks.
    enum Phase : int { Waiting, Playing, Finished };
    int blue = 0, orange = 0, shots = 0, hits = 0, reloads = 0;
    int remainingTicks = 0;
    int phase = Playing;
    uint32_t roundSerial = 0;
    bool finished = false;   // phase == Finished
};
struct ArenaFighter {
    int team = 1;
    bool player = false;   // the one local player of an offline scene; network players aren't authored
    float maxHealth = 100, speed = 5, fireInterval = 1.6f, damage = 10;
    // Runtime state: one component per combatant, no per-entity virtual tick.
    float health = 100;
    int cooldownTicks = 0, respawnTicks = 0, protectionTicks = 0;
    float yaw = 0, pitch = 0;   // aim the authority accepted
    glm::vec3 spawn{0};
    int kills = 0, deaths = 0;
    uint16_t slot = 0;   // network player slot (1 is the host), 0 for bots
    bool ready = false;   // multiplayer lobby
    shockwave::net::InputCommand input{};   // this tick's command (authority)
};
struct ArenaWeapon {
    int magazine = 30;
    float damage = 34, interval = 0.11f, reloadSeconds = 1.5f, sensitivity = 0.1f;
    int ammo = 30;
    int cooldownTicks = 0, reloadTicks = 0;
};
void registerTypes();
// Presentation only: follow interpolated feet, use latest aim, and keep the rifle
// in exactly the camera pose used for this frame. Does not move the physics body.
void updateView(
    shockwave::World& world, shockwave::Entity player, shockwave::Entity camera, shockwave::Entity gun, float yaw,
    float pitch, float recoil, float reloadLowering, float alpha);
std::unique_ptr<shockwave::GameMode> createGame();
}   // namespace arena
