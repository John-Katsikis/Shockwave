// player --physics-test: plays a scene headless at several tick rates and compares them,
// so you can see what a higher tick rate buys (bodies landing where they should, nothing
// tunnelling through walls) and what it costs in CPU time. See docs/Physics.md.
#include "Player.h"

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/SceneFile.h>
#include <GameFramework/PlaySession.h>
#include <Physics/Physics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace player {

namespace {

struct RateResult {
    double rate = 0;
    int ticks = 0;
    double averageTickMs = 0, worstTickMs = 0;
    double cpuPerSecondMs = 0;   // simulation time one second of play takes
    std::map<std::string, glm::vec3> positions;   // dynamic bodies, by name
    std::map<std::string, glm::quat> rotations;
};

std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

}   // namespace

int Player::physicsTest() {
    std::vector<double> rates = m_tool.tickRates;
    if (rates.empty()) rates = {30, 60, 120, 240, 512};
    std::sort(rates.begin(), rates.end());
    const fs::path scene = m_tool.scene.empty() ? m_project.startupScene : m_tool.scene;
    const double seconds = m_tool.audioSeconds;   // --seconds
    std::printf(
        "Physics at %zu tick rates: %s, %.1f s of play each, %u worker threads\n", rates.size(),
        m_project.relative(scene).c_str(), seconds, engine().jobs().threadCount());

    std::vector<RateResult> results;
    std::vector<std::string> bodies;   // dynamic bodies, in scene order
    for (const double rate : rates) {
        shockwave::World world;
        std::string error;
        std::vector<std::string> warnings;
        shockwave::SceneFileHooks hooks;
        hooks.readHeader = [](const std::string&, std::istream&) {};
        if (!shockwave::loadScene(world, engine().assets(), scene.string(), error, hooks, &warnings)) {
            std::fprintf(stderr, "Player: %s\n", error.c_str());
            return 1;
        }
        RateResult result;
        result.rate = rate;
        result.ticks = std::max(1, static_cast<int>(std::lround(seconds * rate)));
        const double dt = 1.0 / rate;
        try {
            shockwave::PlaySession session(world, engine().jobs(), *m_instance, &engine());
            session.start();
            // Frames as the game would render them, about 60 a second, between the ticks.
            const int ticksPerFrame = std::max(1, static_cast<int>(std::lround(rate / 60.0)));
            double total = 0.0;
            for (int i = 0; i < result.ticks; ++i) {
                const auto start = std::chrono::steady_clock::now();
                session.update(dt);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                total += ms;
                result.worstTickMs = std::max(result.worstTickMs, ms);
                if ((i + 1) % ticksPerFrame == 0) session.frame(dt * ticksPerFrame);
            }
            result.averageTickMs = total / result.ticks;
            result.cpuPerSecondMs = total / seconds;
            world.each<const shockwave::RigidBody>([&](shockwave::Entity e, const shockwave::RigidBody& body) {
                if (body.motion != shockwave::RigidBody::Motion::Dynamic || !world.has<shockwave::Transform>(e)) return;
                const auto* name = world.get<shockwave::Name>(e);
                const std::string key = name && !name->value.empty() ? name->value : "#" + std::to_string(e.index);
                const shockwave::Transform placed = shockwave::Transform::fromMatrix(shockwave::worldMatrix(world, e));
                result.positions[key] = placed.position;
                result.rotations[key] = placed.rotation;
                if (results.empty()) bodies.push_back(key);
            });
            session.stop();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Player: at %g ticks a second, gameplay failed: %s\n", rate, e.what());
            return 1;
        }
        results.push_back(std::move(result));
    }

    // ---- Cost ----
    std::printf("\n  rate  tick (avg)  tick (worst)  CPU per second of play  headroom\n");
    for (const auto& r : results) {
        const double headroom = r.cpuPerSecondMs > 0 ? 1000.0 / r.cpuPerSecondMs : 0.0;
        std::printf(
            "  %4.0f  %8.3f ms  %9.3f ms  %18.1f ms  %7.1fx%s\n", r.rate, r.averageTickMs, r.worstTickMs,
            r.cpuPerSecondMs, headroom, headroom < 1.0 ? "  can't keep up: the game would slow down" : "");
    }

    // ---- Outcome, compared with the highest rate ----
    const RateResult& reference = results.back();
    std::printf(
        "\n  Where each body ended up: its position at %.0f Hz, and how far off the others are\n", reference.rate);
    size_t nameWidth = 4;
    for (const auto& name : bodies) nameWidth = std::max(nameWidth, name.size());
    std::printf("  %-*s  %-24s", int(nameWidth), "body", "at the highest rate");
    for (size_t i = 0; i + 1 < results.size(); ++i) std::printf("  %11.0f Hz", results[i].rate);
    std::printf("\n");
    for (const auto& name : bodies) {
        const auto ref = reference.positions.find(name);
        if (ref == reference.positions.end()) continue;
        char at[64];
        std::snprintf(at, sizeof(at), "(%.2f, %.2f, %.2f)", ref->second.x, ref->second.y, ref->second.z);
        std::printf("  %-*s  %-24s", int(nameWidth), name.c_str(), at);
        for (size_t i = 0; i + 1 < results.size(); ++i) {
            const auto p = results[i].positions.find(name);
            if (p == results[i].positions.end()) {
                std::printf("  %14s", "gone");
                continue;
            }
            // Distance, and the turn between the two rotations when it's more than a degree.
            const glm::quat a = results[i].rotations.at(name), b = reference.rotations.at(name);
            const float turn = glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(a, b)))));
            char cell[32];
            if (turn >= 1.0f)
                std::snprintf(cell, sizeof(cell), "%.2f m %3.0f deg", glm::distance(p->second, ref->second), turn);
            else
                std::snprintf(cell, sizeof(cell), "%.2f m", glm::distance(p->second, ref->second));
            std::printf("  %14s", cell);
        }
        std::printf("\n");
    }
    std::printf(
        "\n  Large differences mean a body behaved differently at that rate (fell, bounced, turned\n"
        "  or passed through something). Watch one with player --scene %s --tick-rate N.\n",
        m_project.relative(scene).c_str());

    if (!m_tool.benchmarkJson.empty()) {
        std::ofstream json(m_tool.benchmarkJson, std::ios::trunc);
        json << "{\n  \"scene\": \"" << escape(m_project.relative(scene)) << "\",\n  \"seconds\": " << seconds
             << ",\n  \"rates\": [\n";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            json << "    {\"tick_rate\": " << r.rate << ", \"ticks\": " << r.ticks
                 << ", \"average_tick_ms\": " << r.averageTickMs << ", \"worst_tick_ms\": " << r.worstTickMs
                 << ", \"cpu_ms_per_second\": " << r.cpuPerSecondMs << ", \"bodies\": {";
            bool first = true;
            for (const auto& [name, p] : r.positions) {
                json << (first ? "" : ", ") << "\"" << escape(name) << "\": [" << p.x << ", " << p.y << ", " << p.z
                     << "]";
                first = false;
            }
            json << "}}" << (i + 1 < results.size() ? "," : "") << "\n";
        }
        json << "  ]\n}\n";
        std::printf("  Wrote %s\n", m_tool.benchmarkJson.string().c_str());
    }
    return 0;
}

}   // namespace player
