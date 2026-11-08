#include <GameFramework/PlaySession.h>

#include <Animation/AnimationWorld.h>
#include <Audio/Audio.h>
#include <Circuits/CircuitWorld.h>
#include <Core/Log.h>
#include <Effects/EffectsWorld.h>
#include <Engine/Engine.h>
#include <Engine/Systems.h>
#include <GameFramework/SaveGame.h>
#include <GameUI/UIWorld.h>
#include <Navigation/Navigation.h>
#include <Physics/Physics.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace shockwave {

Graphics& GameContext::graphics() const { return engine ? engine->graphics() : subsystems.get<Graphics>(); }
void GameContext::cutCamera() const {
    if (engine) engine->renderer().cutCamera();
}

AudioWorld& GameContext::audio() const { return subsystems.get<AudioWorld>(); }
AudioWorld* PlaySession::audio() const { return m_subsystems.find<AudioWorld>(); }
EffectsWorld& GameContext::effects() const { return subsystems.get<EffectsWorld>(); }
EffectsWorld* PlaySession::effects() const { return m_subsystems.find<EffectsWorld>(); }
AnimationWorld& GameContext::animation() const { return subsystems.get<AnimationWorld>(); }
AnimationWorld* PlaySession::animation() const { return m_subsystems.find<AnimationWorld>(); }

PhysicsWorld& GameContext::physics() const { return subsystems.get<PhysicsWorld>(); }
UIWorld& GameContext::ui() const { return subsystems.get<UIWorld>(); }
UIWorld* PlaySession::ui() const { return m_subsystems.find<UIWorld>(); }
NavigationWorld& GameContext::navigation() const { return subsystems.get<NavigationWorld>(); }
NavigationWorld* PlaySession::navigation() const { return m_subsystems.find<NavigationWorld>(); }
CircuitWorld* PlaySession::circuits() const { return m_subsystems.find<CircuitWorld>(); }
net::NetworkWorld& GameContext::network() const { return subsystems.get<net::NetworkWorld>(); }
net::NetworkWorld* PlaySession::network() const { return m_subsystems.find<net::NetworkWorld>(); }
bool PlaySession::online() const {
    const net::NetworkWorld* network = this->network();
    return network && network->online();
}

Input& GameContext::input() const {
    if (engine) return engine->input();
    static Input none;   // never polled, so nothing is ever pressed
    return none;
}

PlaySession::PlaySession(World& world, JobSystem& jobs, GameInstance& instance, Engine* engine)
    : m_instance(instance),
      m_context{world, jobs, engine, m_subsystems, nullptr, StartReason::Fresh, 0, nullptr, {}, &m_ownCampaign} {}

PlaySession::~PlaySession() {
    try {
        stop();
    } catch (const std::exception& e) {
        SW_LOG(LogGame, Error, "Play cleanup: {}", e.what());
    } catch (...) {
        SW_LOG(LogGame, Error, "Play cleanup failed");
    }
}

PhysicsWorld* PlaySession::physics() const { return m_subsystems.find<PhysicsWorld>(); }

namespace {
// Circuits ask through the session's game.flow(), so their requests carry its epoch.
class SessionTravel final : public CircuitTravel {
public:
    explicit SessionTravel(const GameContext& game) : m_game(game) {}
    Ticket request(const std::string& level, const std::string& spawn) override {
        return ticket(m_game.flow().request(level, spawn));
    }
    Ticket restartLevel() override { return ticket(m_game.flow().restartLevel()); }
    Ticket returnToMenu() override { return ticket(m_game.flow().returnToMenu()); }
    Ticket useExit(Entity exit) override { return ticket(m_game.flow().useExit(exit)); }
    Result reachCheckpoint(const std::string& id) override { return result(m_game.saves().checkpoint(id)); }
    Result restartCheckpoint() override { return result(m_game.saves().restartCheckpoint()); }
    Ticket finishLevel(const std::string& level, const std::string& spawn) override {
        return ticket(m_game.finishLevel(level, spawn));
    }
    bool completeOnce(const std::string& id) override { return m_game.campaign().complete(id); }
    bool campaignHas(const std::string& key) const override { return m_game.campaign().has(key); }
    std::string campaignGet(const std::string& key) const override { return m_game.campaign().get(key); }
    void campaignSet(const std::string& key, const std::string& value) override {
        m_game.campaign().set(key, std::string_view(value));
    }
    Status status(int32_t id) const override {
        Status s;
        if (id <= 0) return s;
        const auto progress = m_game.flow().status(uint64_t(id));
        if (!progress) return s;
        s.known = true;
        s.phase = travelPhaseName(progress->phase);
        s.progress = progress->progress;
        s.code = travelErrorName(progress->code);
        s.message = progress->message;
        s.cancellable = progress->cancellable;
        return s;
    }

private:
    Result result(bool ok) const { return {ok, ok ? "None" : saveErrorName(saveErrorOf(m_game.saves().lastError()))}; }
    static Ticket ticket(const TravelTicket& t) {
        return {t.id <= uint64_t(INT32_MAX) ? int32_t(t.id) : 0, travelErrorName(t.error), t.message};
    }
    const GameContext& m_game;
};
}   // namespace

void PlaySession::start() {
    if (m_state == State::Playing) return;
    if (m_state == State::Paused) {
        audio()->mixer().pause(false);
        m_state = State::Playing;
        return;
    }
    World& world = m_context.world;
    if (m_worldPolicy == SessionWorldPolicy::RestoreAuthoredSnapshot) m_snapshot.emplace(world);
    m_ticks = 0;
    try {
        if (!m_context.engine)
            m_subsystems.add<Graphics>();
        else if (m_context.engine->guiEnabled() && m_settingsPolicy == SessionSettingsPolicy::RestoreOnStop)
            m_graphicsSnapshot = m_context.graphics().requested();
        m_subsystems.add<PhysicsWorld>(world, m_context.jobs, m_context.engine ? &m_context.engine->assets() : nullptr);
        m_subsystems.add<AudioWorld>(world, m_context.engine ? &m_context.engine->assets() : nullptr, m_audioDevice);
        // Occlusion rays hit the physics world's colliders. Looked up per ray: loading a save
        // replaces the physics world.
        audio()->setRaycast(
            [this](glm::vec3 origin, glm::vec3 direction, float distance, Entity ignore) -> std::optional<AudioRayHit> {
                if (const auto hit = physics()->raycast(origin, direction, distance, ignore))
                    return AudioRayHit{hit->entity, hit->distance};
                return std::nullopt;
            });
        if (m_context.engine) applyVolumes();
        // Finds or bakes the navigation mesh, when the scene has NavAgents or NavMeshSettings.
        m_subsystems.add<NavigationWorld>(
            world, m_context.jobs, m_context.engine ? &m_context.engine->assets() : nullptr);
        audio()->synchronize();
        m_subsystems.add<EffectsWorld>(world, Effects::forEngine(m_context.engine));
        m_subsystems.add<UIWorld>(world, GameUI::forEngine(m_context.engine));
        m_subsystems.add<AnimationWorld>(
            world, m_context.engine ? &m_context.engine->assets() : nullptr, m_context.jobs);
        SaveGames& saves = m_subsystems.add<SaveGames>(
            world, m_context.jobs, m_context.engine ? &m_context.engine->assets() : nullptr, m_saveFolder, m_saveScene);
        saves.setIdentity(m_saveIdentity);
        saves.setCampaign(m_context.campaignState);
        if (m_context.flowHost)
            saves.setLevel(m_context.flowHost->currentLevel().level, m_context.flowHost->currentLevel().spawn);
        // After physics, so it is destroyed before physics; before the GameMode, so
        // beginPlay sees its role. A host listens and a client connects here.
        // Offline it only spawns manifest prefabs as plain copies.
        auto& network = m_subsystems.add<net::NetworkWorld>(m_networkStart, m_services, &world);
        network.setPhysics(physics());   // its network mode follows the role
        if (network.online()) {
            if (auto begun = network.begin(); !begun)
                throw std::runtime_error(std::string("network play didn't start: ") + begun.error().message);
            saves.setNetworkPlay(true);
        }
        saves.beforeSave = [this] {
            if (m_mode) m_mode->beforeSave(m_context);
            // The game circuit's variables go into the save's values, after the GameMode's.
            if (CircuitWorld* circuits = this->circuits())
                circuits->beforeSave([this](const std::string& key, const std::string& value) {
                    this->saves()->set(key, std::string_view(value));
                });
        };
        m_mode = m_instance.createGameMode(world);
        if (!m_mode) throw std::runtime_error("the GameInstance made no GameMode");
        m_schedule = Schedule{};
        addBuiltinSystems(m_schedule);
        m_mode->configureSystems(m_context, m_schedule);
        m_mode->beginPlay(m_context);
        // Circuits run after the C++ GameMode's hooks, from here on.
        CircuitHost host;
        host.input = &m_context.input();
        host.audio = audio();
        host.ui = ui();
        host.physics = physics();
        host.assets = m_context.engine ? &m_context.engine->assets() : nullptr;
        host.network = &network;
        if (m_context.flowHost) {
            m_circuitTravel = std::make_unique<SessionTravel>(m_context);
            host.travel = m_circuitTravel.get();
        }
        CircuitWorld& circuits = m_subsystems.add<CircuitWorld>(world, host, m_gameCircuit);
        circuits.setBreakpointsAllowed(!online());
        circuits.beginPlay();
        physics()->synchronize();
        navigation()->synchronize();
        audio()->synchronize();
        storePreviousTransforms(world, m_context.jobs);
        m_state = State::Playing;
    } catch (...) {
        const auto failure = std::current_exception();
        if (m_mode) {
            try {
                m_mode->endPlay(m_context);
            } catch (...) {}
        }
        tearDown();
        std::rethrow_exception(failure);
    }
}

bool PlaySession::pause() {
    if (m_state != State::Playing) return m_state == State::Paused;
    if (online()) {
        SW_LOG(LogGame, Warning, "Pause is off during network play: the session keeps running");
        return false;
    }
    audio()->mixer().pause(true);
    m_state = State::Paused;
    return true;
}

void PlaySession::stop() {
    if (!active()) return;
    std::exception_ptr failure;
    try {
        // The network goes first: peers are told, and nothing arrives during endPlay.
        if (net::NetworkWorld* network = this->network()) network->close();
        m_mode->endPlay(m_context);
        if (CircuitWorld* circuits = this->circuits()) circuits->endPlay();
    } catch (...) {
        failure = std::current_exception();
    }
    tearDown();
    if (failure) std::rethrow_exception(failure);
}

void PlaySession::tearDown() {
    if (m_graphicsSnapshot) {
        std::string error;
        m_context.graphics().apply(*m_graphicsSnapshot, error);
        m_graphicsSnapshot.reset();
    }
    m_mode.reset();
    m_subsystems.clear();
    m_circuitTravel.reset();
    if (m_snapshot) m_context.world = std::move(*m_snapshot);   // OwnedRuntimeWorld keeps what Play left
    m_snapshot.reset();
    m_schedule = Schedule{};
    m_state = State::Editing;
}

void PlaySession::update(double dt) {
    if (m_state != State::Playing || (circuits() && circuits()->debugPaused())) return;
    if (!std::isfinite(dt) || dt <= 0) throw std::invalid_argument("simulation dt must be positive and finite");
    network()->drain(uint32_t(m_ticks));   // what arrived since the last pump or tick
    applyLoadedSave();
    storePreviousTransforms(m_context.world, m_context.jobs);
    m_schedule.run(m_context.world, m_context.jobs, dt);
    navigation()->step(dt);   // agents steer first, so physics moves their characters
    network()->prepareStep();   // which characters move as clients predict them
    physics()->step(dt);
    m_mode->fixedUpdate(m_context, dt);
    circuits()->fixedTick(dt);
    audio()->update(dt);
    audio()->synchronize();
    saves()->addPlayTime(dt);
    network()->endTick(uint32_t(m_ticks));   // the host sends this tick's state
    ++m_ticks;
}

void PlaySession::frame(double dt) {
    if (circuits() && circuits()->debugPaused()) return;
    if (active()) applyLoadedSave();   // a load from a pause menu goes in while paused
    if (m_state == State::Playing && !(circuits() && circuits()->debugPaused())) {
        // A client's predicted and interpolated poses, before game code places a camera.
        network()->present(m_context.engine ? float(m_context.engine->interpolationAlpha()) : 1.0f);
        m_mode->frame(m_context, dt);
        circuits()->frame(dt);
        animation()->update(dt);   // after game code, so a clip it picks shows this frame
        audio()->synchronize();
    }
    if (active() && m_context.engine) applyVolumes();
}

// The player's volumes (docs/Preferences.md): master, then each category on top.
void PlaySession::applyVolumes() {
    auto& mixer = audio()->mixer();
    mixer.setMasterVolume(m_context.engine->masterVolume());
    for (int c = 0; c < kAudioCategories; ++c)
        mixer.setCategoryVolume(AudioCategory(c), m_context.engine->categoryVolume(c));
}

void PlaySession::pumpNetwork(double nowSeconds) {
    if (net::NetworkWorld* network = this->network()) network->pump(nowSeconds);
}

bool PlaySession::handleOnlineEvent(const net::OnlineEvent& event) {
    net::NetworkWorld* network = this->network();
    return network && network->handleOnlineEvent(event);
}

void PlaySession::collectInput(double frameDt) {
    if (m_state == State::Playing && m_mode) m_mode->collectInput(m_context, frameDt);
}

SaveGames& GameContext::saves() const { return subsystems.get<SaveGames>(); }

TravelTicket GameContext::finishLevel(std::string next, std::string spawn) const {
    TravelTicket ticket = flow().request(next, spawn);
    // Accepted: the campaign as it is now, and where it goes, in one autosave. A crash
    // before the next level starts resumes there, the completion kept and given once.
    if (ticket) {
        SaveGames* saves = subsystems.find<SaveGames>();
        if (saves && !saves->networkPlay() && !saves->folder().empty())
            saves->saveTransition(kAutosaveSlot, std::move(next), std::move(spawn));
    }
    return ticket;
}
SaveGames* PlaySession::saves() const { return m_subsystems.find<SaveGames>(); }

// A finished load replaces the world between ticks. A save from another level travels
// there first (LevelHost brings it back through restore()); one from this level, or from
// before saves knew their level, goes in here.
void PlaySession::applyLoadedSave() {
    SaveGames* saves = this->saves();
    if (!saves) return;
    std::optional<SaveRestore> loaded = saves->takeLoaded();
    if (!loaded) return;
    const std::string& level = loaded->info.level;
    // A fresh save (between levels) starts its level as authored, so it always travels.
    const bool fresh = loaded->info.fresh;
    if (!level.empty() && m_context.flowHost && (fresh || level != m_context.flowHost->currentLevel().level)) {
        TravelRequest request;
        request.level = level;
        request.spawn = loaded->info.spawn;
        request.reason = loaded->info.checkpoint.empty() ? TravelReason::LoadSave : TravelReason::Checkpoint;
        request.fromEpoch = m_context.levelEpoch;
        request.restore = std::make_shared<SaveRestore>(std::move(*loaded));
        const TravelTicket ticket = m_context.flowHost->request(std::move(request));
        if (!ticket)
            saves->fail(
                "the save is from level \"" + level + "\", and going there was refused: " +
                std::string(travelErrorName(ticket.error)) + ": " + ticket.message);
        return;
    }
    applySave(std::move(*loaded));
}

void PlaySession::restore(SaveRestore restore) {
    if (!active() || !saves()) throw std::logic_error("PlaySession::restore needs a started session");
    applySave(std::move(restore));
}

// Physics bodies belong to the old entities, so physics starts over from the loaded
// components; audio and UI keep going and catch up on their next synchronize.
void PlaySession::applySave(SaveRestore restore) {
    SaveGames* saves = this->saves();
    saves->apply(std::move(restore));
    World& world = m_context.world;
    m_subsystems.replace<PhysicsWorld>(world, m_context.jobs, m_context.engine ? &m_context.engine->assets() : nullptr);
    physics()->synchronize();
    circuits()->setPhysics(physics());
    audio()->synchronize();
    storePreviousTransforms(world, m_context.jobs);
    m_mode->afterLoad(m_context);
    circuits()->afterLoad([saves](const std::string& key, std::string& value) {
        if (!saves->has(key)) return false;
        value = saves->get(key);
        return true;
    });
}

}   // namespace shockwave
