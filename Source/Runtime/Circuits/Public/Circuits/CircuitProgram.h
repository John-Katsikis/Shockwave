#pragma once

// Compiling circuits: a CircuitDocument checked and turned into register bytecode, which
// every entity running the circuit shares (each keeps only its registers).
//
// The bytecode is a list of instructions. Each node that runs on an exec wire gets a block:
// "eval" instructions for the pure nodes feeding it, in dependency order, then one "fire"
// that runs it and jumps to the block of whatever its chosen exec output leads to. An
// event's entry is the block its "then" leads to. `player --circuit file --dump` prints it.

#include <Circuits/Circuit.h>
#include <Circuits/CircuitNodes.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shockwave {

struct CircuitProblem {
    int line = 0;
    std::string message;
    bool warning = false;   // unreachable nodes: reported, but the circuit still runs
};

struct CircuitProgram {
    enum class Op : uint8_t {
        Eval,   // a: call site; runs a pure node
        Fire,   // a: call site, b: exec input; runs a node, then follows its exec output
        IntToFloat,   // a: from register, b: to register
    };
    struct Instruction {
        Op op = Op::Eval;
        uint32_t a = 0, b = 0;
    };
    struct CallSite {
        std::shared_ptr<const NodeDesc> node;
        std::string name;   // the node's name in the file
        int line = 0;
        std::vector<uint32_t> inputs, outputs;   // a register per pin
        uint32_t state = 0;   // first state register
        std::vector<int32_t> exec;   // per exec output: the pc of the block it leads to; -1 none
        std::vector<uint8_t> given;   // per input: connected or given a value (Component.X.Set sets only those)
    };
    struct EventEntry {
        CircuitEvent event = CircuitEvent::None;
        std::string param;   // ActionPressed/Released: the action; UICommand: the command
        uint32_t site = 0;   // the event node's call site, whose outputs carry the event's data
    };
    struct Variable {
        std::string name;
        PinType type = PinType::Float;
        uint32_t reg = 0;
    };

    std::string path;   // inside Content
    CircuitKind kind = CircuitKind::Entity;
    std::vector<Instruction> code;
    std::vector<CallSite> sites;
    std::vector<EventEntry> events;
    std::vector<Variable> variables;   // their registers come first
    std::vector<Value> registers;   // initial values: variables' defaults, literals, outputs
    // Component ids the circuit reads and writes, so a parallel mode can schedule it.
    std::vector<uint32_t> reads, writes;
    uint64_t nodeVersion = 0;   // nodeRegistryVersion() it was compiled against

    const Variable* findVariable(std::string_view name) const;
    bool handles(CircuitEvent event) const;
};

// Checks document and compiles it. Returns null when there are errors; problems gets
// errors and warnings either way.
std::shared_ptr<const CircuitProgram> compileCircuit(
    const CircuitDocument& document, std::string path, CircuitKind kind, std::vector<CircuitProblem>& problems);
// The nodes a circuit may not use when it runs with this execution: Authority allows
// everything, LocalOwner presentation nodes and network requests, Presentation only
// presentation nodes. Each problem names the node and its line; none means it may run.
std::vector<CircuitProblem> circuitScopeProblems(const CircuitProgram& program, CircuitExecution execution);
// The bytecode as text, one instruction per line, for player --circuit --dump.
std::string dumpCircuit(const CircuitProgram& program);

// Loads, compiles and caches .swcircuit files from Content (prefabs().contentFolder()).
// One per process (circuitLibrary()); thread-safe.
class CircuitLibrary {
public:
    // The compiled circuit, or null. problems gets "file:line: message" lines, warnings
    // included, with file as the project names it under Content.
    std::shared_ptr<const CircuitProgram>
    load(std::string_view path, CircuitKind kind, std::vector<std::string>* problems = nullptr);
    std::filesystem::path file(std::string_view path) const;
    // Compiles changed files again, and everything when nodes or components changed. At
    // most every interval seconds; pass the frame time. Returns the paths compiled again.
    std::vector<std::string> poll(double frameDt, double interval = 0.5);
    // Bumps whenever a circuit is compiled again: running instances then move to it.
    uint64_t generation() const;
    void clear();

private:
    struct Entry {
        std::shared_ptr<const CircuitProgram> program;
        std::vector<std::string> problems;
        std::filesystem::file_time_type modified{};
        uint64_t nodeVersion = 0;
        bool exists = false;
    };
    Entry compile(const std::string& path, CircuitKind kind) const;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;   // key: kind, then path
    uint64_t m_generation = 1;
    double m_pollTimer = 0.0;
};

CircuitLibrary& circuitLibrary();

}   // namespace shockwave
