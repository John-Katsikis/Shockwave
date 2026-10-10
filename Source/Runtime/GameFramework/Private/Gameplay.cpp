#include <GameFramework/Gameplay.h>
#include <Physics/Physics.h>
#include <Engine/Engine.h>
#include <Engine/Systems.h>
#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace shockwave {
GameSession::GameSession(World& world, JobSystem& jobs, GameplayFactory factory, Engine* engine)
    : m_context{world, jobs, engine}, m_factory(std::move(factory)) {
    if (!m_factory) throw std::invalid_argument("GameSession requires a gameplay factory");
}
GameSession::~GameSession() {
    try { stop(); }
    catch (const std::exception& e) { std::fprintf(stderr, "Gameplay cleanup: %s\n", e.what()); }
    catch (...) { std::fprintf(stderr, "Gameplay cleanup failed\n"); }
}
void GameSession::start() {
    if (m_state == State::Playing) return;
    if (m_state == State::Paused) { m_state = State::Playing; return; }
    m_snapshot.emplace(m_context.world);
    m_ticks = 0;
    try {
        m_physics=std::make_unique<PhysicsWorld>(m_context.world,m_context.jobs,m_context.engine ? &m_context.engine->assets() : nullptr);
        m_context.physics=m_physics.get();
        m_game = m_factory();
        if (!m_game) throw std::runtime_error("Gameplay factory returned null");
        m_schedule = Schedule{};
        addBuiltinSystems(m_schedule);
        m_game->configureSystems(m_context, m_schedule);
        m_game->onStart(m_context);
        m_physics->synchronize();
        storePreviousTransforms(m_context.world, m_context.jobs);
        m_state = State::Playing;
    } catch (...) {
        const auto failure = std::current_exception();
        if (m_game) { try { m_game->onStop(m_context); } catch (...) {} }
        m_physics.reset();m_context.physics=nullptr;
        m_context.world = std::move(*m_snapshot);
        m_snapshot.reset(); m_game.reset(); m_schedule = Schedule{};
        std::rethrow_exception(failure);
    }
}
void GameSession::pause() {
    if (m_state == State::Playing) m_state = State::Paused;
}
void GameSession::stop() {
    if (!active()) return;
    std::exception_ptr failure;
    try { m_game->onStop(m_context); } catch (...) { failure = std::current_exception(); }
    m_physics.reset();m_context.physics=nullptr;
    m_context.world = std::move(*m_snapshot);
    m_snapshot.reset(); m_game.reset(); m_schedule = Schedule{};
    m_state = State::Editing;
    if (failure) std::rethrow_exception(failure);
}
void GameSession::update(double dt) {
    if (m_state != State::Playing) return;
    if (!std::isfinite(dt) || dt <= 0) throw std::invalid_argument("Simulation dt must be positive and finite");
    storePreviousTransforms(m_context.world, m_context.jobs);
    m_schedule.run(m_context.world, m_context.jobs, dt);
    m_physics->step(dt);
    m_game->onPhysicsStep(m_context,dt);
    ++m_ticks;
}
void GameSession::frame(double dt) {
    if (m_state == State::Playing) m_game->onFrame(m_context, dt);
}
} // namespace shockwave
