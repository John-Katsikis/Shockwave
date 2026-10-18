#pragma once

#include <GameFramework/GameMode.h>

namespace arena {
// Authored settings appear in the Inspector, scene schema, and --check range validation.
struct ArenaRules {
    int scoreLimit = 20;
    float roundSeconds = 180;
    bool autoPlay = false;
    // Runtime counters, available to integration tests and the HUD.
    int blue = 0, orange = 0, shots = 0, hits = 0, reloads = 0;
    float remaining = 180;
    bool finished = false;
};
struct ArenaFighter {
    int team = 1;
    bool player = false;
    float maxHealth = 100, speed = 5, fireInterval = 1.6f, damage = 10;
    // Runtime state: one component per combatant, no per-entity virtual tick.
    float health = 100, cooldown = 0, respawn = 0, protection = 0;
    glm::vec3 spawn{0};
    int kills = 0, deaths = 0;
};
struct ArenaWeapon {
    int magazine = 30;
    float damage = 34, interval = 0.11f, reloadSeconds = 1.5f, sensitivity = 0.1f;
    int ammo = 30;
    float cooldown = 0, reload = 0;
};
void registerTypes();
// Presentation only: follow interpolated feet, use latest aim, and keep the rifle
// in exactly the camera pose used for this frame. Does not move the physics body.
void updateView(
    shockwave::World& world, shockwave::Entity player, shockwave::Entity camera, shockwave::Entity gun, float yaw,
    float pitch, float recoil, float reloadLowering, float alpha);
std::unique_ptr<shockwave::GameMode> createGame();
}   // namespace arena
