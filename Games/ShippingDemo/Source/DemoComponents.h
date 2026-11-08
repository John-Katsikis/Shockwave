#pragma once

// Reactor Run's components (S07, Games/ShippingDemo/README.md). Everything the game
// remembers lives in these, with its saved fields registered in DemoComponents.cpp, so a
// save, a checkpoint and Restart all bring it back without the GameMode keeping a copy.
// Timers count fixed ticks, never frames.

#include <Engine/World.h>

#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace demo {

using shockwave::Entity;

// The one local player: walks with a CharacterController, looks with the mouse or the
// right stick, fires its Loadout from the eye.
struct DemoPlayer {
    float speed = 4.5f;   // metres per second
    float sprint = 1.6f;   // speed multiplier while sprint is held
    float sensitivity = 0.12f;   // degrees per mouse pixel
    float stickSpeed = 150.0f;   // degrees per second at full stick
    float eyeHeight = 1.55f;
    float yaw = 0, pitch = 0;   // radians; saved, so a load keeps the aim
};

// Anything that can be hurt. At 0 it's dead: a target or enemy is destroyed (and stays
// gone with a PersistentIdentity), the player goes down.
struct Health {
    float max = 100;
    float current = 100;
    std::string group;   // what Destroy objectives count ("targets", "enemies")
    int hurtTicks = 0;   // runtime: ticks since the last hit flashes the HUD
};

// A weapon: a magazine, a reserve, hitscan from the eye.
struct Loadout {
    int magazine = 12;   // rounds a full magazine holds
    int ammo = 12;   // in the magazine now
    int reserve = 36;
    int maxReserve = 96;
    float damage = 34;
    float interval = 0.16f;   // seconds between shots
    float reloadSeconds = 1.2f;
    float range = 60;
    int cooldownTicks = 0, reloadTicks = 0;
    int shots = 0, hits = 0;   // for the results screen
};

enum class ObjectiveKind : int { Reach, Destroy, Collect, Use, Hold };
const char* objectiveKindName(ObjectiveKind kind);   // "reach", "destroy", ...

// One step of a level, done in `order` (lowest first). Only the active one (the first
// that isn't complete) makes progress, except Destroy and Collect, which count what is
// already gone so nothing is lost by doing things early.
//   Reach    the player comes within `radius` of `target`
//   Destroy  `required` things with Health of `group` are destroyed (0: all of them)
//   Collect  `required` pickups of `group` are picked up (0: all of them)
//   Use      the InteractionTarget `target` is used
//   Hold     the player stays within `radius` of `target` for `seconds` (fixed ticks)
struct ObjectiveProgress {
    std::string id;   // stable: tests, the campaign and translations refer to it
    ObjectiveKind kind = ObjectiveKind::Reach;
    std::string text;   // what the HUD says while it's active
    int order = 0;
    std::string group;
    int required = 0;
    Entity target;
    float radius = 2.0f;
    float seconds = 0;
    // Progress, saved:
    int count = 0, total = 0;   // Destroy, Collect: done so far, out of
    int ticks = 0;   // Hold
    bool complete = false;
};

enum class InteractionKind : int { Use, Pickup, Exit };

// Something the player uses with an action (a console, the way out) or walks over (a
// pickup). The HUD's prompt names the action's bound key or button, never a letter
// written into the game.
//   Use     does what its Use objective says
//   Pickup  gives `amount` of `item` ("ammo", "health") on contact, then is destroyed
//   Exit    leaves the level when every objective is complete (the results screen first)
struct InteractionTarget {
    InteractionKind kind = InteractionKind::Use;
    std::string prompt = "Use";   // after the binding: "[E] Restart the reactor"
    std::string action = "demo_use";
    float radius = 1.8f;
    std::string item;
    int amount = 0;
    std::string group;   // Pickup: what Collect objectives count
    bool used = false;   // saved
};

// A simple enemy: it sees along a ray, walks straight at what it sees until
// `stopDistance`, and fires on a fixed interval. No behavior tree, no navigation mesh.
struct DemoEnemy {
    float speed = 2.2f;
    float sight = 20;
    float stopDistance = 5;
    float fireInterval = 1.3f;
    float damage = 7;
    float eyeHeight = 1.5f;
    int cooldownTicks = 0;
    bool alerted = false;   // has seen the player
    // Runtime, from the "Demo enemies" system for this tick:
    glm::vec3 move{0};
    bool fire = false;
};

// A door that opens when an objective completes: its colliders turn off and it hides.
struct DemoGate {
    std::string objective;
    bool open = false;   // saved
};

// The level as a whole: what finishing it records, where it leads, and how it went.
struct CampaignProgress {
    std::string completeId;   // game.campaign().complete(this) when the level is won
    std::string next;   // the level the exit leads to
    std::string spawn;
    bool results = true;   // show the results screen before leaving
    enum State : int { Playing, Won, Down };
    int state = Playing;
    int ticks = 0;   // time in the level, in fixed ticks
    int kills = 0;
};

void registerTypes();

}   // namespace demo
