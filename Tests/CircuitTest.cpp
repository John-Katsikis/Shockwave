// Circuits: the file format, the compiler's checks, and circuits running in a CircuitWorld
// (no window, no physics; docs/Circuits.md).
#include <Assets/Assets.h>
#include <Circuits/Circuit.h>
#include <Circuits/CircuitNodes.h>
#include <Circuits/CircuitProgram.h>
#include <Circuits/CircuitWorld.h>
#include <Engine/Components.h>
#include <Engine/Prefab.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

struct Health {
    float current = 100;
    float damage(float amount) {
        current -= amount;
        return current;
    }
};

// Compiles text and returns its problems, joined, for matching.
std::string problemsOf(std::string_view text, CircuitKind kind, bool* compiled = nullptr) {
    CircuitDocument document;
    std::string error;
    if (!parseCircuitText(text, "test.swcircuit", document, error)) {
        if (compiled) *compiled = false;
        return error;
    }
    std::vector<CircuitProblem> problems;
    const auto program = compileCircuit(document, "test.swcircuit", kind, problems);
    if (compiled) *compiled = program != nullptr;
    std::string all;
    for (const auto& p : problems)
        all += std::to_string(p.line) + (p.warning ? " warning " : " error ") + p.message + "\n";
    return all;
}

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

const char* kMover = R"(shockwave_circuit 1
# Moves up at speed units a second and counts ticks; adds 1 to 4 into total once.
var ticks Int 0
var total Int 0
var speed Float 2

node start Event.BeginPlay

node sum Flow.ForLoop
  exec start.then
  in first = 1
  in last = 4

node add Set.total
  exec sum.body
  in value = plus.result

node plus Math.AddInt
  in a = total
  in b = sum.index

node tick Event.FixedTick

node seq Flow.Sequence
  exec tick.then

node move Component.Transform.Set.position
  exec seq.then0
  in position = next.result

node next Vector.Add
  in a = pos.position
  in b = step.result

node pos Component.Transform.Get.position

node step Vector.Scale
  in vector = 0 1 0
  in scale = scaled.result

node scaled Math.Multiply
  in a = tick.dt
  in b = speed

node count Set.ticks
  exec seq.then1
  in value = inc.result

node inc Math.AddInt
  in a = ticks
  in b = 1

editor
  start -400 0
  tick -400 200
end
)";

void format() {
    CircuitDocument document;
    std::string error;
    check(parseCircuitText(kMover, "mover.swcircuit", document, error), error);
    check(document.variables.size() == 3 && document.nodes.size() == 13, "variables and nodes read");
    check(document.nodes[0].placed && near(document.nodes[0].position.x, -400), "editor block positions");
    check(document.description.find("Moves up") == 0, "leading comment kept as the description");
    std::ostringstream out;
    writeCircuit(out, document);
    CircuitDocument again;
    check(parseCircuitText(out.str(), "again.swcircuit", again, error), error);
    check(
        again.nodes.size() == document.nodes.size() && again.nodes[1].execs.size() == 1 &&
            again.nodes[3].inputs.size() == 2 && again.nodes[0].placed,
        "write then read gives the same circuit");
    check(out.str().rfind("editor") > out.str().rfind("node "), "positions come after the logic");

    check(
        !parseCircuitText("node a Event.BeginPlay\n", "x", document, error) && error.find("x:1:") == 0,
        "header required, with file:line");
    check(
        !parseCircuitText("shockwave_circuit 1\nin a = 1\n", "x", document, error) && error.find("x:2:") == 0,
        "in outside a node");
    check(!parseCircuitText("shockwave_circuit 1\nnode a T\nnode a T\n", "x", document, error), "duplicate node names");

    Value value;
    check(parseValue("1 2 3", PinType::Vec3, value, error) && near(value.v.y, 2), "vector literal");
    check(parseValue("\"Door\"", PinType::Entity, value, error) && value.text == "Door", "entity by name");
    check(parseValue("self", PinType::Entity, value, error) && value.text == "self", "self");
    check(!parseValue("maybe", PinType::Bool, value, error), "bad bool rejected");
    check(formatValue(Value::string("a \"b\"")) == "\"a \\\"b\\\"\"", "strings are quoted and escaped");
    const auto vars = parseCircuitVars("a=1; name=\"x; y=2\"; b = true");
    check(vars.size() == 3 && vars[1].second == "\"x; y=2\"" && vars[2].first == "b", "CircuitVars respects quotes");
    check(formatCircuitVars(vars).find("a=1; name=") == 0, "CircuitVars written back");
}

void compilerChecks() {
    bool compiled = true;
    std::string p = problemsOf("shockwave_circuit 1\nnode x Nope.Nothing\n", CircuitKind::Entity, &compiled);
    check(
        !compiled && p.find("2 error") == 0 && p.find("unknown node type") != std::string::npos, "unknown node: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode t Event.FixedTick\nnode say Debug.Print\n  exec t.then\n  in text = t.dt\n",
        CircuitKind::Entity, &compiled);
    check(!compiled && p.find("wants String but t.dt is Float") != std::string::npos, "type mismatch: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode b Event.BeginPlay\nnode s Entity.Spawn\n  exec b.then\n", CircuitKind::Entity,
        &compiled);
    check(!compiled && p.find("input 'prefab' needs a value") != std::string::npos, "required input: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode a1 Math.Add\n  in a = a2.result\nnode a2 Math.Add\n  in a = a1.result\n",
        CircuitKind::Entity, &compiled);
    check(!compiled && p.find("data loop") != std::string::npos, "data loop: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode b Event.BeginPlay\nnode x Debug.Print\n  exec b.then\nnode y Debug.Print\n  exec "
        "b.then\n",
        CircuitKind::Entity, &compiled);
    check(!compiled && p.find("already leads to node 'x'") != std::string::npos, "one exec output, one node: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode u Event.UICommand\n  in command = \"resume\"\n", CircuitKind::Entity, &compiled);
    check(!compiled && p.find("can't be used in an entity circuit") != std::string::npos, "game-only event: " + p);
    p = problemsOf(
        "shockwave_circuit 1\nnode u Event.UICommand\n  in command = \"resume\"\n", CircuitKind::Game, &compiled);
    check(compiled, "game circuits take UI commands: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nvar n Int\nnode u Event.ActionPressed\n  in action = n\n", CircuitKind::Entity,
        &compiled);
    check(!compiled && p.find("only a literal") != std::string::npos, "constant pins take literals: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode lonely Debug.Print\n  in text = \"hi\"\n", CircuitKind::Entity, &compiled);
    check(
        compiled && p.find("warning") != std::string::npos && p.find("never runs") != std::string::npos,
        "unreachable nodes warn but compile: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode b Event.BeginPlay\nnode s Set.missing\n  exec b.then\n  in value = 1\n",
        CircuitKind::Entity, &compiled);
    check(!compiled && p.find("no variable 'missing'") != std::string::npos, "unknown variable node: " + p);

    p = problemsOf(
        "shockwave_circuit 1\nnode b Event.BeginPlay\nnode s Debug.Print\n  exec b.then\n  in txt = \"x\"\n",
        CircuitKind::Entity, &compiled);
    check(!compiled && p.find("has no input 'txt' (it has: text)") != std::string::npos, "unknown pin: " + p);

    // Int goes into Float pins; the dump shows the conversion.
    CircuitDocument document;
    std::string error;
    check(
        parseCircuitText(
            "shockwave_circuit 1\nvar n Int 3\nvar f Float\nnode b Event.BeginPlay\nnode s Set.f\n  exec b.then\n"
            "  in value = n\n",
            "convert.swcircuit", document, error),
        error);
    std::vector<CircuitProblem> problems;
    const auto program = compileCircuit(document, "convert.swcircuit", CircuitKind::Entity, problems);
    check(program && dumpCircuit(*program).find("float") != std::string::npos, "Int to Float conversion");
}

void running(const fs::path& content) {
    write(content / "Circuits/mover.swcircuit", kMover);
    World world;
    const Entity e = world.create();
    world.add<Name>(e, {"Mover"});
    world.add<Transform>(e);
    world.add<Circuit>(e, {"Circuits/mover.swcircuit"});
    const Entity other = world.create();
    world.add<Transform>(other);
    world.add<Circuit>(other, {"Circuits/mover.swcircuit"});
    world.add<CircuitVars>(other, {"speed=4; ticks=10"});   // per-entity values from the scene
    CircuitWorld circuits(world, CircuitHost{});
    circuits.beginPlay();
    check(circuits.diagnostics().empty(), circuits.diagnostics().empty() ? "" : circuits.diagnostics().front());
    check(circuits.instanceCount() == 2, "an instance per entity");
    const Value* total = circuits.variable(e, "total");
    check(total && total->i == 10, "ForLoop and Set: 1 + 2 + 3 + 4");
    circuits.fixedTick(0.5);
    check(near(world.get<Transform>(e)->position.y, 1.0f), "Sequence then0 moves it: speed 2 x 0.5 s");
    check(near(world.get<Transform>(other)->position.y, 2.0f), "CircuitVars gave the other one speed 4");
    const Value* ticks = circuits.variable(e, "ticks");
    check(ticks && ticks->i == 1, "Sequence then1 counts the tick");
    check(circuits.variable(other, "ticks")->i == 11, "counting starts from the scene's value");
    const CircuitVars* vars = world.get<CircuitVars>(e);
    check(
        vars && vars->values.find("ticks=1") != std::string::npos &&
            vars->values.find("total=10") != std::string::npos && vars->values.find("speed") == std::string::npos,
        "changed variables are written to CircuitVars: " + (vars ? vars->values : std::string("none")));

    // Disabling the Circuit drops the instance; destroying the entity too.
    world.get<Circuit>(other)->enabled = false;
    circuits.fixedTick(0.5);
    check(
        circuits.instanceCount() == 1 && near(world.get<Transform>(other)->position.y, 2.0f), "disabled circuits stop");
    circuits.endPlay();
}

void components(const fs::path& content) {
    registerComponent<Health>("Health", {field("current", &Health::current)});
    check(findNode("Component.Health.Set.current") != nullptr, "reflected nodes for a new component");
    write(content / "Circuits/hurt.swcircuit", R"(shockwave_circuit 1
node start Event.BeginPlay
node hurt Component.Health.Set.current
  exec start.then
  in current = 25
node twice Flow.DoOnce
  exec start.then
)");
    // That second exec from start.then is an error; fix it and try again.
    std::vector<std::string> problems;
    check(
        !circuitLibrary().load("Circuits/hurt.swcircuit", CircuitKind::Entity, &problems) && !problems.empty() &&
            problems.front().find("Circuits/hurt.swcircuit:") == 0,
        "library reports file:line problems");
    write(content / "Circuits/hurt.swcircuit", R"(shockwave_circuit 1
node start Event.BeginPlay
node hurt Component.Health.Set
  exec start.then
  in current = 25
)");
    circuitLibrary().clear();
    World world;
    const Entity e = world.create();
    world.add<Health>(e);
    world.add<Circuit>(e, {"Circuits/hurt.swcircuit"});
    CircuitWorld circuits(world, CircuitHost{});
    circuits.beginPlay();
    check(near(world.get<Health>(e)->current, 25), "Component.Health.Set writes the field given");
}

void functionNodes(const fs::path& content) {
    const uint64_t before = nodeRegistryVersion();
    const uint64_t token =
        registerNode(detail::makeFunctionNode("Test", "Double", [](float x) { return x * 2.0f; }, true, {"x"}));
    check(nodeRegistryVersion() != before, "registering a node changes the version");
    const auto node = findNode("Test.Double");
    check(
        node && node->pure() && node->inputs.size() == 1 && node->inputs[0].name == "x" && node->outputs.size() == 1 &&
            node->outputs[0].type == PinType::Float,
        "pins come from the function");
    write(content / "Circuits/double.swcircuit", R"(shockwave_circuit 1
var out Float
node start Event.BeginPlay
node d Test.Double
  in x = 21
node keep Set.out
  exec start.then
  in value = d.result
)");
    World world;
    const Entity e = world.create();
    world.add<Circuit>(e, {"Circuits/double.swcircuit"});
    CircuitWorld circuits(world, CircuitHost{});
    circuits.beginPlay();
    const Value* out = circuits.variable(e, "out");
    check(out && near(out->v.x, 42), "a C++ function node runs");
    unregisterNode("Test.Double", token + 1);
    check(findNode("Test.Double") != nullptr, "another token can't remove it");
    unregisterNode("Test.Double", token);
    check(findNode("Test.Double") == nullptr, "its own token removes it");
}

void authoringAndDebugging(const fs::path& content) {
    CircuitDocument source;
    std::string error;
    check(parseCircuitText(kMover, "mover", source, error), error);
    source.nodes.front().note = "Starts the loop; quoted \"note\"";
    std::ostringstream out;
    writeCircuit(out, source);
    CircuitDocument restored;
    check(parseCircuitText(out.str(), "notes", restored, error), error);
    check(restored.nodes.front().note == source.nodes.front().note, "node notes round-trip");
    check(
        !parseCircuitText("shockwave_circuit 1\neditor\n@note missing \"bad\"\nend\n", "notes", restored, error),
        "unknown note target is rejected");
    CircuitDocument pasted = source;
    appendCircuitNodes(pasted, source, {40, 40});
    std::vector<CircuitProblem> problems;
    check(
        compileCircuit(pasted, "pasted", CircuitKind::Entity, problems) != nullptr,
        "duplicated graph compiles with remapped wires");
    check(pasted.nodes[source.nodes.size()].name == "start_2", "paste names are unique");
    check(pasted.nodes[source.nodes.size() + 1].execs.front().node == "start_2", "paste remaps exec wires");
    check(pasted.nodes[source.nodes.size()].note == source.nodes.front().note, "paste keeps notes");
    check(findNode("Data.Reroute.Vec3") != nullptr, "typed reroute node exists");
    CircuitDocument conflict;
    conflict.variables.push_back({"ticks", PinType::String, "\"hello\""});
    appendCircuitNodes(conflict, source, {0, 0});
    problems.clear();
    check(
        compileCircuit(conflict, "conflict", CircuitKind::Entity, problems) != nullptr,
        "paste renames conflicting variable types");
    check(
        std::any_of(
            conflict.nodes.begin(), conflict.nodes.end(), [](const auto& n) { return n.type == "Set.ticks_2"; }),
        "paste remaps variable node types");
    check(
        !findNode("Component.RigidBody.Set.motion")->inputs[1].options.empty(),
        "enum labels propagate from component metadata");
    CircuitDocument badEnum;
    check(
        parseCircuitText(
            "shockwave_circuit 1\nnode start Event.BeginPlay\nnode set Component.RigidBody.Set.motion\n exec "
            "start.then\n in motion = 99\n",
            "enum", badEnum, error),
        error);
    problems.clear();
    check(!compileCircuit(badEnum, "enum", CircuitKind::Entity, problems), "compiler rejects invalid enum literal");

    write(content / "Circuits/debug.swcircuit", kMover);
    circuitLibrary().clear();
    World world;
    const Entity e = world.create();
    world.add<Transform>(e);
    world.add<Circuit>(e, {"Circuits/debug.swcircuit"});
    CircuitWorld circuits(world, {});
    circuits.setTracing(true);
    circuits.setBreakpoint("Circuits/debug.swcircuit", "add", true);
    circuits.beginPlay();
    check(circuits.debugPaused() && circuits.debugLocation().node == "add", "breakpoint pauses before mutation");
    check(circuits.variable(e, "total")->i == 0, "breakpoint preserves variable before write");
    circuits.fixedTick(1);
    circuits.frame(1);
    check(circuits.variable(e, "ticks")->i == 0, "events do not run while paused");
    circuits.debugContinue(true);
    check(circuits.debugPaused() && circuits.debugLocation().node == "sum", "single step resumes loop stack");
    check(circuits.variable(e, "total")->i == 1, "single step executes one node");
    circuits.setBreakpoint("Circuits/debug.swcircuit", "add", false);
    circuits.debugContinue();
    check(!circuits.debugPaused() && circuits.variable(e, "total")->i == 10, "continue finishes nested loop correctly");
    check(!circuits.trace().empty() && !circuits.trace().front().inputs.empty(), "trace records input snapshots");
    circuits.clearTrace();
    circuits.setTracing(false);
    circuits.fixedTick(0.1);
    check(circuits.trace().empty(), "disabled tracing creates no history");
    circuits.setTracing(true);
    for (int i = 0; i < 100; ++i) circuits.fixedTick(0.1);
    check(circuits.trace().size() == 256, "trace history is bounded");
    circuits.setBreakpoint("Circuits/debug.swcircuit", "move", true);
    circuits.fixedTick(0.1);
    check(circuits.debugPaused(), "tick breakpoint pauses");
    world.destroy(e);
    circuits.debugContinue();
    check(!circuits.debugPaused(), "destroyed debug target cancels safely");
}

void gameCircuit(const fs::path& content) {
    write(content / "Circuits/game.swcircuit", R"(shockwave_circuit 1
var score Int 0
var loads Int 0
node start Event.BeginPlay
node set Set.score
  exec start.then
  in value = 5
node loaded Event.AfterLoad
node count Set.loads
  exec loaded.then
  in value = more.result
node more Math.AddInt
  in a = loads
  in b = 1
)");
    World world;
    CircuitWorld circuits(world, CircuitHost{}, "Circuits/game.swcircuit");
    circuits.beginPlay();
    check(circuits.diagnostics().empty(), circuits.diagnostics().empty() ? "" : circuits.diagnostics().front());
    const Value* score = circuits.variable(kNullEntity, "score");
    check(score && score->i == 5, "the game circuit runs BeginPlay");
    std::map<std::string, std::string> saved;
    circuits.beforeSave([&](const std::string& key, const std::string& value) { saved[key] = value; });
    check(saved["circuit.score"] == "5", "game circuit variables go into the save's values");
    saved["circuit.score"] = "42";
    circuits.afterLoad([&](const std::string& key, std::string& value) {
        const auto found = saved.find(key);
        if (found == saved.end()) return false;
        value = found->second;
        return true;
    });
    check(circuits.variable(kNullEntity, "score")->i == 42, "and come back on load");
    check(circuits.variable(kNullEntity, "loads")->i == 1, "AfterLoad runs after they're back");
}
void gameplayMessages(const fs::path& content) {
    write(content / "Circuits/commands.swcircuit", R"(shockwave_circuit 1
var received Float 0
node open Event.Message
  in message = "Open"
node move Transform.SetPosition
  exec open.then
  in position = 0 2 0
node record Set.received
  exec move.then
  in value = open.amount
node again Circuit.Send
  exec record.then
  in message = "Open"
  in amount = 7
)");
    auto method = detail::componentMethod("Combat", "Damage", &Health::damage, "amount");
    const auto token = registerNode(std::move(method));
    write(content / "Circuits/damage.swcircuit", R"(shockwave_circuit 1
node start Event.BeginPlay
node damage Combat.Damage
  exec start.then
  in amount = 12
)");
    circuitLibrary().clear();
    World world;
    Entity target = world.create();
    world.add<Transform>(target);
    world.add<Circuit>(target, {"Circuits/commands.swcircuit"});
    CircuitWorld circuits(world, {});
    check(!circuits.send(target, "Open"), "commands reject stopped sessions");
    circuits.beginPlay();
    check(!circuits.send(target, "Unknown"), "commands reject missing methods");
    check(circuits.send(target, "Open", target, 3), "command finds named handler");
    check(near(world.get<Transform>(target)->position.y, 0), "commands never execute recursively");
    circuits.fixedTick(0.01);
    check(near(world.get<Transform>(target)->position.y, 2), "gameplay method moves target");
    check(near(circuits.variable(target, "received")->v.x, 3), "typed payload reaches handler");
    circuits.fixedTick(0.01);
    check(near(circuits.variable(target, "received")->v.x, 7), "self command waits until next boundary");
    for (int i = 0; i < 1023; ++i) check(circuits.send(target, "Open"), "queue accepts bounded commands");
    check(!circuits.send(target, "Open"), "queue refuses overflow");
    circuits.endPlay();
    const float saved = circuits.variable(target, "received")->v.x;
    circuits.beginPlay();
    circuits.fixedTick(0.01);
    check(near(circuits.variable(target, "received")->v.x, saved), "Stop cancels pending commands");
    Entity victim = world.create();
    world.add<Health>(victim);
    world.add<Circuit>(victim, {"Circuits/damage.swcircuit"});
    circuits.frame(0.01);
    check(near(world.get<Health>(victim)->current, 88), "component member method invoked with typed arguments");
    Entity invalid = world.create();
    world.add<Circuit>(invalid, {"Circuits/damage.swcircuit"});
    circuits.frame(0.01);
    check(
        std::any_of(
            circuits.diagnostics().begin(), circuits.diagnostics().end(),
            [](const auto& problem) { return problem.find("missing its required component") != std::string::npos; }),
        "missing method receiver becomes a diagnostic");
    unregisterNode("Combat.Damage", token);
    write(content / "Circuits/rate.swcircuit", R"(shockwave_circuit 1
node tick Event.FixedTick
node spin Transform.RotateByRate
  exec tick.then
  in speed = 90
  in dt = tick.dt
)");
    Entity spinner = world.create();
    world.add<Transform>(spinner);
    world.add<Circuit>(spinner, {"Circuits/rate.swcircuit"});
    circuits.fixedTick(1.0);
    const auto forward = world.get<Transform>(spinner)->rotation * glm::vec3(0, 0, 1);
    check(near(forward.x, 1) && near(forward.z, 0), "rate action applies degrees per second with event dt");
    world.destroy(target);
    check(!circuits.send(target, "Open"), "stale entity commands rejected");
    check(
        problemsOf("shockwave_circuit 1\nnode event Event.Message\n in message = \"\"\n", CircuitKind::Entity)
                .find("must not be empty") != std::string::npos,
        "empty method name fails check");
}

}   // namespace

int main() {
    registerCircuitComponents();
    const fs::path root =
        fs::temp_directory_path() /
        ("shockwave-circuit-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path content = root / "Content";
    fs::create_directories(content);
    Assets assets;
    prefabs().setup(content, &assets);
    format();
    compilerChecks();
    running(content);
    components(content);
    functionNodes(content);
    gameCircuit(content);
    authoringAndDebugging(content);
    gameplayMessages(content);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::puts("Circuits: format, compiler checks, events, variables and nodes passed");
    return failures == 0 ? 0 : 1;
}
