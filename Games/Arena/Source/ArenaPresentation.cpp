#include "ArenaPresentation.h"

#include "ArenaSimulation.h"

#include <Engine/Systems.h>
#include <Shockwave/Game.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace arena {
using namespace shockwave;

namespace {

void useCurrentPose(World& world, Entity entity) {
    if (auto* previous = world.get<PreviousTransform>(entity)) previous->value = *world.get<Transform>(entity);
}

const glm::vec4 kTeamColour[2] = {{0.08f, 0.5f, 0.7f, 1}, {0.9f, 0.32f, 0.06f, 1}};

}   // namespace

void updateView(
    World& world, Entity player, Entity camera, Entity gun, float yaw, float pitch, float recoil, float reloadLowering,
    float alpha) {
    // Where the body is drawn: blended between ticks, or a network client's presentation
    // pose (the prediction with any correction smoothed out).
    const glm::vec3 feet = presentedTransform(world, player, alpha).position;
    auto& view = *world.get<Transform>(camera);
    view.position = feet + glm::vec3(0, kPlayerEye, 0);
    view.rotation = glm::quat(glm::vec3(pitch, yaw, 0));
    // cameraMatrices uses the current pose. renderWorld must use that exact same
    // pose when walking the rifle's parents, rather than interpolating it again.
    useCurrentPose(world, camera);
    if (auto* weapon = world.get<Transform>(gun)) {
        weapon->position = {0.26f, -0.24f - reloadLowering * 0.18f, -0.55f + recoil * 0.06f};
        useCurrentPose(world, gun);
    }
}

void ArenaPresentation::begin(GameContext& game, Entity camera, Entity gun, Entity flash) {
    m_camera = camera;
    m_gun = gun;
    m_flash = flash;
    if (!game.engine) return;
    // Fixed pool: firing never creates entities or loads resources mid-match.
    for (auto& tracer : m_tracers) {
        tracer = game.world.create();
        game.world.add<Transform>(tracer);
        MeshRenderer mesh;
        mesh.mesh = game.engine->assets().findMesh("cube");
        mesh.texture = game.engine->assets().findTexture("white").value_or(TextureHandle{});
        mesh.tint = {1, 0.7f, 0.18f, 1};
        mesh.emissive = mesh.tint;
        mesh.emissiveIntensity = 2;
        mesh.visible = false;
        mesh.castShadows = false;
        game.world.add<MeshRenderer>(tracer, mesh);
    }
}

void ArenaPresentation::reset(GameContext& game) {
    m_recoil = m_reloadLowering = m_hit = m_hurt = 0;
    m_traceLife.fill(0);
    for (const auto e : m_tracers)
        if (auto* mesh = game.world.get<MeshRenderer>(e)) mesh->visible = false;
}

void ArenaPresentation::tick(GameContext& game, float dt, bool finished) {
    if (finished) {
        for (const auto e : m_tracers)
            if (auto* mesh = game.world.get<MeshRenderer>(e)) mesh->visible = false;
        return;
    }
    m_hit = std::max(0.0f, m_hit - dt);
    m_hurt = std::max(0.0f, m_hurt - dt);
    for (size_t i = 0; i < m_tracers.size(); ++i) {
        m_traceLife[i] -= dt;
        if (auto* mesh = game.world.get<MeshRenderer>(m_tracers[i])) mesh->visible = m_traceLife[i] > 0;
    }
}

void ArenaPresentation::shot(GameContext& game, glm::vec3 origin, glm::vec3 end, bool local) {
    const size_t slot = m_nextTracer++ % m_tracers.size();
    if (auto* t = game.world.get<Transform>(m_tracers[slot])) {
        const auto delta = end - origin;
        const glm::vec3 direction = glm::length(delta) > 0.001f ? glm::normalize(delta) : glm::vec3(0, 0, -1);
        t->position = (origin + end) * 0.5f;
        t->scale = {0.018f, 0.018f, std::max(glm::length(delta), 0.01f)};
        if (glm::length(delta) > 0.001f)
            t->rotation =
                glm::quatLookAtRH(direction, std::abs(direction.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
        game.world.get<MeshRenderer>(m_tracers[slot])->visible = true;
        m_traceLife[slot] = 0.055f;
    }
    if (local) {
        m_recoil = 1;
        game.audio().playClip("Audio/arena-shot.wav", {.volume = 0.35f});
    } else {
        game.audio().playAt("Audio/arena-shot.wav", origin, {.volume = 0.13f, .maxDistance = 35});
    }
}

void ArenaPresentation::showBodies(World& world, Entity localPawn, bool fromState) {
    world.each<const ArenaFighter>([&](Entity pawn, const ArenaFighter& f) {
        if (!ArenaSim::isPawn(f) || f.player) return;
        const bool visible = pawn != localPawn && (!fromState || f.health > 0);
        const glm::vec4& colour = kTeamColour[std::clamp(f.team, 0, 1)];
        world.each<const Parent, MeshRenderer>([&](Entity part, const Parent& p, MeshRenderer& mesh) {
            if (p.entity != pawn) return;
            if (fromState || pawn == localPawn) mesh.visible = visible;
            if (const Name* name = world.get<Name>(part); name && (name->value == "Vest" || name->value == "Visor"))
                mesh.tint = colour;
        });
    });
}

void ArenaPresentation::frame(
    GameContext& game, double frameDt, Entity pawn, float yaw, float pitch, bool active, const ArenaHud& hud) {
    if (active) {
        const float dt = float(std::clamp(frameDt, 0.0, 0.1));
        m_recoil = std::max(0.0f, m_recoil - dt * 8);
        const float target = hud.reloading ? 1.0f : 0.0f;
        m_reloadLowering += (target - m_reloadLowering) * (1.0f - std::exp(-16.0f * dt));
    }
    if (game.world.alive(pawn))
        updateView(
            game.world, pawn, m_camera, m_gun, yaw, pitch, m_recoil, m_reloadLowering,
            game.engine ? float(game.engine->interpolationAlpha()) : 1.0f);
    game.ui().set("arena_blue", hud.blue);
    game.ui().set("arena_orange", hud.orange);
    game.ui().set("arena_health", hud.health);
    game.ui().set("arena_ammo", hud.ammo);
    game.ui().set("arena_magazine", hud.magazine);
    game.ui().set("arena_time", hud.seconds);
    game.ui().set("arena_kills", hud.kills);
    game.ui().set("arena_deaths", hud.deaths);
    game.ui().set("arena_feed", m_feed);
    game.ui().set("arena_status", hud.status);
    game.ui().setClass("Arena HUD", "crosshair", "hit", m_hit > 0);
    game.ui().setClass("Arena HUD", "damage", "hurt", m_hurt > 0);
    if (auto* flash = game.world.get<MeshRenderer>(m_flash)) flash->visible = m_recoil > 0.65f && active && hud.alive;
}

}   // namespace arena
