// Reactor Run's GameMode (S07): devices in, DemoRules each tick, HUD, sounds, checkpoints,
// results and the way to the next level. The rules themselves are in DemoRules; this file
// only samples input, shows what happened and asks the flow to travel.
//
// Input is read in frame() and latched for the next tick: held actions (fire, sprint,
// movement) as they are, presses (reload, use) until a tick consumes them, so a tap is
// never lost and never counted twice however many ticks a frame runs. While a menu or
// one of the game's own screens has the input, the command is neutral and presses are
// dropped.
#include "DemoGame.h"

#include "DemoComponents.h"
#include "DemoRules.h"

#include <Engine/Levels.h>
#include <Engine/Systems.h>
#include <Shockwave/Game.h>

#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace demo {
using namespace shockwave;

std::string bindingLabel(const Input& input, std::string_view action, bool gamepad) {
    using S = InputBinding::Source;
    const auto it = input.actions().find(std::string(action));
    if (it == input.actions().end() || it->second.empty()) return "?";
    const InputDevice* pad = nullptr;
    for (const InputDevice& d : input.devices())
        if (d.kind == InputDevice::Kind::Gamepad) {
            pad = &d;
            break;
        }
    auto label = [&](const InputBinding& b) -> std::string {
        switch (b.source) {
        case S::Key: {
            const std::string_view name = keyName(b.code);
            return name.empty() ? "?" : std::string(name);
        }
        case S::Mouse:
            return b.code == 1   ? "Left mouse"
                   : b.code == 3 ? "Right mouse"
                   : b.code == 2 ? "Middle mouse"
                                 : "Mouse " + std::to_string(b.code);
        case S::GamepadButton: {
            const auto button = static_cast<GamepadButton>(b.code);
            return pad ? pad->buttonLabel(button) : std::string(gamepadButtonName(button));
        }
        case S::GamepadAxis:
            switch (static_cast<GamepadAxis>(b.code)) {
            case GamepadAxis::LeftX:
            case GamepadAxis::LeftY: return "Left stick";
            case GamepadAxis::RightX:
            case GamepadAxis::RightY: return "Right stick";
            case GamepadAxis::LeftTrigger: return pad && pad->type == GamepadType::PlayStation5 ? "L2" : "Left trigger";
            case GamepadAxis::RightTrigger:
                return pad && pad->type == GamepadType::PlayStation5 ? "R2" : "Right trigger";
            default: return "?";
            }
        default: return "Joystick";
        }
    };
    for (const InputBinding& b : it->second) {
        const bool isPad = b.source == S::GamepadButton || b.source == S::GamepadAxis;
        if (isPad == gamepad) return label(b);
    }
    return label(it->second.front());
}

namespace {

constexpr const char* kHud = "Demo HUD";
constexpr const char* kResults = "Demo Results";
constexpr const char* kDown = "Demo Down";
constexpr const char* kCredits = "Demo Credits";
constexpr float kCheckpointRadius = 2.5f;

void useCurrentPose(World& world, Entity entity) {
    if (auto* previous = world.get<PreviousTransform>(entity)) previous->value = *world.get<Transform>(entity);
}

std::string percent(int part, int whole) { return whole > 0 ? std::to_string(part * 100 / whole) + "%" : "-"; }

std::string clock(int ticks, float rate) {
    const int seconds = int(float(ticks) / rate);
    std::ostringstream out;
    out << seconds / 60 << ":" << (seconds % 60 < 10 ? "0" : "") << seconds % 60;
    return out.str();
}

class DemoGame final : public GameMode {
public:
    void configureSystems(GameContext&, Schedule& schedule) override {
        // Bulk work across cores: every enemy's sight and move for this tick, from where
        // the player stood after the last one (DemoRules::think). Applied, in entity
        // order, by the rules on the main thread after physics.
        schedule.each<DemoEnemy, const Transform, const Health>(
            "Demo enemies",
            [this](const SystemContext&, Entity e, DemoEnemy& ai, const Transform& at, const Health& health) {
                if (m_rules) m_rules->think(e, ai, at, health);
            });
        schedule.each<Health>("Demo hurt timers", [](const SystemContext&, Health& h) {
            if (h.hurtTicks < 1000) ++h.hurtTicks;
        });
    }

    void beginPlay(GameContext& game) override {
        if (const auto problems = DemoRules::validate(game.world); !problems.empty()) {
            std::string message = "Reactor Run: this level's content can't play (gameplay content):";
            for (const std::string& p : problems) message += "\n  - " + p;
            throw std::runtime_error(message);
        }
        m_gameplay = DemoRules::isGameplayLevel(game.world);
        m_tickRate = game.engine ? float(game.engine->tickRate()) : 60.0f;
        game.ui().set("demo_level", game.flow().level());
        if (!m_gameplay) {
            // The menu and the ending. The ending records that the game was finished,
            // once per campaign however often its credits play.
            if (game.find(kCredits)) {
                game.campaign().complete("game");
                game.ui().on("credits_menu", [&game](const UIEvent&) { game.flow().returnToMenu(); });
                if (game.engine) game.input().setMouseCaptured(false);
            }
            return;
        }
        m_rules.emplace(game.world, &game.physics(), m_tickRate);
        m_rules->scan();
        // Arrive where the level was entered: the spawn's place and facing.
        if (game.startReason == StartReason::Fresh && game.arrival.found) {
            auto& at = *game.world.get<Transform>(m_rules->player());
            at.position = game.arrival.transform.position;
            const glm::vec3 f = game.arrival.transform.rotation * glm::vec3(0, 0, -1);
            game.world.get<DemoPlayer>(m_rules->player())->yaw = std::atan2(-f.x, -f.z);
        }
        m_camera = game.find("Demo Camera").entity();
        m_sceneFov = 0;
        m_gun = game.find("Demo Gun").entity();
        m_flash = game.find("Demo Muzzle").entity();
        readAim(game);
        game.ui().on("results_continue", [this, &game](const UIEvent&) { finish(game); });
        game.ui().on("retry_checkpoint", [this, &game](const UIEvent&) { retry(game, true); });
        game.ui().on("retry_level", [this, &game](const UIEvent&) { retry(game, false); });
        game.ui().on("down_menu", [&game](const UIEvent&) { game.flow().returnToMenu(); });
        screens(game);
    }

    void afterLoad(GameContext& game) override {
        if (!m_rules) return;
        // The world and its physics were replaced: find everything again, take the saved aim.
        m_rules.emplace(game.world, &game.physics(), m_tickRate);
        m_rules->scan();
        m_camera = game.find("Demo Camera").entity();
        m_sceneFov = 0;
        m_gun = game.find("Demo Gun").entity();
        m_flash = game.find("Demo Muzzle").entity();
        readAim(game);
        m_command = {};
        m_finishing = false;
        screens(game);
    }

    void frame(GameContext& game, double frameDt) override {
        if (!m_rules) return;
        Input& input = game.input();
        if (const auto& last = input.lastPressed())
            m_pad = last->source == InputBinding::Source::GamepadButton ||
                    last->source == InputBinding::Source::GamepadAxis;
        const CampaignProgress* level = m_rules->progress();
        const bool playing = level && level->state == CampaignProgress::Playing;
        const bool active = playing && !game.ui().capturesInput();
        const float dt = float(std::clamp(frameDt, 0.0, 0.1));
        if (active) {
            if (game.engine && !input.isMouseCaptured()) input.setMouseCaptured(true);
            const auto* p = game.world.get<DemoPlayer>(m_rules->player());
            // The player's look sensitivity and inverted look (Settings > Controls).
            const PlayerPreferences& prefs = game.preferences();
            const float sensitivity = glm::radians(p ? p->sensitivity : 0.12f) * prefs.lookSensitivity;
            const float stick = glm::radians(p ? p->stickSpeed : 150.0f) * dt * prefs.lookSensitivity;
            const float vertical = prefs.invertLookY ? -1.0f : 1.0f;
            if (input.isMouseCaptured() || !game.engine) {
                m_yaw -= input.mouseDelta().x * sensitivity;
                m_pitch -= input.mouseDelta().y * sensitivity * vertical;
            }
            m_yaw -= (input.actionValue(kLookRight) - input.actionValue(kLookLeft)) * stick;
            m_pitch += (input.actionValue(kLookUp) - input.actionValue(kLookDown)) * stick * vertical;
            m_pitch = std::clamp(m_pitch, -1.45f, 1.45f);
            m_command.move = {
                input.actionValue(kRight) - input.actionValue(kLeft),
                input.actionValue(kForward) - input.actionValue(kBack)};
            m_command.fire = input.isActionDown(kFire);
            // Toggle instead of hold (Settings > Controls): one press starts sprinting, the next stops.
            if (prefs.toggleHold) {
                if (input.wasActionPressed(kSprint)) m_sprintToggled = !m_sprintToggled;
                m_command.sprint = m_sprintToggled;
            } else {
                m_sprintToggled = false;
                m_command.sprint = input.isActionDown(kSprint);
            }
            m_command.reload |= input.wasActionPressed(kReload);
            m_command.use |= input.wasActionPressed(kUse);
        } else {
            m_command = {};
        }
        m_recoil = std::max(0.0f, m_recoil - dt * 8);
        m_hitFlash = std::max(0.0f, m_hitFlash - dt);
        m_hurtFlash = std::max(0.0f, m_hurtFlash - dt);
        view(game);
        hud(game);
    }

    void fixedUpdate(GameContext& game, double) override {
        if (!m_rules) return;
        m_command.yaw = m_yaw;
        m_command.pitch = m_pitch;
        const TickEvents events = m_rules->tick(m_command);
        m_command.reload = m_command.use = false;
        AudioWorld& audio = game.audio();
        if (events.shots) {
            audio.playClip("Audio/shot.wav", {.volume = 0.5f});
            m_recoil = 1;
            game.input().rumble(0.2f, 0.4f, 0.06);
        }
        if (events.hit) {
            audio.playClip("Audio/hit.wav", {.volume = 0.6f});
            m_hitFlash = 0.15f;
        }
        if (events.reloadStarted) audio.playClip("Audio/reload.wav", {.volume = 0.6f});
        if (events.hurt) {
            audio.playClip("Audio/hurt.wav", {.volume = 0.6f});
            m_hurtFlash = 0.3f;
            game.input().rumble(0.6f, 0.2f, 0.15);
        }
        if (!events.pickups.empty() || !events.completed.empty()) audio.playClip("Audio/pickup.wav", {.volume = 0.5f});
        for (const std::string& id : events.completed) SW_LOG(LogGame, Info, "Objective {} complete", id);
        checkpoints(game);
        if (events.won) {
            const CampaignProgress* level = m_rules->progress();
            SW_LOG(LogGame, Info, "Level {} won", game.flow().level());
            if (level && level->results)
                screens(game);
            else
                finish(game);
        }
        if (events.down) {
            SW_LOG(LogGame, Info, "The player went down in {}", game.flow().level());
            screens(game);
        }
    }

private:
    void readAim(GameContext& game) {
        const auto* p = game.world.get<DemoPlayer>(m_rules->player());
        m_yaw = p ? p->yaw : 0;
        m_pitch = p ? p->pitch : 0;
    }

    // Reaching a Checkpoint saves there (once each, and not again for the one a restart
    // came back to).
    void checkpoints(GameContext& game) {
        const CampaignProgress* level = m_rules->progress();
        if (!level || level->state != CampaignProgress::Playing) return;
        const glm::vec3 feet = glm::vec3(worldMatrix(game.world, m_rules->player())[3]);
        game.world.each<const Checkpoint>([&](Entity e, const Checkpoint& c) {
            if (!c.enabled || m_reached.count(c.id) || !game.world.has<Transform>(e)) return;
            if (glm::distance(glm::vec3(worldMatrix(game.world, e)[3]), feet) > kCheckpointRadius) return;
            m_reached.insert(c.id);
            if (game.saves().checkpointId() == c.id) return;   // came back to it
            if (!game.saves().checkpoint(c.id))
                SW_LOG(LogGame, Warning, "Checkpoint {}: {}", c.id, game.saves().lastError());
        });
    }

    void finish(GameContext& game) {
        const CampaignProgress* level = m_rules ? m_rules->progress() : nullptr;
        if (!level || m_finishing) return;
        if (!level->completeId.empty()) game.campaign().complete(level->completeId);
        const TravelTicket ticket = game.finishLevel(level->next, level->spawn);
        m_finishing = bool(ticket);
        if (!ticket) {
            SW_LOG(LogGame, Warning, "Can't go on to {}: {}", level->next, ticket.message);
            game.ui().set(
                "demo_message", game.format("results.cant_go_on", {{"level", level->next}, {"why", ticket.message}}));
        }
    }

    void retry(GameContext& game, bool checkpoint) {
        std::string why;
        if (checkpoint && game.saves().hasCheckpoint(&why)) {
            if (game.saves().restartCheckpoint()) return;
            why = game.saves().lastError();
        }
        if (checkpoint) SW_LOG(LogGame, Info, "No checkpoint to go back to ({}): restarting the level", why);
        game.flow().restartLevel();
    }

    // The results and down screens follow the level's state; they take the mouse.
    void screens(GameContext& game) {
        const CampaignProgress* level = m_rules->progress();
        const int state = level ? level->state : CampaignProgress::Playing;
        game.ui().show(kResults, state == CampaignProgress::Won && level->results);
        game.ui().show(kDown, state == CampaignProgress::Down);
        std::string why;
        game.ui().set("demo_can_retry_checkpoint", game.saves().hasCheckpoint(&why));
        game.ui().set("demo_retry_reason", why);
        if (state != CampaignProgress::Playing && game.engine) game.input().setMouseCaptured(false);
    }

    void view(GameContext& game) {
        const Entity player = m_rules->player();
        auto* camera = game.world.get<Transform>(m_camera);
        if (!camera || !game.world.alive(player)) return;
        const auto* p = game.world.get<DemoPlayer>(player);
        const double alpha = game.engine ? game.engine->interpolationAlpha() : 1.0;
        camera->position =
            presentedTransform(game.world, player, alpha).position + glm::vec3(0, p ? p->eyeHeight : 1.55f, 0);
        // The recoil kick is camera shake: the player's Camera shake scales it, 0 stills it.
        const PlayerPreferences& prefs = game.preferences();
        camera->rotation = glm::quat(glm::vec3(m_pitch + m_recoil * 0.01f * prefs.cameraShake, m_yaw, 0));
        // Field of view: the player's, or the scene's own when they leave it at 0.
        if (auto* lens = game.world.get<Camera>(m_camera)) {
            if (m_sceneFov <= 0) m_sceneFov = lens->fov;
            lens->fov = prefs.fieldOfView > 0 ? prefs.fieldOfView : m_sceneFov;
        }
        useCurrentPose(game.world, m_camera);
        const auto* gun = game.world.get<Loadout>(player);
        if (auto* model = game.world.get<Transform>(m_gun)) {
            const float lowered = gun && gun->reloadTicks > 0 ? 0.16f : 0.0f;
            model->position = {0.24f, -0.22f - lowered, -0.5f + m_recoil * 0.05f};
            useCurrentPose(game.world, m_gun);
        }
        if (auto* flash = game.world.get<MeshRenderer>(m_flash)) flash->visible = m_recoil > 0.6f;
    }

    void hud(GameContext& game) {
        UIWorld& ui = game.ui();
        const Input& input = game.input();
        const Entity player = m_rules->player();
        const CampaignProgress* level = m_rules->progress();
        const auto* life = game.world.get<Health>(player);
        const auto* gun = game.world.get<Loadout>(player);
        const int state = level ? level->state : 0;
        ui.set(
            "demo_state", state == CampaignProgress::Won    ? "won"
                          : state == CampaignProgress::Down ? "down"
                                                            : "playing");
        ui.set("demo_health", life ? int(std::ceil(life->current)) : 0);
        ui.set("demo_ammo", gun ? gun->ammo : 0);
        ui.set("demo_magazine", gun ? gun->magazine : 0);
        ui.set("demo_reserve", gun ? gun->reserve : 0);
        ui.set("demo_reloading", gun && gun->reloadTicks > 0);
        // The objective: its text, and how far it's come.
        const Entity active = m_rules->activeObjective();
        int done = 0, total = 0;
        for (const Entity e : m_rules->objectives()) {
            ++total;
            done += game.world.get<ObjectiveProgress>(e)->complete;
        }
        std::string text = game.text("hud.all_objectives_done"), id, progress;
        if (const auto* o = game.world.get<ObjectiveProgress>(active)) {
            text = o->text;
            id = o->id;
            if (o->kind == ObjectiveKind::Destroy || o->kind == ObjectiveKind::Collect) {
                const int needed = o->required > 0 ? std::min(o->required, o->total) : o->total;
                progress = game.format(
                    "hud.progress_count",
                    {{"count", std::to_string(std::min(o->count, needed))}, {"needed", std::to_string(needed)}});
            } else if (o->kind == ObjectiveKind::Hold) {
                const int left = std::max(0, m_rules->ticksFor(o->seconds) - o->ticks);
                progress = game.format(
                    "hud.progress_seconds", {{"seconds", std::to_string(int(std::ceil(float(left) / m_tickRate)))}});
            }
        } else if (level && state == CampaignProgress::Playing) {
            text = game.text(level->results ? "hud.find_exit" : "hud.open_gate");
        }
        ui.set("demo_objective", text);
        ui.set("demo_objective_id", id.empty() ? std::string("done") : id);
        ui.set("demo_progress", progress);
        ui.set("demo_objectives_done", done);
        ui.set("demo_objectives_total", total);
        ui.set(
            "demo_objective_caption",
            game.format("hud.objective_caption", {{"done", std::to_string(done)}, {"total", std::to_string(total)}}));
        // What's in reach, with the key or button that uses it.
        const Entity near = m_rules->usable();
        std::string prompt, nearName;
        if (const auto* t = game.world.get<InteractionTarget>(near)) {
            prompt =
                game.format("hud.prompt", {{"binding", bindingLabel(input, t->action, m_pad)}, {"prompt", t->prompt}});
            const auto* n = game.world.get<Name>(near);
            nearName = n ? n->value : "something";
        }
        ui.set("demo_prompt", prompt);
        ui.set("demo_near", nearName);
        ui.set("demo_has_prompt", !prompt.empty());
        // The controls, from the bindings.
        std::string move = m_pad
                               ? bindingLabel(input, kForward, true)
                               : bindingLabel(input, kForward, false) + " " + bindingLabel(input, kLeft, false) + " " +
                                     bindingLabel(input, kBack, false) + " " + bindingLabel(input, kRight, false);
        ui.set(
            "demo_controls",
            game.format(
                "hud.controls", {{"move", move},
                                 {"look", m_pad ? bindingLabel(input, kLookRight, true) : game.text("hud.mouse")},
                                 {"fire", bindingLabel(input, kFire, m_pad)},
                                 {"reload", bindingLabel(input, kReload, m_pad)},
                                 {"use", bindingLabel(input, kUse, m_pad)}}));
        ui.set("demo_reload_label", bindingLabel(input, kReload, m_pad));
        ui.set("demo_empty", game.format("hud.empty", {{"binding", bindingLabel(input, kReload, m_pad)}}));
        ui.set("demo_low_ammo", gun && gun->ammo == 0 && gun->reloadTicks == 0);
        // Checkpoint, and the level as a whole.
        const auto saveState = game.saves().checkpointState();
        ui.set(
            "demo_checkpoint", saveState == SaveGames::CheckpointState::Saving ? game.text("hud.checkpoint_saving")
                               : saveState == SaveGames::CheckpointState::Saved
                                   ? (m_reached.empty() ? std::string() : game.text("hud.checkpoint_reached"))
                               : saveState == SaveGames::CheckpointState::Failed ? game.text("hud.checkpoint_failed")
                                                                                 : std::string());
        ui.set("demo_enemies_left", m_rules->alive("enemies"));
        ui.set("demo_targets_left", m_rules->alive("targets"));
        ui.set("demo_kills", level ? level->kills : 0);
        ui.set("demo_time", level ? clock(level->ticks, m_tickRate) : std::string("0:00"));
        ui.set("demo_shots", gun ? gun->shots : 0);
        ui.set("demo_accuracy", gun ? percent(gun->hits, gun->shots) : std::string("-"));
        ui.set("demo_time_text", game.format("results.time", {{"time", ui.get("demo_time")}}));
        ui.set("demo_kills_text", game.format("results.kills", {{"kills", std::to_string(level ? level->kills : 0)}}));
        ui.set(
            "demo_shots_text",
            game.format(
                "results.shots", {{"shots", std::to_string(gun ? gun->shots : 0)},
                                  {"accuracy", gun ? percent(gun->hits, gun->shots) : std::string("-")}}));
        // Where the crosshair points: what it's on, and the facing in whole degrees.
        const Entity aimed = m_rules->aimedAt(m_yaw, m_pitch);
        const auto* aimedName = game.world.get<Name>(aimed);
        ui.set("demo_aiming_at", aimedName ? aimedName->value : std::string());
        ui.set("demo_yaw", int(std::lround(glm::degrees(std::remainder(m_yaw, 2 * glm::pi<float>())))));
        ui.setClass(kHud, "crosshair", "on", aimed.valid());
        ui.setClass(kHud, "crosshair", "hit", m_hitFlash > 0);
        // Reduce flashes (Settings > Accessibility): a faint edge instead of a red screen.
        const bool soft = game.preferences().reduceFlashes;
        ui.setClass(kHud, "damage", "hurt", m_hurtFlash > 0 && !soft);
        ui.setClass(kHud, "damage", "hurt_soft", m_hurtFlash > 0 && soft);
    }

    std::optional<DemoRules> m_rules;
    bool m_gameplay = false;
    float m_tickRate = 60;
    Entity m_camera, m_gun, m_flash;
    float m_yaw = 0, m_pitch = 0;
    PlayerCommand m_command;
    bool m_pad = false;   // the player used a gamepad last: prompts show its buttons
    float m_recoil = 0, m_hitFlash = 0, m_hurtFlash = 0;
    float m_sceneFov = 0;
    bool m_sprintToggled = false;   // the camera's own field of view, before the player's
    std::set<std::string> m_reached;
    bool m_finishing = false;
};

}   // namespace

std::unique_ptr<GameMode> createGame() { return std::make_unique<DemoGame>(); }

}   // namespace demo
