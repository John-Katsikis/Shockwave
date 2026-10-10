#include <GameFramework/PlaySession.h>

#include <Engine/Engine.h>
#include <Engine/Systems.h>
#include <Physics/Physics.h>

#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace shockwave {

PhysicsWorld& GameContext::physics() const { return subsystems.get<PhysicsWorld>(); }

PlaySession::PlaySession(World& world, JobSystem& jobs, GameInstance& instance, Engine* engine)
    : m_instance(instance), m_context{world, jobs, engine, m_subsystems} {}

PlaySession::~PlaySession() {
    try {
        stop();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Play cleanup: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "Play cleanup failed\n");
    }
}

PhysicsWorld* PlaySession::physics() const { return m_subsystems.find<PhysicsWorld>(); }

void PlaySession::start() {
    if (m_state == State::Playing) return;
    if (m_state == State::Paused) {
        m_state = State::Playing;
        return;
    }
    World& world = m_context.world;
    m_snapshot.emplace(world);
    m_ticks = 0;
    try {
        m_subsystems.add<PhysicsWorld>(world, m_context.jobs, m_context.engine ? &m_context.engine->assets() : nullptr);
        m_mode = m_instance.createGameMode(world);
        if (!m_mode) throw std::runtime_error("the GameInstance made no GameMode");
        m_schedule = Schedule{};
        addBuiltinSystems(m_schedule);
        m_mode->configureSystems(m_context, m_schedule);
        m_mode->beginPlay(m_context);
        physics()->synchronize();
        storePreviousTransforms(world, m_context.jobs);
        m_state = State::Playing;
    } catch (...) {
        const auto failure = std::current_exception();
        if (m_mode) {
            try { m_mode->endPlay(m_context); } catch (...) {}
        }
        tearDown();
        std::rethrow_exception(failure);
    }
}

void PlaySession::pause() {
    if (m_state == State::Playing) m_state = State::Paused;
}

void PlaySession::stop() {
    if (!active()) return;
    std::exception_ptr failure;
    try {
        m_mode->endPlay(m_context);
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
    if (m_state != State::Playing) return;
    if (!std::isfinite(dt) || dt <= 0) throw std::invalid_argument("simulation dt must be positive and finite");
    storePreviousTransforms(m_context.world, m_context.jobs);
    m_schedule.run(m_context.world, m_context.jobs, dt);
    physics()->step(dt);
    m_mode->fixedUpdate(m_context, dt);
    ++m_ticks;
}

void PlaySession::frame(double dt) {
    if (m_state == State::Playing) m_mode->frame(m_context, dt);
}

} // namespace shockwave
