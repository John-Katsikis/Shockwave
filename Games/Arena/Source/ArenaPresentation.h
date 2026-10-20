#pragma once

// What the local player sees and hears: the first-person camera and rifle, tracers, muzzle
// flash, recoil, sounds, the HUD and the third-person bodies of everyone else. It never
// changes the match: on a client it only shows the host's state, and a shot it shows
// before the host confirms it never counts as a hit.

#include "ArenaGame.h"

#include <array>
#include <string>

namespace arena {

// What the HUD shows this frame.
struct ArenaHud {
    int blue = 0, orange = 0, seconds = 0;
    int health = 0, ammo = 0, magazine = 0, kills = 0, deaths = 0;
    bool reloading = false, alive = true, finished = false;
    std::string status;
};

class ArenaPresentation {
public:
    // camera: the one primary camera of this process; gun: the rifle parented to it.
    void begin(shockwave::GameContext& game, shockwave::Entity camera, shockwave::Entity gun, shockwave::Entity flash);
    void reset(shockwave::GameContext& game);
    // Once per fixed tick: tracers, hit marker and damage flash fade.
    void tick(shockwave::GameContext& game, float dt, bool finished);

    // A shot to draw (and hear): local is the player's own.
    void shot(shockwave::GameContext& game, glm::vec3 origin, glm::vec3 end, bool local);
    void hit() { m_hit = 0.12f; }   // the host confirmed it
    void hurt() { m_hurt = 0.2f; }
    void kick() { m_recoil = 1; }
    void setFeed(std::string feed) { m_feed = std::move(feed); }

    // Once per rendered frame. pawn is the local player's (null: the camera stays).
    // Network bodies: everyone else's shows while alive, in their team's colour; the
    // local player's own body is hidden (first person).
    void frame(
        shockwave::GameContext& game, double frameDt, shockwave::Entity pawn, float yaw, float pitch, bool active,
        const ArenaHud& hud);
    // Clients: bodies follow the host's life state, since nothing here simulates them.
    void showBodies(shockwave::World& world, shockwave::Entity localPawn, bool fromState);

private:
    shockwave::Entity m_camera, m_gun, m_flash;
    std::array<shockwave::Entity, 24> m_tracers{};
    std::array<float, 24> m_traceLife{};
    size_t m_nextTracer = 0;
    float m_recoil = 0, m_hit = 0, m_hurt = 0, m_reloadLowering = 0;
    std::string m_feed;
};

}   // namespace arena
