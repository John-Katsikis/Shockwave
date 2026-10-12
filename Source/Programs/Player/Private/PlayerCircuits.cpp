// The player's circuit tools: --check for circuits, --node-schema and --circuit --dump.
#include "Player.h"

#include <Circuits/Circuit.h>
#include <Circuits/CircuitNodes.h>
#include <Circuits/CircuitProgram.h>
#include <Circuits/CircuitWorld.h>
#include <Engine/Components.h>
#include <Engine/Prefab.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace player {

void Player::checkCircuits(
    shockwave::World& world, const std::string& scene, std::vector<std::string>& problems,
    std::vector<std::string>& usedCircuits) {
    const std::string shownScene = m_project.relative(scene);
    world.each<const shockwave::Circuit>([&](shockwave::Entity e, const shockwave::Circuit& circuit) {
        if (world.has<shockwave::PrefabPart>(e)) return;   // checked with its prefab's file
        const shockwave::Name* name = world.get<shockwave::Name>(e);
        const std::string where = shownScene + ": entity '" + (name ? name->value : "?") + "': Circuit";
        if (circuit.circuit.empty()) {
            problems.push_back(where + " names no circuit");
            return;
        }
        std::error_code ec;
        if (!fs::is_regular_file(shockwave::circuitLibrary().file(circuit.circuit), ec)) {
            problems.push_back(
                where + ": " + circuit.circuit + " doesn't exist in " + m_project.relative(m_project.assetFolder));
            return;
        }
        usedCircuits.push_back(circuit.circuit);
        // Variables set in the scene must be ones the circuit declares, with values of its type.
        const auto program = shockwave::circuitLibrary().load(circuit.circuit, shockwave::CircuitKind::Entity);
        const auto* vars = world.get<shockwave::CircuitVars>(e);
        if (!program || !vars) return;
        for (const auto& [variable, text] : shockwave::parseCircuitVars(vars->values)) {
            const auto* declared = program->findVariable(variable);
            if (!declared) {
                problems.push_back(where + "Vars: " + circuit.circuit + " has no variable '" + variable + "'");
                continue;
            }
            shockwave::Value value;
            std::string why;
            if (!shockwave::parseValue(text, declared->type, value, why))
                problems.push_back(where + "Vars: " + variable + ": " + why);
        }
    });
}

// Every .swcircuit in Content compiles (game code and spawned prefabs can use any of them),
// or with scenes named on the command line, the ones those use. Warnings are reported but
// don't fail the check.
void Player::checkAllCircuits(const std::vector<std::string>& usedCircuits, std::vector<std::string>& problems) {
    const fs::path content = m_project.assetFolder;
    const std::string game = m_project.gameCircuitPath();
    std::vector<std::string> circuits = usedCircuits;
    if (m_tool.scenes.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(content, ec))
            if (entry.is_regular_file(ec) && entry.path().extension() == ".swcircuit")
                circuits.push_back(entry.path().lexically_relative(content).generic_string());
        if (!game.empty()) circuits.push_back(game);
    }
    std::sort(circuits.begin(), circuits.end());
    circuits.erase(std::unique(circuits.begin(), circuits.end()), circuits.end());
    const std::string folder = m_project.relative(content) + "/";
    for (const std::string& path : circuits) {
        const auto kind = path == game ? shockwave::CircuitKind::Game : shockwave::CircuitKind::Entity;
        std::vector<std::string> found;
        const auto program = shockwave::circuitLibrary().load(path, kind, &found);
        size_t errors = 0;
        for (const std::string& problem : found) {
            const bool warning = problem.find(": warning: ") != std::string::npos;
            if (warning)
                std::printf("%s%s\n", folder.c_str(), problem.c_str());
            else {
                problems.push_back(folder + problem);
                ++errors;
            }
        }
        if (program && errors == 0)
            std::printf(
                "ok %s%s (%s circuit, %zu nodes)\n", folder.c_str(), path.c_str(),
                kind == shockwave::CircuitKind::Game ? "game" : "entity", program->sites.size());
        else if (!program && errors == 0)
            problems.push_back(folder + path + ": doesn't compile");
    }
}

int Player::nodeSchema() {
    std::ostringstream out;
    shockwave::writeNodeSchema(out);
    if (m_tool.schemaOut == "-") {
        std::fputs(out.str().c_str(), stdout);
        return 0;
    }
    std::ofstream file(m_tool.schemaOut, std::ios::trunc);
    if (!(file << out.str())) {
        std::fprintf(stderr, "Player: can't write %s\n", m_tool.schemaOut.string().c_str());
        return 1;
    }
    std::printf("Wrote %s\n", m_tool.schemaOut.string().c_str());
    return 0;
}

int Player::dumpCircuit() {
    if (m_tool.circuit.empty()) {
        std::fprintf(stderr, "Player: --dump needs --circuit <file.swcircuit>\n");
        return 1;
    }
    // A name inside Content (Circuits/Door.swcircuit), or a file path, from the working
    // folder or the project's (Content/Circuits/Door.swcircuit).
    std::string path = m_tool.circuit;
    std::error_code ec;
    if (!fs::is_regular_file(shockwave::circuitLibrary().file(path), ec)) {
        for (const fs::path& candidate : {fs::absolute(path, ec), m_project.root / path}) {
            const fs::path file = candidate.lexically_normal();
            if (!fs::is_regular_file(file, ec)) continue;
            const fs::path inside = file.lexically_relative(m_project.assetFolder);
            if (!inside.empty() && *inside.begin() != "..") path = inside.generic_string();
            break;
        }
    }
    const auto kind =
        path == m_project.gameCircuitPath() ? shockwave::CircuitKind::Game : shockwave::CircuitKind::Entity;
    std::vector<std::string> problems;
    const auto program = shockwave::circuitLibrary().load(path, kind, &problems);
    for (const std::string& problem : problems) std::fprintf(stderr, "%s\n", problem.c_str());
    if (!program) return 1;
    std::fputs(shockwave::dumpCircuit(*program).c_str(), stdout);
    return 0;
}

int Player::runCircuit() {
    if (m_tool.circuit.empty()) {
        std::fprintf(stderr, "--run-circuit needs --circuit <Content-relative file>\n");
        return 1;
    }
    const auto kind =
        m_tool.circuit == m_project.gameCircuitPath() ? shockwave::CircuitKind::Game : shockwave::CircuitKind::Entity;
    std::vector<std::string> problems;
    auto program = shockwave::circuitLibrary().load(m_tool.circuit, kind, &problems);
    if (!program) {
        for (const auto& problem : problems) std::fprintf(stderr, "%s\n", problem.c_str());
        return 1;
    }
    if (!m_tool.circuitBreakpoint.empty() &&
        std::none_of(program->sites.begin(), program->sites.end(), [&](const auto& site) {
            return site.name == m_tool.circuitBreakpoint && site.node->event == shockwave::CircuitEvent::None;
        })) {
        std::fprintf(stderr, "Unknown breakpoint node '%s'\n", m_tool.circuitBreakpoint.c_str());
        return 1;
    }
    shockwave::World world;
    const auto entity = world.create();
    world.add<shockwave::Name>(entity, {"CircuitRunner"});
    world.add<shockwave::Transform>(entity);
    if (kind == shockwave::CircuitKind::Entity) world.add<shockwave::Circuit>(entity, {m_tool.circuit});
    shockwave::CircuitWorld circuits(
        world, {.assets = &engine().assets()}, kind == shockwave::CircuitKind::Game ? m_tool.circuit : "");
    circuits.setTracing(true);
    if (!m_tool.circuitBreakpoint.empty()) circuits.setBreakpoint(m_tool.circuit, m_tool.circuitBreakpoint, true);
    circuits.beginPlay();
    for (int i = 0; i < m_tool.circuitTicks && !circuits.debugPaused(); ++i) {
        circuits.fixedTick(1.0 / m_project.tickRate);
        circuits.frame(1.0 / m_project.tickRate);
    }
    for (int i = 0; i < m_tool.circuitSteps && circuits.debugPaused(); ++i) circuits.debugContinue(true);
    for (const auto& entry : circuits.trace()) {
        std::printf("%s:%d node %s\n", entry.circuit.c_str(), entry.line, entry.node.c_str());
        for (const auto& [pin, value] : entry.inputs) std::printf("  in %s = %s\n", pin.c_str(), value.c_str());
        for (const auto& [pin, value] : entry.outputs) std::printf("  out %s = %s\n", pin.c_str(), value.c_str());
    }
    if (circuits.debugPaused()) std::printf("paused %s\n", circuits.debugLocation().node.c_str());
    for (const auto& variable : program->variables)
        if (const auto* value = circuits.variable(
                kind == shockwave::CircuitKind::Game ? shockwave::kNullEntity : entity, variable.name))
            std::printf("variable %s = %s\n", variable.name.c_str(), shockwave::formatValue(*value).c_str());
    for (const auto& problem : circuits.diagnostics()) std::fprintf(stderr, "%s\n", problem.c_str());
    return circuits.diagnostics().empty() ? 0 : 1;
}

}   // namespace player
