#include "ArenaGame.h"

#include "ArenaGraphicsMenu.h"

#include <Shockwave/Game.h>

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <stdexcept>

namespace arena {
using namespace shockwave;

namespace {
void useCurrentPose(World& world, Entity entity) {
    if (auto* previous = world.get<PreviousTransform>(entity)) previous->value = *world.get<Transform>(entity);
}
}   // namespace

void updateView(
    World& world, Entity player, Entity camera, Entity gun, float yaw, float pitch, float recoil, float reloadLowering,
    float alpha) {
    const auto& body = *world.get<Transform>(player);
    glm::vec3 feet = body.position;
    if (const auto* previous = world.get<PreviousTransform>(player))
        feet = glm::mix(previous->value.position, feet, std::clamp(alpha, 0.0f, 1.0f));
    auto& view = *world.get<Transform>(camera);
    view.position = feet + glm::vec3(0, 1.55f, 0);
    view.rotation = glm::quat(glm::vec3(pitch, yaw, 0));
    // cameraMatrices uses the current pose. renderWorld must use that exact same
    // pose when walking the rifle's parents, rather than interpolating it again.
    useCurrentPose(world, camera);
    if (auto* weapon = world.get<Transform>(gun)) {
        weapon->position = {0.26f, -0.24f - reloadLowering * 0.18f, -0.55f + recoil * 0.06f};
        useCurrentPose(world, gun);
    }
}

void registerTypes() {
    registerComponent<ArenaRules>(
        "ArenaRules",
        {field("score_limit", &ArenaRules::scoreLimit, 1, 1, 200),
         field("round_seconds", &ArenaRules::roundSeconds, 1, 10, 3600), field("auto_play", &ArenaRules::autoPlay)},
        "Arena");
    registerComponent<ArenaFighter>(
        "ArenaFighter",
        {field("team", &ArenaFighter::team, 1, 0, 1), field("player", &ArenaFighter::player),
         field("max_health", &ArenaFighter::maxHealth, 1, 1, 1000), field("speed", &ArenaFighter::speed, 0.1f, 0, 15),
         field("fire_interval", &ArenaFighter::fireInterval, 0.01f, 0.1f, 10),
         field("damage", &ArenaFighter::damage, 1, 1, 100)},
        "Arena");
    registerComponent<ArenaWeapon>(
        "ArenaWeapon",
        {field("magazine", &ArenaWeapon::magazine, 1, 1, 100), field("damage", &ArenaWeapon::damage, 1, 1, 100),
         field("interval", &ArenaWeapon::interval, 0.01f, 0.05f, 2),
         field("reload_seconds", &ArenaWeapon::reloadSeconds, 0.1f, 0.1f, 10),
         field("sensitivity", &ArenaWeapon::sensitivity, 0.01f, 0.01f, 1)},
        "Arena");
}
class ArenaGame final : public GameMode {
public:
    void configureSystems(GameContext& game, Schedule& schedule) override {
        // Controls and patrols run before Jolt; fixedUpdate below handles shots after Jolt.
        schedule.add(
            "Arena controls", {.reads = {}, .writes = {}, .exclusive = true},
            [this, &game](const SystemContext& ctx) { prepare(game, ctx.dt); });
    }
    void beginPlay(GameContext& game) override {
        int rulesCount = 0, players = 0;
        const auto range = [](float value, float low, float high, const char* name) {
            if (!std::isfinite(value) || value < low || value > high)
                throw std::runtime_error(
                    std::string(name) + " must be between " + std::to_string(low) + " and " + std::to_string(high));
        };
        game.world.each<ArenaRules>([&](Entity e, ArenaRules& rules) {
            range(float(rules.scoreLimit), 1, 200, "ArenaRules.score_limit");
            range(rules.roundSeconds, 10, 3600, "ArenaRules.round_seconds");
            m_rules = e;
            ++rulesCount;
        });
        game.world.each<ArenaWeapon>([&](const ArenaWeapon& weapon) {
            range(float(weapon.magazine), 1, 100, "ArenaWeapon.magazine");
            range(weapon.damage, 1, 100, "ArenaWeapon.damage");
            range(weapon.interval, .05f, 2, "ArenaWeapon.interval");
            range(weapon.reloadSeconds, .1f, 10, "ArenaWeapon.reload_seconds");
            range(weapon.sensitivity, .01f, 1, "ArenaWeapon.sensitivity");
        });
        game.world.each<ArenaFighter>([&](Entity e, ArenaFighter& f) {
            range(float(f.team), 0, 1, "ArenaFighter.team");
            range(f.maxHealth, 1, 1000, "ArenaFighter.max_health");
            range(f.speed, 0, 15, "ArenaFighter.speed");
            range(f.fireInterval, .1f, 10, "ArenaFighter.fire_interval");
            range(f.damage, 1, 100, "ArenaFighter.damage");
            if (!game.world.has<Transform>(e) || game.world.has<Parent>(e))
                throw std::runtime_error("ArenaFighter requires a root Transform");
            if (f.player) {
                m_player = e;
                ++players;
                if (!game.world.has<CharacterController>(e) || !game.world.has<ArenaWeapon>(e))
                    throw std::runtime_error("Arena player requires CharacterController and ArenaWeapon");
            } else if (!game.world.has<Collider>(e))
                throw std::runtime_error("Arena bot requires Collider");
            f.spawn = game.world.get<Transform>(e)->position;
        });
        m_camera = game.find("Arena Camera").entity();
        if (rulesCount != 1 || players != 1 || !game.world.has<Camera>(m_camera) ||
            !game.world.has<Transform>(m_camera) || game.world.has<Parent>(m_camera) ||
            !game.world.get<Camera>(m_camera)->primary)
            throw std::runtime_error(
                "Arena needs exactly one ArenaRules, one player, and a root Arena Camera with primary "
                "Camera/Transform");
        m_gun = game.find("Arena Rifle").entity();
        m_flash = game.find("Arena Muzzle").entity();
        reset(game);
        if (game.engine) {
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
        m_graphicsMenu.bind(game);
        game.ui().on("arena_resume", [this, &game](const UIEvent&) { pause(game, false); });
        game.ui().on("arena_restart", [this, &game](const UIEvent&) {
            reset(game);
            pause(game, false);
        });
        SW_LOG(LogGame, Info, "Arena: local bot match ready; click to capture mouse, P for menu, F2 to restart");
    }
    void fixedUpdate(GameContext& game, double seconds) override {
        auto& rules = *game.world.get<ArenaRules>(m_rules);
        if (m_paused || rules.finished) return;
        const float dt = float(seconds);
        rules.remaining = std::max(0.0f, rules.remaining - dt);
        m_hit = std::max(0.0f, m_hit - dt);
        m_hurt = std::max(0.0f, m_hurt - dt);
        for (size_t i = 0; i < m_tracers.size(); ++i) {
            m_traceLife[i] -= dt;
            if (auto* mesh = game.world.get<MeshRenderer>(m_tracers[i])) mesh->visible = m_traceLife[i] > 0;
        }
        auto& weapon = *game.world.get<ArenaWeapon>(m_player);
        weapon.cooldown = std::max(0.0f, weapon.cooldown - dt);
        if (weapon.reload > 0) {
            weapon.reload = std::max(0.0f, weapon.reload - dt);
            if (weapon.reload == 0) weapon.ammo = weapon.magazine;
        }
        auto& player = *game.world.get<ArenaFighter>(m_player);
        if (player.health > 0 && weapon.reload == 0 && weapon.cooldown == 0 && (m_fire || rules.autoPlay)) {
            if (weapon.ammo == 0)
                reload(game);
            else {
                --weapon.ammo;
                ++rules.shots;
                weapon.cooldown = weapon.interval;
                m_recoil = 1;
                const auto origin = eye(game, m_player);
                const auto direction = glm::quat(glm::vec3(m_pitch, m_yaw, 0)) * glm::vec3(0, 0, -1);
                shoot(game, m_player, origin, direction, weapon.damage);
                game.audio().playClip("Audio/arena-shot.wav", {.volume = 0.35f});
            }
        }
        // Bots share the same line-of-sight and damage rules as the player.
        game.world.each<ArenaFighter>([&](Entity e, ArenaFighter& fighter) {
            if (fighter.player || fighter.health <= 0 || fighter.cooldown > 0) return;
            const Entity target = nearest(game, e);
            if (!target.valid()) return;
            const auto origin = eye(game, e), delta = eye(game, target) - origin;
            const float distance = glm::length(delta);
            if (distance < 0.01f) return;
            const auto hit = game.physics().raycast(origin, delta / distance, distance + 0.5f, e);
            if (!hit || hit->entity != target) return;
            fighter.cooldown = fighter.fireInterval;
            shoot(game, e, origin, delta / distance, fighter.damage);
            game.audio().playAt("Audio/arena-shot.wav", origin, {.volume = 0.13f, .maxDistance = 35});
        });
        rules.finished = rules.blue >= rules.scoreLimit || rules.orange >= rules.scoreLimit || rules.remaining <= 0;
        if (rules.finished) {
            for (const auto e : m_tracers)
                if (auto* mesh = game.world.get<MeshRenderer>(e)) mesh->visible = false;
        }
        updateCamera(game);
    }
    void frame(GameContext& game, double frameDt) override {
        auto& input = game.input();
        if (input.wasActionPressed("arena_menu")) pause(game, !m_paused);
        if (input.wasActionPressed("arena_restart")) reset(game);
        const auto& rules = *game.world.get<ArenaRules>(m_rules);
        const auto& fighter = *game.world.get<ArenaFighter>(m_player);
        auto& weapon = *game.world.get<ArenaWeapon>(m_player);
        if (!m_paused && !rules.autoPlay && !rules.finished) {
            if (input.wasActionPressed("arena_fire") && !input.isMouseCaptured() && !game.ui().capturesInput())
                input.setMouseCaptured(true);
            if (input.isMouseCaptured()) {
                const auto delta = input.mouseDelta();
                m_yaw -= glm::radians(delta.x * weapon.sensitivity);
                m_pitch = std::clamp(m_pitch - glm::radians(delta.y * weapon.sensitivity), -1.45f, 1.45f);
            }
            m_fire = input.isMouseCaptured() && input.isActionDown("arena_fire");
            if (input.wasActionPressed("arena_reload")) reload(game);
            if (input.wasActionPressed("arena_jump") && fighter.health > 0)
                game.world.get<CharacterController>(m_player)->jumpRequested = true;
        } else
            m_fire = false;
        if (!m_paused && !rules.finished) {
            const float dt = float(std::clamp(frameDt, 0.0, 0.1));
            m_recoil = std::max(0.0f, m_recoil - dt * 8);
            const float target = weapon.reload > 0 ? 1.0f : 0.0f;
            m_reloadLowering += (target - m_reloadLowering) * (1.0f - std::exp(-16.0f * dt));
        }
        updateView(
            game.world, m_player, m_camera, m_gun, m_yaw, m_pitch, m_recoil, m_reloadLowering,
            game.engine ? float(game.engine->interpolationAlpha()) : 1.0f);
        game.ui().set("arena_blue", rules.blue);
        game.ui().set("arena_orange", rules.orange);
        game.ui().set("arena_health", int(std::ceil(fighter.health)));
        game.ui().set("arena_ammo", weapon.ammo);
        game.ui().set("arena_magazine", weapon.magazine);
        game.ui().set("arena_time", int(std::ceil(rules.remaining)));
        game.ui().set("arena_kills", fighter.kills);
        game.ui().set("arena_deaths", fighter.deaths);
        game.ui().set("arena_feed", m_feed);
        std::string status = "CLICK TO CAPTURE MOUSE";
        if (input.isMouseCaptured()) status = "WASD MOVE / SHIFT SPRINT / SPACE JUMP / R RELOAD / P MENU";
        if (rules.autoPlay) status = "AUTOPLAY / LOCAL BOT MATCH";
        if (weapon.reload > 0) status = "RELOADING";
        if (fighter.health <= 0)
            status = "ELIMINATED / RESPAWNING IN " + std::to_string(int(std::ceil(fighter.respawn)));
        if (rules.finished)
            status = (rules.blue == rules.orange  ? "DRAW"
                      : rules.blue > rules.orange ? "BLUE WINS"
                                                  : "ORANGE WINS") +
                     std::string(" / F2 TO RESTART");
        game.ui().set("arena_status", status);
        game.ui().setClass("Arena HUD", "crosshair", "hit", m_hit > 0);
        game.ui().setClass("Arena HUD", "damage", "hurt", m_hurt > 0);
        if (auto* flash = game.world.get<MeshRenderer>(m_flash))
            flash->visible = m_recoil > 0.65f && !m_paused && !rules.finished && fighter.health > 0;
    }
    void endPlay(GameContext& game) override {
        game.input().setMouseCaptured(false);
        const auto* rules = game.world.get<ArenaRules>(m_rules);
        if (!rules) return;
        const auto& r = *rules;
        SW_LOG(
            LogGame, Info, "Arena result: blue={} orange={} shots={} hits={} reloads={}", r.blue, r.orange, r.shots,
            r.hits, r.reloads);
    }

private:
    glm::vec3 eye(GameContext& game, Entity e) const {
        return game.world.get<Transform>(e)->position + glm::vec3(0, e == m_player ? 1.55f : 1.25f, 0);
    }
    Entity nearest(GameContext& game, Entity shooter) const {
        const auto& self = *game.world.get<ArenaFighter>(shooter);
        Entity result;
        float best = 10000;
        game.world.each<const ArenaFighter>([&](Entity e, const ArenaFighter& f) {
            if (f.team == self.team || f.health <= 0) return;
            const auto delta = eye(game, e) - eye(game, shooter);
            const float d = glm::dot(delta, delta);
            if (d < best) {
                best = d;
                result = e;
            }
        });
        return result;
    }
    void visibility(GameContext& game, Entity e, bool visible) {
        if (auto* mesh = game.world.get<MeshRenderer>(e)) mesh->visible = visible;
        game.world.each<const Parent, MeshRenderer>([&](const Parent& p, MeshRenderer& mesh) {
            if (p.entity == e) mesh.visible = visible;
        });
        if (auto* collider = game.world.get<Collider>(e)) collider->enabled = visible;
        if (auto* controller = game.world.get<CharacterController>(e)) controller->enabled = visible;
    }
    void reset(GameContext& game) {
        auto& r = *game.world.get<ArenaRules>(m_rules);
        r.blue = r.orange = r.shots = r.hits = r.reloads = 0;
        r.remaining = r.roundSeconds;
        r.finished = false;
        game.world.each<ArenaFighter, Transform>([&](Entity e, ArenaFighter& f, Transform& t) {
            f.health = f.maxHealth;
            f.cooldown = 1 + float(e.index % 6) * 0.15f;
            f.respawn = 0;
            f.protection = 1;
            f.kills = f.deaths = 0;
            t.position = f.spawn;
            useCurrentPose(game.world, e);   // restart is a teleport, not a movement to blend
            visibility(game, e, true);
            if (auto* c = game.world.get<CharacterController>(e)) {
                c->desiredVelocity = {};
                c->jumpRequested = false;
            }
        });
        auto& w = *game.world.get<ArenaWeapon>(m_player);
        w.ammo = w.magazine;
        w.cooldown = w.reload = 0;
        m_time = m_yaw = m_pitch = m_recoil = m_reloadLowering = m_hit = m_hurt = 0;
        m_fire = false;
        m_traceLife.fill(0);
        for (const auto e : m_tracers)
            if (auto* mesh = game.world.get<MeshRenderer>(e)) mesh->visible = false;
        m_feed = "FIRST TO " + std::to_string(r.scoreLimit) + " / BLUE VS ORANGE";
        updateCamera(game);
    }
    GraphicsMenu m_graphicsMenu;
    void pause(GameContext& game, bool paused) {
        m_graphicsMenu.close(game);
        m_paused = paused;
        m_fire = false;
        game.ui().set("arena_menu_open", paused);
        if (paused) {
            game.ui().show("Arena Menu");
            game.input().setMouseCaptured(false);
        } else {
            game.ui().hide("Arena Menu");
            game.input().setMouseCaptured(true);
        }
    }
    void updateCamera(GameContext& game) {
        auto& camera = *game.world.get<Transform>(m_camera);
        camera.position = eye(game, m_player);
        camera.rotation = glm::quat(glm::vec3(m_pitch, m_yaw, 0));
    }
    void prepare(GameContext& game, float dt) {
        auto& r = *game.world.get<ArenaRules>(m_rules);
        auto& controller = *game.world.get<CharacterController>(m_player);
        controller.desiredVelocity = {};
        if (m_paused || r.finished) return;
        m_time += dt;
        game.world.each<ArenaFighter, Transform>([&](Entity e, ArenaFighter& f, Transform& t) {
            f.cooldown = std::max(0.0f, f.cooldown - dt);
            f.protection = std::max(0.0f, f.protection - dt);
            if (f.health <= 0) {
                f.respawn -= dt;
                if (f.respawn <= 0) {
                    f.health = f.maxHealth;
                    f.protection = 1;
                    f.cooldown = 1;
                    t.position = f.spawn;
                    useCurrentPose(game.world, e);
                    visibility(game, e, true);
                    if (f.player) {
                        auto& w = *game.world.get<ArenaWeapon>(e);
                        w.ammo = w.magazine;
                        w.reload = 0;
                    }
                }
                return;
            }
            if (!f.player && f.speed > 0)
                t.position = f.spawn + glm::vec3(std::sin(m_time * f.speed + float(e.index)) * 1.1f, 0, 0);
        });
        const auto& f = *game.world.get<ArenaFighter>(m_player);
        if (f.health <= 0) return;
        if (r.autoPlay) {
            const Entity target = nearest(game, m_player);
            if (target.valid()) {
                const auto d = glm::normalize(eye(game, target) - eye(game, m_player));
                m_yaw = std::atan2(-d.x, -d.z);
                m_pitch = std::asin(d.y);
            }
        } else if (game.input().isMouseCaptured()) {
            glm::vec3 move(
                game.input().axis("arena_left", "arena_right"), 0, game.input().axis("arena_forward", "arena_back"));
            if (glm::length(move) > 1) move = glm::normalize(move);
            controller.desiredVelocity = glm::quat(glm::vec3(0, m_yaw, 0)) * move * f.speed *
                                         (game.input().isActionDown("arena_sprint") ? 1.55f : 1);
        }
    }
    void reload(GameContext& game) {
        auto& w = *game.world.get<ArenaWeapon>(m_player);
        if (w.reload > 0 || w.ammo == w.magazine || game.world.get<ArenaFighter>(m_player)->health <= 0) return;
        w.reload = w.reloadSeconds;
        ++game.world.get<ArenaRules>(m_rules)->reloads;
        game.audio().playClip("Audio/arena-reload.wav", {.volume = 0.25f});
    }
    void shoot(GameContext& game, Entity from, glm::vec3 origin, glm::vec3 direction, float damage) {
        const auto hit = game.physics().raycast(origin, direction, 60, from);
        const auto end = hit ? hit->point : origin + direction * 60.0f;
        const size_t slot = m_nextTracer++ % m_tracers.size();
        if (auto* t = game.world.get<Transform>(m_tracers[slot])) {
            const auto delta = end - origin;
            t->position = (origin + end) * 0.5f;
            t->scale = {0.018f, 0.018f, std::max(glm::length(delta), 0.01f)};
            if (glm::length(delta) > 0.001f)
                t->rotation = glm::quatLookAtRH(
                    glm::normalize(delta), std::abs(direction.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
            game.world.get<MeshRenderer>(m_tracers[slot])->visible = true;
            m_traceLife[slot] = 0.055f;
        }
        if (!hit) return;
        auto* victim = game.world.get<ArenaFighter>(hit->entity);
        auto& shooter = *game.world.get<ArenaFighter>(from);
        if (!victim || victim->health <= 0 || victim->team == shooter.team || victim->protection > 0) return;
        if (from == m_player) {
            m_hit = 0.12f;
            ++game.world.get<ArenaRules>(m_rules)->hits;
        }
        if (hit->entity == m_player) m_hurt = 0.2f;
        victim->health = std::max(0.0f, victim->health - damage);
        if (victim->health > 0) return;
        ++shooter.kills;
        ++victim->deaths;
        victim->respawn = 3;
        visibility(game, hit->entity, false);
        auto& r = *game.world.get<ArenaRules>(m_rules);
        if (shooter.team == 0)
            ++r.blue;
        else
            ++r.orange;
        const auto* name = game.world.get<Name>(hit->entity);
        m_feed = std::string(shooter.team == 0 ? "BLUE" : "ORANGE") + " eliminated " + (name ? name->value : "target");
        if (from == m_player) game.audio().playClip("Audio/arena-hit.wav", {.volume = 0.3f});
    }
    Entity m_rules, m_player, m_camera, m_gun, m_flash;
    std::array<Entity, 24> m_tracers{};
    std::array<float, 24> m_traceLife{};
    size_t m_nextTracer = 0;
    float m_yaw = 0, m_pitch = 0, m_time = 0, m_recoil = 0, m_hit = 0, m_hurt = 0;
    float m_reloadLowering = 0;
    bool m_fire = false, m_paused = false;
    std::string m_feed;
};
std::unique_ptr<GameMode> createGame() { return std::make_unique<ArenaGame>(); }
}   // namespace arena
