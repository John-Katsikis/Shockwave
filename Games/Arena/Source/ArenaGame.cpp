// Arena's GameMode: lifecycle and who decides what. The authority (the host, or offline
// play) runs ArenaSimulation with one InputCommand per pawn per tick: its own player's
// from the local InputSampler, every other player's from the network. A client runs no
// rules at all: it sends its sampled commands, predicts its own pawn's movement with them
// and shows what the host's snapshots say for everything else.
//
// Two kinds of scene play with it. An offline scene (Scenes/arena.swscene) authors one
// local player and bots. A multiplayer scene (Scenes/arena-mp.swscene) authors no player:
// the authority spawns a Net Player for each player who joins, six a side, at the team's
// NetworkSpawnPoints; played offline it's a match for one.
#include "ArenaGame.h"

#include "ArenaGraphicsMenu.h"
#include "ArenaNetwork.h"
#include "ArenaPresentation.h"
#include "ArenaSimulation.h"

#include <GameFramework/InputSampler.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkWorld.h>
#include <Shockwave/Game.h>

#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>

namespace arena {
using namespace shockwave;
using net::InputCommand;

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

namespace {

// Config/Input.swinput's names for the parts of an input command.
const InputActionNames kActions{"arena_left", "arena_right",  "arena_forward", "arena_back",
                                "arena_fire", "arena_sprint", "arena_jump",    "arena_reload"};

const char* teamName(int team) { return team == 0 ? "BLUE" : "ORANGE"; }

std::string fighterName(World& world, Entity e) {
    const auto* f = world.get<ArenaFighter>(e);
    if (f && f->slot != 0) return "PLAYER " + std::to_string(f->slot);
    const auto* name = world.get<Name>(e);
    return name ? name->value : "target";
}

const char* connectingText(net::NetworkState state) {
    switch (state) {
    case net::NetworkState::Starting:
    case net::NetworkState::Connecting: return "CONNECTING TO THE HOST";
    case net::NetworkState::Authenticating: return "AUTHENTICATING";
    case net::NetworkState::Synchronizing: return "LOADING THE MATCH";
    default: return nullptr;
    }
}

}   // namespace

class ArenaGame final : public GameMode {
public:
    void configureSystems(GameContext& game, Schedule& schedule) override {
        // Controls and patrols run before Jolt; fixedUpdate below handles shots after Jolt.
        schedule.add(
            "Arena controls", {.reads = {}, .writes = {}, .exclusive = true},
            [this, &game](const SystemContext& ctx) { controls(game, ctx.dt); });
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
        int spawnPoints[2] = {0, 0};
        game.world.each<const net::NetworkSpawnPoint>([&](const net::NetworkSpawnPoint& p) {
            if (p.team == 0 || p.team == 1) ++spawnPoints[p.team];
        });
        m_match = players == 0 && (spawnPoints[0] > 0 || spawnPoints[1] > 0);
        m_camera = game.find("Arena Camera").entity();
        if (rulesCount != 1 || (players != 1 && !m_match) || !game.world.has<Camera>(m_camera) ||
            !game.world.has<Transform>(m_camera) || game.world.has<Parent>(m_camera) ||
            !game.world.get<Camera>(m_camera)->primary)
            throw std::runtime_error(
                "Arena needs exactly one ArenaRules, one player, and a root Arena Camera with primary "
                "Camera/Transform");
        net::NetworkWorld& network = game.network();
        if (network.online() && !m_match)
            throw std::runtime_error("Arena network play needs a multiplayer scene (Scenes/arena-mp.swscene)");
        if (m_match) {
            if (spawnPoints[0] == 0 || spawnPoints[1] == 0)
                throw std::runtime_error("Arena multiplayer scene needs NetworkSpawnPoints for team 0 and team 1");
            if (network.online()) {
                for (const net::NetworkPrefab& prefab : network.start().prefabs)
                    if (prefab.path == kNetPlayerPrefab) m_prefabId = prefab.id;
                if (m_prefabId == 0)
                    throw std::runtime_error(
                        std::string("Arena network play needs ") + kNetPlayerPrefab + " in the network manifest");
            }
        }
        m_gun = game.find("Arena Rifle").entity();
        m_flash = game.find("Arena Muzzle").entity();
        uint32_t tickRate = game.engine ? uint32_t(std::lround(game.engine->tickRate())) : 60;
        if (network.online()) tickRate = network.start().settings.tickRate;
        m_sim.emplace(ArenaSim{game.world, game.physics(), m_rules, tickRate, {}});
        hookUp(game);
        if (network.role() == net::NetworkRole::Client) {
            // The client moves its own pawn at once by the host's movement rule
            // (ArenaSim::stepControls), and the host corrects it when they differ.
            network.setPredictionMove([this](const World& world, Entity pawn, const InputCommand& c) {
                net::PredictedMove move;
                const auto* f = world.get<ArenaFighter>(pawn);
                if (!f || f->health <= 0 || m_sim->match().phase == ArenaRules::Finished) return move;
                move.desiredVelocity = ArenaSim::pawnVelocity(*f, c);
                move.jump = (c.pressedBits & InputCommand::kPressedJump) != 0;
                return move;
            });
        }
        m_view.begin(game, m_camera, m_gun, m_flash);
        if (!network.authority()) {
            // A client: everything comes from the host, starting with the baseline.
            m_sim->match().phase = ArenaRules::Waiting;
        } else if (m_match) {
            joinPlayer(game, 1);
            if (network.online()) {
                // Players gather first; the round starts when everyone is ready.
                auto& r = m_sim->match();
                r.phase = ArenaRules::Waiting;
                r.remainingTicks = ticksFor(r.roundSeconds, tickRate);
            } else {
                restart(game);
            }
        } else {
            restart(game);
        }
        m_graphicsMenu.bind(game);
        game.ui().on("arena_resume", [this, &game](const UIEvent&) { pause(game, false); });
        game.ui().on("arena_restart", [this, &game](const UIEvent&) {
            requestRestart(game);
            pause(game, false);
        });
        game.ui().on("arena_ready", [this, &game](const UIEvent&) { request(game, kReadyToPlay, 1); });
        game.ui().on("arena_team_blue", [this, &game](const UIEvent&) { request(game, kRequestTeam, 0); });
        game.ui().on("arena_team_orange", [this, &game](const UIEvent&) { request(game, kRequestTeam, 1); });
        game.ui().on("arena_leave", [&game](const UIEvent&) {
            if (game.network().online()) game.network().close();
        });
        game.ui().set("arena_online", network.online());
        game.ui().set("arena_match", m_match);
        SW_LOG(
            LogGame, Info, "Arena: {} ready; click to capture mouse, P for menu, F2 to restart",
            !m_match                                     ? "local bot match"
            : network.role() == net::NetworkRole::Client ? "network client"
                                                         : "match");
    }

    void collectInput(GameContext& game, double) override {
        const auto& rules = *game.world.get<ArenaRules>(m_rules);
        const bool active = !m_menuOpen && !rules.autoPlay && !rules.finished && game.input().isMouseCaptured() &&
                            !game.network().localInputSuppressed();
        const Entity pawn = localPawn(game);
        const auto* weapon = game.world.alive(pawn) ? game.world.get<ArenaWeapon>(pawn) : nullptr;
        m_input.collect(readInputFrame(game.input(), kActions, active), weapon ? weapon->sensitivity : 0.1f);
    }

    void fixedUpdate(GameContext& game, double seconds) override {
        auto& rules = *game.world.get<ArenaRules>(m_rules);
        if (!game.network().authority()) {
            m_view.tick(game, float(seconds), rules.finished);
            return;
        }
        if (m_paused || rules.finished) return;
        m_sim->stepCombat();
        m_view.tick(game, float(seconds), rules.finished);
    }

    void frame(GameContext& game, double frameDt) override {
        auto& input = game.input();
        net::NetworkWorld& network = game.network();
        if (input.wasActionPressed("arena_menu")) pause(game, !m_paused);
        if (input.wasActionPressed("arena_restart")) requestRestart(game);
        const auto& rules = *game.world.get<ArenaRules>(m_rules);
        const Entity pawn = localPawn(game);
        const auto* fighter = game.world.alive(pawn) ? game.world.get<ArenaFighter>(pawn) : nullptr;
        const auto* weapon = game.world.alive(pawn) ? game.world.get<ArenaWeapon>(pawn) : nullptr;
        if (!m_menuOpen && !rules.autoPlay && !rules.finished && input.wasActionPressed("arena_fire") &&
            !input.isMouseCaptured() && !game.ui().capturesInput())
            input.setMouseCaptured(true);
        if (!network.authority() && fighter && weapon) {
            // Shown from the host's state: a shot the host made spends a round.
            if (m_seenPawn == pawn && weapon->ammo < m_seenAmmo) {
                m_view.kick();
                game.audio().playClip("Audio/arena-shot.wav", {.volume = 0.35f});
            }
            if (m_seenPawn == pawn && fighter->health < m_seenHealth) m_view.hurt();
            m_seenPawn = pawn;
            m_seenAmmo = weapon->ammo;
            m_seenHealth = fighter->health;
        }
        if (m_match) m_view.showBodies(game.world, pawn, !network.authority());
        ArenaHud hud;
        hud.blue = rules.blue;
        hud.orange = rules.orange;
        hud.seconds = int(std::ceil(double(rules.remainingTicks) / m_sim->tickRate));
        hud.finished = rules.finished;
        if (fighter && weapon) {
            hud.health = int(std::ceil(fighter->health));
            hud.ammo = weapon->ammo;
            hud.magazine = weapon->magazine;
            hud.kills = fighter->kills;
            hud.deaths = fighter->deaths;
            hud.reloading = weapon->reloadTicks > 0;
            hud.alive = fighter->health > 0;
        }
        hud.status = status(game, rules, fighter, weapon);
        const bool autoAim = rules.autoPlay && fighter;
        m_view.frame(
            game, frameDt, pawn, autoAim ? fighter->yaw : m_input.yaw(), autoAim ? fighter->pitch : m_input.pitch(),
            !m_paused && !rules.finished, hud);
        if (m_match) scoreboard(game);
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
    // ---- Who's who ----

    // The local player's pawn: the offline scene's player, or the one this machine owns.
    Entity localPawn(GameContext& game) const {
        if (!m_match) return m_player;
        return pawnOf(game, game.network().localPlayer());
    }
    Entity pawnOf(GameContext& game, uint16_t slot) const {
        Entity found;
        if (slot == 0) return found;
        game.world.each<const ArenaFighter>([&](Entity e, const ArenaFighter& f) {
            if (f.slot == slot) found = e;
        });
        return found;
    }
    uint32_t incarnation(GameContext& game, Entity pawn) const {
        const auto* identity = game.world.alive(pawn) ? game.network().identity(pawn) : nullptr;
        return identity ? identity->incarnation : 0;
    }

    void hookUp(GameContext& game) {
        ArenaSimHooks& hooks = m_sim->hooks;
        hooks.shot = [this, &game](Entity shooter, glm::vec3 origin, glm::vec3 end, Entity) {
            m_view.shot(game, origin, end, shooter == localPawn(game));
        };
        hooks.hit = [this, &game](Entity shooter, Entity victim) {
            const Entity local = localPawn(game);
            if (shooter == local) m_view.hit();
            if (victim == local) m_view.hurt();
        };
        hooks.killed = [this, &game](Entity killer, Entity victim) {
            const auto& shooter = *game.world.get<ArenaFighter>(killer);
            m_view.setFeed(std::string(teamName(shooter.team)) + " eliminated " + fighterName(game.world, victim));
            if (killer == localPawn(game)) game.audio().playClip("Audio/arena-hit.wav", {.volume = 0.3f});
        };
        hooks.reloaded = [this, &game](Entity pawn) {
            if (pawn == localPawn(game)) game.audio().playClip("Audio/arena-reload.wav", {.volume = 0.25f});
        };
        hooks.respawned = [&game](Entity pawn) {
            // A new life (or a teleport): clients drop anything from the old one.
            if (game.network().online() && game.network().identity(pawn)) (void)game.network().respawn(pawn);
        };
    }

    // ---- The authority's tick ----

    void controls(GameContext& game, float dt) {
        net::NetworkWorld& network = game.network();
        const auto& rules = *game.world.get<ArenaRules>(m_rules);
        const Entity local = localPawn(game);
        const uint32_t life = incarnation(game, local);
        // Exactly one command per tick from the local devices, so presses go to one tick.
        InputCommand mine = m_input.next(life);
        if (rules.autoPlay && game.world.alive(local)) mine = m_sim->autoCommand(local, life);
        if (m_paused) mine = InputCommand{};
        if (!network.authority()) {
            clientTick(game, local, mine);
            return;
        }
        m_sim->tickRate = uint32_t(std::lround(1.0 / dt));
        if (m_match) {
            handleEvents(game);
            handleCommands(game);
        }
        if (m_paused) {
            game.world.each<const ArenaFighter, CharacterController>([](const ArenaFighter& f, CharacterController& c) {
                if (ArenaSim::isPawn(f)) c.desiredVelocity = {};
            });
            return;
        }
        // One command per pawn per tick: the local one, or what its player sent.
        std::vector<std::pair<Entity, uint16_t>> remote;
        game.world.each<ArenaFighter>([&](Entity e, ArenaFighter& f) {
            if (!ArenaSim::isPawn(f)) return;
            if (f.player || f.slot == network.localPlayer())
                f.input = mine;
            else
                remote.emplace_back(e, f.slot);
        });
        for (const auto& [e, slot] : remote) {
            InputCommand c;
            network.takeInput(slot, incarnation(game, e), c);
            game.world.get<ArenaFighter>(e)->input = c;
        }
        m_sim->stepControls(dt);
    }

    void clientTick(GameContext& game, Entity local, const InputCommand& mine) {
        net::NetworkWorld& network = game.network();
        if (network.state() != net::NetworkState::Active || !game.world.alive(local)) return;
        network.sendInput(network.localInputSuppressed() ? InputCommand{.incarnation = mine.incarnation} : mine);
    }

    void handleEvents(GameContext& game) {
        net::NetworkEvent event;
        while (game.network().pollEvent(event)) {
            if (event.kind == net::NetworkEventKind::PlayerJoined) joinPlayer(game, event.slot);
            if (event.kind == net::NetworkEventKind::PlayerLeft) leavePlayer(game, event.slot);
        }
    }

    void handleCommands(GameContext& game) {
        net::ReceivedCommand c;
        while (game.network().pollCommand(c))
            command(game, c.senderSlot, c.commandId, c.command->payload.empty() ? 0 : int(c.payload.fields[0].u));
    }

    // A new player gets a pawn on the team with room, at a free spawn point.
    void joinPlayer(GameContext& game, uint16_t slot) {
        net::NetworkWorld& network = game.network();
        const int team = teamWithRoom(game.world);
        glm::vec3 at{0};
        if (team < 0 || !spawnPoint(game.world, team, kNullEntity, at)) {
            SW_LOG(LogGame, Warning, "Arena: no room for player {}", slot);
            network.disconnect(slot, net::ReasonCode::ServerFull);
            return;
        }
        Transform place;
        place.position = at;
        Entity pawn;
        if (network.online()) {
            auto spawned = network.spawn(m_prefabId, place, slot);
            if (!spawned) {
                SW_LOG(LogGame, Error, "Arena: couldn't spawn player {}: {}", slot, spawned.error().message);
                network.disconnect(slot, net::ReasonCode::ServiceUnavailable);
                return;
            }
            pawn = spawned.value();
        } else {
            pawn = game.spawn(kNetPlayerPrefab, at).entity();
            if (!game.world.alive(pawn))
                throw std::runtime_error(std::string("Arena couldn't spawn ") + kNetPlayerPrefab);
        }
        auto* f = game.world.get<ArenaFighter>(pawn);
        if (!f || !game.world.has<CharacterController>(pawn) || !game.world.has<ArenaWeapon>(pawn))
            throw std::runtime_error(
                std::string(kNetPlayerPrefab) + " needs ArenaFighter, CharacterController and ArenaWeapon");
        f->team = team;
        f->slot = slot;
        f->spawn = at;
        f->health = f->maxHealth;
        f->protectionTicks = ticksFor(kProtectionSeconds, m_sim->tickRate);
        f->ready = false;
        SW_LOG(LogGame, Info, "Arena: player {} joined {}", slot, teamName(team));
    }

    void leavePlayer(GameContext& game, uint16_t slot) {
        const Entity pawn = pawnOf(game, slot);
        if (!game.world.alive(pawn)) return;
        if (auto gone = game.network().despawn(pawn); !gone) destroyWithChildren(game.world, pawn);
        SW_LOG(LogGame, Info, "Arena: player {} left", slot);
        maybeStart(game);
    }

    // The match commands, whoever sent them: the authority's rules decide.
    void command(GameContext& game, uint16_t sender, uint16_t id, int value) {
        auto& r = m_sim->match();
        const Entity pawn = pawnOf(game, sender);
        auto* f = game.world.alive(pawn) ? game.world.get<ArenaFighter>(pawn) : nullptr;
        switch (id) {
        case kRequestTeam: {
            if (!f || r.phase != ArenaRules::Waiting || f->team == value) return;
            if (teamCount(game.world, value, pawn) >= kTeamSize) return;
            glm::vec3 at;
            if (!spawnPoint(game.world, value, pawn, at)) return;
            f->team = value;
            f->spawn = at;
            m_sim->respawn(pawn, *f);
            return;
        }
        case kReadyToPlay:
            if (!f || r.phase != ArenaRules::Waiting) return;
            f->ready = value != 0;
            maybeStart(game);
            return;
        case kRequestRoundRestart:
            if (sender != 1 || r.phase != ArenaRules::Finished) return;
            restart(game);
            return;
        default: return;
        }
    }

    void maybeStart(GameContext& game) {
        auto& r = m_sim->match();
        if (r.phase != ArenaRules::Waiting) return;
        int players = 0, ready = 0;
        game.world.each<const ArenaFighter>([&](const ArenaFighter& f) {
            if (f.slot == 0) return;
            ++players;
            ready += f.ready;
        });
        if (players > 0 && ready == players) restart(game);
    }

    void restart(GameContext& game) {
        auto& r = m_sim->match();
        ++r.roundSerial;
        m_sim->startRound();
        m_view.reset(game);
        m_input.setAim(0, 0);
        m_view.setFeed("FIRST TO " + std::to_string(r.scoreLimit) + " / BLUE VS ORANGE");
    }

    // ---- Local requests ----

    static net::ProfileValue payload(int value) {
        net::ProfileValue v;
        v.fields[0].u = uint64_t(value);
        return v;
    }

    // Menu and key actions: the same commands a client sends, through the network world
    // when online so the host checks them like anyone's.
    void request(GameContext& game, uint16_t id, int value) {
        if (!m_match) return;
        if (game.network().online()) {
            if (auto sent = game.network().submitCommand(id, 0, payload(value)); !sent)
                SW_LOG(LogGame, Info, "Arena: {}", sent.error().message);
            return;
        }
        command(game, 1, id, value);
    }

    void requestRestart(GameContext& game) {
        if (!m_match) {
            restart(game);   // the offline bot match restarts any time
            return;
        }
        if (game.network().authority()) {
            if (game.network().online())
                (void)game.network().submitCommand(kRequestRoundRestart, 0, {});
            else
                restart(game);
        }
    }

    GraphicsMenu m_graphicsMenu;
    void pause(GameContext& game, bool paused) {
        m_graphicsMenu.close(game);
        m_paused = paused && !game.network().online();   // online the match keeps running
        m_menuOpen = paused;
        game.network().setLocalInputSuppressed(paused);
        game.ui().set("arena_menu_open", paused);
        if (paused) {
            game.ui().show("Arena Menu");
            game.input().setMouseCaptured(false);
        } else {
            game.ui().hide("Arena Menu");
            game.input().setMouseCaptured(true);
        }
    }

    // ---- HUD ----

    std::string
    status(GameContext& game, const ArenaRules& rules, const ArenaFighter* fighter, const ArenaWeapon* weapon) {
        const net::NetworkWorld& network = game.network();
        if (network.state() == net::NetworkState::Failed)
            return std::string("DISCONNECTED / ") +
                   (net::reasonName(network.failure()) ? net::reasonName(network.failure()) : "ERROR");
        if (const char* joining =
                network.role() == net::NetworkRole::Client ? connectingText(network.state()) : nullptr)
            return joining;
        if (network.online() && network.state() != net::NetworkState::Active) return "OFFLINE";
        std::string status = "CLICK TO CAPTURE MOUSE";
        if (game.input().isMouseCaptured()) status = "WASD MOVE / SHIFT SPRINT / SPACE JUMP / R RELOAD / P MENU";
        if (rules.autoPlay) status = m_match ? "AUTOPLAY / MATCH" : "AUTOPLAY / LOCAL BOT MATCH";
        if (rules.phase == ArenaRules::Waiting) status = "WAITING FOR PLAYERS / P MENU TO READY UP";
        if (weapon && weapon->reloadTicks > 0) status = "RELOADING";
        if (fighter && fighter->health <= 0) {
            const int seconds = int(std::ceil(double(fighter->respawnTicks) / m_sim->tickRate));
            status = network.authority() ? "ELIMINATED / RESPAWNING IN " + std::to_string(std::max(seconds, 0))
                                         : "ELIMINATED / RESPAWNING";
        }
        if (rules.finished)
            status = (rules.blue == rules.orange  ? "DRAW"
                      : rules.blue > rules.orange ? "BLUE WINS"
                                                  : "ORANGE WINS") +
                     std::string(network.authority() ? " / F2 TO RESTART" : " / WAITING FOR THE HOST");
        // A prediction that had to snap says so for a few seconds.
        if (!network.predictionWarning().empty()) status = "CONNECTION PROBLEM / POSITION CORRECTED";
        return status;
    }

    // Twelve rows, six a side, for the menu's scoreboard.
    void scoreboard(GameContext& game) {
        std::array<std::array<std::string, kTeamSize>, 2> rows;
        std::array<int, 2> used{0, 0};
        const uint16_t me = game.network().localPlayer();
        for (uint16_t slot = 1; slot <= net::kMaxPlayers; ++slot) {
            const Entity pawn = pawnOf(game, slot);
            if (!game.world.alive(pawn)) continue;
            const auto& f = *game.world.get<ArenaFighter>(pawn);
            const int team = std::clamp(f.team, 0, 1);
            if (used[team] == kTeamSize) continue;
            rows[team][used[team]++] = (slot == me ? "> PLAYER " : "PLAYER ") + std::to_string(slot) + "  " +
                                       std::to_string(f.kills) + " / " + std::to_string(f.deaths);
        }
        for (int team = 0; team < 2; ++team)
            for (int i = 0; i < kTeamSize; ++i)
                game.ui().set(
                    std::string(team == 0 ? "arena_blue_" : "arena_orange_") + std::to_string(i + 1),
                    rows[team][i].empty() ? std::string("-") : rows[team][i]);
        std::string net = "OFFLINE MATCH";
        if (game.network().online())
            net = std::string(game.network().authority() ? "HOSTING" : "CONNECTED") + " / " +
                  std::to_string(game.network().stats().rttMs > 0 ? int(game.network().stats().rttMs) : 0) + " MS";
        game.ui().set("arena_network", net);
    }

    std::optional<ArenaSim> m_sim;
    ArenaPresentation m_view;
    InputSampler m_input;
    Entity m_rules, m_player, m_camera, m_gun, m_flash;
    Entity m_seenPawn;
    int m_seenAmmo = 0;
    float m_seenHealth = 0;
    uint16_t m_prefabId = 0;
    bool m_match = false, m_paused = false, m_menuOpen = false;
};

std::unique_ptr<GameMode> createGame() { return std::make_unique<ArenaGame>(); }
}   // namespace arena
