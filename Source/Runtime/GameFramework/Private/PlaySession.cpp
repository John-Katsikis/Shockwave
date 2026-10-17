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

#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace shockwave {

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

Input& GameContext::input() const {
    if (engine) return engine->input();
    static Input none;   // never polled, so nothing is ever pressed
    return none;
}

PlaySession::PlaySession(World& world, JobSystem& jobs, GameInstance& instance, Engine* engine)
    : m_instance(instance), m_context{world, jobs, engine, m_subsystems} {}

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

void PlaySession::start() {
    if (m_state == State::Playing) return;
    if (m_state == State::Paused) {
        audio()->mixer().pause(false);
        m_state = State::Playing;
        return;
    }
    World& world = m_context.world;
    m_snapshot.emplace(world);
    m_ticks = 0;
    try {
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
        if (m_context.engine) audio()->mixer().setMasterVolume(m_context.engine->masterVolume());
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
        m_subsystems.add<CircuitWorld>(world, host, m_gameCircuit).beginPlay();
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

void PlaySession::pause() {
    if (m_state == State::Playing) {
        audio()->mixer().pause(true);
        m_state = State::Paused;
    }
}

void PlaySession::stop() {
    if (!active()) return;
    std::exception_ptr failure;
    try {
        m_mode->endPlay(m_context);
        if (CircuitWorld* circuits = this->circuits()) circuits->endPlay();
    } catch (...) {
        failure = std::current_exception();
    }
    tearDown();
    if (failure) std::rethrow_exception(failure);
}

void PlaySession::tearDown() {
    m_mode.reset();
    m_subsystems.clear();
    m_context.world = std::move(*m_snapshot);
    m_snapshot.reset();
    m_schedule = Schedule{};
    m_state = State::Editing;
}

void PlaySession::update(double dt) {
    if (m_state != State::Playing || (circuits() && circuits()->debugPaused())) return;
    if (!std::isfinite(dt) || dt <= 0) throw std::invalid_argument("simulation dt must be positive and finite");
    applyLoadedSave();
    storePreviousTransforms(m_context.world, m_context.jobs);
    m_schedule.run(m_context.world, m_context.jobs, dt);
    navigation()->step(dt);   // agents steer first, so physics moves their characters
    physics()->step(dt);
    m_mode->fixedUpdate(m_context, dt);
    circuits()->fixedTick(dt);
    audio()->update(dt);
    audio()->synchronize();
    saves()->addPlayTime(dt);
    ++m_ticks;
}

void PlaySession::frame(double dt) {
    if (circuits() && circuits()->debugPaused()) return;
    if (active()) applyLoadedSave();   // a load from a pause menu goes in while paused
    if (m_state == State::Playing && !(circuits() && circuits()->debugPaused())) {
        m_mode->frame(m_context, dt);
        circuits()->frame(dt);
        animation()->update(dt);   // after game code, so a clip it picks shows this frame
        audio()->synchronize();
    }
    if (active() && m_context.engine) audio()->mixer().setMasterVolume(m_context.engine->masterVolume());
}

SaveGames& GameContext::saves() const { return subsystems.get<SaveGames>(); }
SaveGames* PlaySession::saves() const { return m_subsystems.find<SaveGames>(); }

// A finished load replaces the world between ticks. Physics bodies belong to the old
// entities, so physics starts over from the loaded components; audio and UI keep going
// and catch up on their next synchronize.
void PlaySession::applyLoadedSave() {
    SaveGames* saves = this->saves();
    if (!saves || !saves->applyLoaded()) return;
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
