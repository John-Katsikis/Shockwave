#include "ArenaSimulation.h"

#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>
#include <GameFramework/InputSampler.h>
#include <Multiplayer/NetworkComponents.h>
#include <Physics/Physics.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace arena {
using namespace shockwave;
using net::InputCommand;

namespace {

void useCurrentPose(World& world, Entity entity) {
    if (auto* previous = world.get<PreviousTransform>(entity)) previous->value = *world.get<Transform>(entity);
}

glm::vec3 aimDirection(float yaw, float pitch) { return glm::quat(glm::vec3(pitch, yaw, 0)) * glm::vec3(0, 0, -1); }

}   // namespace

int ticksFor(float seconds, uint32_t tickRate) {
    return seconds > 0 ? int(std::ceil(double(seconds) * double(tickRate) - 1e-9)) : 0;
}

glm::vec3 ArenaSim::eye(Entity e) const {
    const ArenaFighter* f = world.get<ArenaFighter>(e);
    return world.get<Transform>(e)->position + glm::vec3(0, f && isPawn(*f) ? kPlayerEye : kBotEye, 0);
}

Entity ArenaSim::nearestEnemy(Entity shooter) const {
    const auto& self = *world.get<ArenaFighter>(shooter);
    Entity result;
    float best = 10000;
    world.each<const ArenaFighter>([&](Entity e, const ArenaFighter& f) {
        if (f.team == self.team || f.health <= 0) return;
        const auto delta = eye(e) - eye(shooter);
        const float d = glm::dot(delta, delta);
        if (d < best) {
            best = d;
            result = e;
        }
    });
    return result;
}

InputCommand ArenaSim::autoCommand(Entity pawn, uint32_t incarnation) const {
    InputCommand c;
    c.incarnation = incarnation;
    const ArenaFighter& f = *world.get<ArenaFighter>(pawn);
    float yaw = f.yaw, pitch = f.pitch;
    if (f.health > 0)
        if (const Entity target = nearestEnemy(pawn); target.valid()) {
            const glm::vec3 delta = eye(target) - eye(pawn);
            if (glm::dot(delta, delta) > 1e-8f) {
                const glm::vec3 d = glm::normalize(delta);
                yaw = std::atan2(-d.x, -d.z);
                pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
            }
        }
    c.yaw = net::encodeYaw(yaw);
    c.pitch = net::encodePitch(pitch);
    c.heldBits = InputCommand::kHeldFire;
    return c;
}

void ArenaSim::setVisible(Entity e, bool visible) {
    if (auto* mesh = world.get<MeshRenderer>(e)) mesh->visible = visible;
    world.each<const Parent, MeshRenderer>([&](const Parent& p, MeshRenderer& mesh) {
        if (p.entity == e) mesh.visible = visible;
    });
    if (auto* collider = world.get<Collider>(e)) collider->enabled = visible;
    if (auto* controller = world.get<CharacterController>(e)) controller->enabled = visible;
}

void ArenaSim::respawn(Entity e, ArenaFighter& f) {
    f.health = f.maxHealth;
    f.protectionTicks = ticksFor(kProtectionSeconds, tickRate);
    f.cooldownTicks = ticksFor(1, tickRate);
    f.respawnTicks = 0;
    world.get<Transform>(e)->position = f.spawn;
    useCurrentPose(world, e);
    setVisible(e, true);
    if (auto* w = world.get<ArenaWeapon>(e)) {
        w->ammo = w->magazine;
        w->reloadTicks = 0;
    }
    if (hooks.respawned) hooks.respawned(e);
}

void ArenaSim::startRound() {
    auto& r = match();
    r.blue = r.orange = r.shots = r.hits = r.reloads = 0;
    r.remainingTicks = ticksFor(r.roundSeconds, tickRate);
    r.phase = ArenaRules::Playing;
    r.finished = false;
    time = 0;
    std::vector<Entity> moved;
    world.each<ArenaFighter, Transform>([&](Entity e, ArenaFighter& f, Transform& t) {
        f.health = f.maxHealth;
        f.cooldownTicks = ticksFor(1 + float(e.index % 6) * 0.15f, tickRate);
        f.respawnTicks = 0;
        f.protectionTicks = ticksFor(kProtectionSeconds, tickRate);
        f.kills = f.deaths = 0;
        f.input = {};
        t.position = f.spawn;
        useCurrentPose(world, e);   // restart is a teleport, not a movement to blend
        setVisible(e, true);
        if (auto* c = world.get<CharacterController>(e)) {
            c->desiredVelocity = {};
            c->jumpRequested = false;
        }
        if (auto* w = world.get<ArenaWeapon>(e)) {
            w->ammo = w->magazine;
            w->cooldownTicks = w->reloadTicks = 0;
        }
        moved.push_back(e);
    });
    if (hooks.respawned)
        for (const Entity e : moved) hooks.respawned(e);
}

void ArenaSim::reload(Entity pawn) {
    auto* w = world.get<ArenaWeapon>(pawn);
    const auto* f = world.get<ArenaFighter>(pawn);
    if (!w || !f || w->reloadTicks > 0 || w->ammo == w->magazine || f->health <= 0) return;
    w->reloadTicks = ticksFor(w->reloadSeconds, tickRate);
    ++match().reloads;
    if (hooks.reloaded) hooks.reloaded(pawn);
}

glm::vec3 ArenaSim::pawnVelocity(const ArenaFighter& f, const InputCommand& c) {
    const bool sprint = (c.heldBits & InputCommand::kHeldSprint) != 0;
    return commandMoveDirection(c) * f.speed * (sprint ? kSprint : 1.0f);
}

void ArenaSim::stepControls(float dt) {
    auto& r = match();
    world.each<const ArenaFighter, CharacterController>([](const ArenaFighter& f, CharacterController& c) {
        if (ArenaSim::isPawn(f)) c.desiredVelocity = {};
    });
    if (r.phase == ArenaRules::Finished) return;
    const bool playing = r.phase == ArenaRules::Playing;
    if (playing) time += dt;
    std::vector<Entity> respawns;
    world.each<ArenaFighter, Transform>([&](Entity e, ArenaFighter& f, Transform& t) {
        if (!playing) return;
        f.cooldownTicks = std::max(0, f.cooldownTicks - 1);
        f.protectionTicks = std::max(0, f.protectionTicks - 1);
        if (f.health <= 0) {
            if (--f.respawnTicks <= 0) respawns.push_back(e);
            return;
        }
        if (!isPawn(f) && f.speed > 0)
            t.position = f.spawn + glm::vec3(std::sin(time * f.speed + float(e.index)) * 1.1f, 0, 0);
    });
    // Outside the iteration: a respawn may tell the network, which adds components.
    for (const Entity e : respawns) respawn(e, *world.get<ArenaFighter>(e));
    std::vector<Entity> reloads;
    world.each<ArenaFighter, CharacterController>([&](Entity e, ArenaFighter& f, CharacterController& controller) {
        if (!isPawn(f) || f.health <= 0) return;
        const InputCommand& c = f.input;
        f.yaw = commandYaw(c);
        f.pitch = commandPitch(c);
        controller.desiredVelocity = pawnVelocity(f, c);
        if (c.pressedBits & InputCommand::kPressedJump) controller.jumpRequested = true;
        if (playing && (c.pressedBits & InputCommand::kPressedReload)) reloads.push_back(e);
    });
    for (const Entity e : reloads) reload(e);
}

void ArenaSim::stepCombat() {
    auto& r = match();
    if (r.phase != ArenaRules::Playing) return;
    r.remainingTicks = std::max(0, r.remainingTicks - 1);
    std::vector<Entity> pawns, bots;
    world.each<const ArenaFighter>([&](Entity e, const ArenaFighter& f) { (isPawn(f) ? pawns : bots).push_back(e); });
    for (const Entity e : pawns) {
        auto* weapon = world.get<ArenaWeapon>(e);
        if (!weapon) continue;
        auto& w = *weapon;
        w.cooldownTicks = std::max(0, w.cooldownTicks - 1);
        if (w.reloadTicks > 0 && --w.reloadTicks == 0) w.ammo = w.magazine;
        const auto& f = *world.get<ArenaFighter>(e);
        if (f.health <= 0 || w.reloadTicks > 0 || w.cooldownTicks > 0) continue;
        if (!(f.input.heldBits & InputCommand::kHeldFire)) continue;
        if (w.ammo == 0) {
            reload(e);
            continue;
        }
        --w.ammo;
        ++r.shots;
        w.cooldownTicks = ticksFor(w.interval, tickRate);
        shoot(e, eye(e), aimDirection(f.yaw, f.pitch), w.damage);
    }
    // Bots share the same line-of-sight and damage rules as the players.
    for (const Entity e : bots) {
        auto& fighter = *world.get<ArenaFighter>(e);
        if (fighter.health <= 0 || fighter.cooldownTicks > 0) continue;
        const Entity target = nearestEnemy(e);
        if (!target.valid()) continue;
        const auto origin = eye(e), delta = eye(target) - origin;
        const float distance = glm::length(delta);
        if (distance < 0.01f) continue;
        const auto hit = physics.raycast(origin, delta / distance, distance + 0.5f, e);
        if (!hit || hit->entity != target) continue;
        fighter.cooldownTicks = ticksFor(fighter.fireInterval, tickRate);
        shoot(e, origin, delta / distance, fighter.damage);
    }
    if (r.blue >= r.scoreLimit || r.orange >= r.scoreLimit || r.remainingTicks <= 0) {
        r.phase = ArenaRules::Finished;
        r.finished = true;
    }
}

void ArenaSim::shoot(Entity from, glm::vec3 origin, glm::vec3 direction, float damage) {
    const auto hit = physics.raycast(origin, direction, 60, from);
    const auto end = hit ? hit->point : origin + direction * 60.0f;
    auto* victim = hit ? world.get<ArenaFighter>(hit->entity) : nullptr;
    if (hooks.shot) hooks.shot(from, origin, end, victim ? hit->entity : kNullEntity);
    if (!victim) return;
    auto& shooter = *world.get<ArenaFighter>(from);
    if (victim->health <= 0 || victim->team == shooter.team || victim->protectionTicks > 0) return;
    if (isPawn(shooter)) ++match().hits;
    if (hooks.hit) hooks.hit(from, hit->entity);
    victim->health = std::max(0.0f, victim->health - damage);
    if (victim->health > 0) return;
    ++shooter.kills;
    ++victim->deaths;
    victim->respawnTicks = ticksFor(kRespawnSeconds, tickRate);
    setVisible(hit->entity, false);
    auto& r = match();
    if (shooter.team == 0)
        ++r.blue;
    else
        ++r.orange;
    if (hooks.killed) hooks.killed(from, hit->entity);
}

int teamCount(World& world, int team, Entity except) {
    int n = 0;
    world.each<const ArenaFighter>([&](Entity e, const ArenaFighter& f) {
        if (ArenaSim::isPawn(f) && f.team == team && e != except) ++n;
    });
    return n;
}

int teamWithRoom(World& world, Entity except) {
    const int blue = teamCount(world, 0, except), orange = teamCount(world, 1, except);
    if (blue >= kTeamSize && orange >= kTeamSize) return -1;
    if (blue >= kTeamSize) return 1;
    if (orange >= kTeamSize) return 0;
    return orange < blue ? 1 : 0;
}

bool spawnPoint(World& world, int team, Entity except, glm::vec3& out) {
    std::vector<std::pair<int, glm::vec3>> points;
    world.each<const net::NetworkSpawnPoint, const Transform>([&](const net::NetworkSpawnPoint& p, const Transform& t) {
        if (p.team == team) points.emplace_back(p.order, t.position);
    });
    if (points.empty()) return false;
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [order, position] : points) {
        bool taken = false;
        world.each<const ArenaFighter, const Transform>([&](Entity e, const ArenaFighter& f, const Transform& t) {
            const glm::vec3 d = t.position - position;
            if (e != except && f.health > 0 && glm::dot(d, d) < 1.0f) taken = true;
        });
        if (!taken) {
            out = position;
            return true;
        }
    }
    out = points.front().second;
    return true;
}

}   // namespace arena
