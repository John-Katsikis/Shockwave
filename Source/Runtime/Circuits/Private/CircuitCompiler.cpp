// Checking a circuit and turning it into register bytecode; the library that caches them.
#include <Circuits/CircuitProgram.h>

#include <Engine/Prefab.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace shockwave {

const CircuitProgram::Variable* CircuitProgram::findVariable(std::string_view name) const {
    for (const Variable& v : variables)
        if (v.name == name) return &v;
    return nullptr;
}

bool CircuitProgram::handles(CircuitEvent event) const {
    return std::any_of(events.begin(), events.end(), [&](const EventEntry& e) { return e.event == event; });
}

namespace {

bool identifier(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(
        s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool splitReference(std::string_view text, std::string& node, std::string& pin) {
    const size_t dot = text.find('.');
    if (dot == std::string_view::npos) return false;
    if (!identifier(text.substr(0, dot)) || !identifier(text.substr(dot + 1))) return false;
    node = text.substr(0, dot);
    pin = text.substr(dot + 1);
    return true;
}

std::string pinList(const std::vector<PinDesc>& pins) {
    std::string out;
    for (const PinDesc& p : pins) out += (out.empty() ? "" : ", ") + p.name;
    return out.empty() ? "none" : out;
}
std::string nameList(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
    return out.empty() ? "none" : out;
}

// Get.<variable> and Set.<variable>: nodes made for one variable of one circuit.
std::shared_ptr<const NodeDesc> variableNode(std::string_view type, const CircuitDocument::Variable& variable) {
    NodeDesc node;
    node.id = std::string(type);
    node.category = "Variable";
    PinDesc value;
    value.name = "value";
    value.type = variable.type;
    if (type.rfind("Set.", 0) == 0) {
        node.description = "Sets the variable " + variable.name + ".";
        node.execInputs = {"exec"};
        node.execOutputs = {"then"};
        value.required = true;
        node.inputs = {value};
        value.required = false;
        node.outputs = {value};
        node.setsVariable = true;
        node.run = [](CircuitCall& c) {
            c.out(0) = c.in(0);
            return 0;
        };
    } else {
        node.description = "The variable " + variable.name + ".";
        node.outputs = {value};
        node.run = [](CircuitCall&) { return 0; };   // its output is the variable's register
    }
    return std::make_shared<const NodeDesc>(std::move(node));
}

struct Compiler {
    Compiler(const CircuitDocument& document, CircuitKind circuitKind, std::vector<CircuitProblem>& found)
        : doc(document), kind(circuitKind), problems(found) {}

    const CircuitDocument& doc;
    CircuitKind kind;
    std::vector<CircuitProblem>& problems;
    std::shared_ptr<CircuitProgram> program = std::make_shared<CircuitProgram>();
    bool failed = false;

    // Per document node, in file order.
    struct Node {
        const CircuitDocument::Node* src = nullptr;
        std::shared_ptr<const NodeDesc> desc;
        int site = -1;   // its call site; -1 when the node couldn't be resolved
    };
    std::vector<Node> nodes;
    std::map<std::string, size_t> byName;
    // Per call site: pure call sites its inputs come from, and Int-to-Float conversions to
    // run before it.
    std::vector<std::vector<uint32_t>> pureDeps;
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> conversions;
    // (site, exec output) -> (site, exec input)
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> execLinks;
    std::vector<bool> hasIncomingExec, usedOutput;

    void error(int line, std::string message) {
        problems.push_back({line, std::move(message), false});
        failed = true;
    }
    void warning(int line, std::string message) { problems.push_back({line, std::move(message), true}); }

    std::string describe(const Node& n) const { return "node '" + n.src->name + "' (" + n.src->type + ")"; }

    uint32_t newRegister(Value value) {
        program->registers.push_back(std::move(value));
        return static_cast<uint32_t>(program->registers.size() - 1);
    }

    void variables() {
        for (const auto& variable : doc.variables) {
            Value value = defaultValue(variable.type);
            std::string why;
            if (!variable.value.empty() && !parseValue(variable.value, variable.type, value, why))
                error(variable.line, "variable '" + variable.name + "': " + why);
            program->variables.push_back({variable.name, variable.type, newRegister(std::move(value))});
        }
    }

    const CircuitDocument::Variable* findVariable(std::string_view name) const {
        for (const auto& variable : doc.variables)
            if (variable.name == name) return &variable;
        return nullptr;
    }

    void resolve() {
        for (const auto& src : doc.nodes) {
            Node node;
            node.src = &src;
            if (src.type.rfind("Set.", 0) == 0 || src.type.rfind("Get.", 0) == 0) {
                const std::string name = src.type.substr(4);
                if (const auto* variable = findVariable(name))
                    node.desc = variableNode(src.type, *variable);
                else
                    error(
                        src.line, "node '" + src.name + "': there's no variable '" + name + "' (declare it with var)");
            } else {
                node.desc = findNode(src.type);
                if (!node.desc)
                    error(
                        src.line, "node '" + src.name + "': unknown node type '" + src.type +
                                      "' (player --node-schema lists them all)");
            }
            if (node.desc && node.desc->event != CircuitEvent::None && !eventAllowed(node.desc->event, kind))
                error(
                    src.line, describe(node) + ": " + circuitEventName(node.desc->event) + " can't be used in " +
                                  (kind == CircuitKind::Game ? "the game circuit" : "an entity circuit"));
            byName[src.name] = nodes.size();
            nodes.push_back(std::move(node));
        }
    }

    // Call sites and their output and state registers.
    void sites() {
        for (Node& node : nodes) {
            if (!node.desc) continue;
            CircuitProgram::CallSite site;
            site.node = node.desc;
            site.name = node.src->name;
            site.line = node.src->line;
            const bool variable = node.desc->category == "Variable";
            for (const PinDesc& out : node.desc->outputs) {
                if (variable) {
                    site.outputs.push_back(program->findVariable(node.src->type.substr(4))->reg);
                    continue;
                }
                site.outputs.push_back(newRegister(defaultValue(out.type)));
            }
            site.state = static_cast<uint32_t>(program->registers.size());
            for (uint32_t i = 0; i < node.desc->stateSlots; ++i) newRegister(Value::integer(0));
            site.exec.assign(node.desc->execOutputs.size(), -1);
            node.site = static_cast<int>(program->sites.size());
            program->sites.push_back(std::move(site));
        }
        pureDeps.resize(program->sites.size());
        conversions.resize(program->sites.size());
        hasIncomingExec.assign(program->sites.size(), false);
        usedOutput.assign(program->sites.size(), false);
    }

    // Where a value of `from` can go into a pin of `to`: the same type, or Int into Float.
    static bool compatible(PinType from, PinType to) {
        return from == to || (from == PinType::Int && to == PinType::Float);
    }

    uint32_t convertIfNeeded(uint32_t site, uint32_t reg, PinType from, PinType to) {
        if (from == to) return reg;
        const uint32_t temp = newRegister(defaultValue(PinType::Float));
        conversions[site].push_back({reg, temp});
        return temp;
    }

    void inputs() {
        for (Node& node : nodes) {
            if (node.site < 0) continue;
            auto& site = program->sites[size_t(node.site)];
            const NodeDesc& desc = *node.desc;
            for (const auto& given : node.src->inputs) {
                if (std::none_of(
                        desc.inputs.begin(), desc.inputs.end(), [&](const PinDesc& p) { return p.name == given.pin; }))
                    error(
                        given.line,
                        describe(node) + " has no input '" + given.pin + "' (it has: " + pinList(desc.inputs) + ")");
            }
            site.given.assign(desc.inputs.size(), 0);
            for (size_t i = 0; i < desc.inputs.size(); ++i) {
                const PinDesc& pin = desc.inputs[i];
                const auto found = std::find_if(node.src->inputs.begin(), node.src->inputs.end(), [&](const auto& in) {
                    return in.pin == pin.name;
                });
                if (found == node.src->inputs.end()) {
                    if (pin.required)
                        error(node.src->line, describe(node) + ": input '" + pin.name + "' needs a value");
                    Value value = defaultValue(pin.type);
                    std::string why;
                    if (!pin.defaultValue.empty()) parseValue(pin.defaultValue, pin.type, value, why);
                    site.inputs.push_back(newRegister(std::move(value)));
                    continue;
                }
                site.given[i] = 1;
                site.inputs.push_back(input(node, i, *found));
            }
        }
    }

    // One given input: a link, a variable or a literal. Returns its register.
    uint32_t input(const Node& node, size_t index, const CircuitDocument::Input& given) {
        const PinDesc& pin = node.desc->inputs[index];
        const uint32_t self = static_cast<uint32_t>(node.site);
        const std::string where = describe(node) + ", input '" + pin.name + "'";
        std::string sourceName, outputName;
        if (!pin.constant && splitReference(given.text, sourceName, outputName) && byName.count(sourceName)) {
            const Node& source = nodes[byName[sourceName]];
            if (!source.desc) return newRegister(defaultValue(pin.type));
            const auto& outputs = source.desc->outputs;
            const auto out =
                std::find_if(outputs.begin(), outputs.end(), [&](const PinDesc& p) { return p.name == outputName; });
            if (out == outputs.end()) {
                error(
                    given.line, where + ": " + describe(source) + " has no output '" + outputName +
                                    "' (it has: " + pinList(outputs) + ")");
                return newRegister(defaultValue(pin.type));
            }
            if (!compatible(out->type, pin.type)) {
                error(
                    given.line, where + " wants " + pinTypeName(pin.type) + " but " + sourceName + "." + outputName +
                                    " is " + pinTypeName(out->type));
                return newRegister(defaultValue(pin.type));
            }
            const uint32_t sourceSite = static_cast<uint32_t>(source.site);
            usedOutput[sourceSite] = true;
            if (source.desc->pure()) pureDeps[self].push_back(sourceSite);
            const uint32_t reg = program->sites[sourceSite].outputs[size_t(out - outputs.begin())];
            return convertIfNeeded(self, reg, out->type, pin.type);
        }
        if (!pin.constant && identifier(given.text)) {
            if (const auto* variable = program->findVariable(given.text)) {
                if (!compatible(variable->type, pin.type)) {
                    error(
                        given.line, where + " wants " + pinTypeName(pin.type) + " but variable " + variable->name +
                                        " is " + pinTypeName(variable->type));
                    return newRegister(defaultValue(pin.type));
                }
                return convertIfNeeded(self, variable->reg, variable->type, pin.type);
            }
        }
        Value value;
        std::string why;
        if (!parseValue(given.text, pin.type, value, why)) {
            if (pin.constant)
                error(given.line, where + ": " + why + " (this pin takes only a literal)");
            else if (identifier(given.text) && given.text != "true" && given.text != "false" && given.text != "self")
                error(given.line, where + ": '" + given.text + "' isn't a variable, a value or node.output");
            else if (splitReference(given.text, sourceName, outputName) && !pin.constant)
                error(given.line, where + ": there's no node '" + sourceName + "'");
            else
                error(given.line, where + ": " + why);
            return newRegister(defaultValue(pin.type));
        }
        if ((node.desc->id == "Event.Message" || node.desc->id == "Circuit.Send") && pin.name == "message" &&
            value.text.find_first_not_of(" \t\r\n") == std::string::npos) {
            error(given.line, where + ": message name must not be empty");
        }
        if (!pin.options.empty() && (value.i < 0 || size_t(value.i) >= pin.options.size())) {
            error(given.line, where + ": enum value is outside its named options");
            return newRegister(defaultValue(pin.type));
        }
        return newRegister(std::move(value));
    }

    void execs() {
        for (Node& node : nodes) {
            if (node.site < 0) continue;
            const NodeDesc& desc = *node.desc;
            for (const auto& exec : node.src->execs) {
                uint32_t input = 0;
                if (desc.execInputs.empty()) {
                    error(
                        exec.line, describe(node) + " has no exec input: " +
                                       (desc.event != CircuitEvent::None ? "events start wires" : "it's a pure node") +
                                       " and runs whenever its outputs are used");
                    continue;
                }
                if (!exec.pin.empty()) {
                    const auto found = std::find(desc.execInputs.begin(), desc.execInputs.end(), exec.pin);
                    if (found == desc.execInputs.end()) {
                        error(
                            exec.line, describe(node) + " has no exec input '" + exec.pin +
                                           "' (it has: " + nameList(desc.execInputs) + ")");
                        continue;
                    }
                    input = static_cast<uint32_t>(found - desc.execInputs.begin());
                }
                const auto sourceAt = byName.find(exec.node);
                if (sourceAt == byName.end()) {
                    error(exec.line, describe(node) + ": exec from unknown node '" + exec.node + "'");
                    continue;
                }
                const Node& source = nodes[sourceAt->second];
                if (!source.desc) continue;
                const auto& outs = source.desc->execOutputs;
                const auto out = std::find(outs.begin(), outs.end(), exec.output);
                if (out == outs.end()) {
                    error(
                        exec.line, describe(node) + ": " + describe(source) + " has no exec output '" + exec.output +
                                       "' (it has: " + nameList(outs) + ")");
                    continue;
                }
                const std::pair<uint32_t, uint32_t> from{uint32_t(source.site), uint32_t(out - outs.begin())};
                const std::pair<uint32_t, uint32_t> to{uint32_t(node.site), input};
                if (const auto taken = execLinks.find(from); taken != execLinks.end()) {
                    error(
                        exec.line, "exec output " + exec.node + "." + exec.output + " already leads to node '" +
                                       program->sites[taken->second.first].name +
                                       "'; one output leads to one node (use Flow.Sequence to do several things)");
                    continue;
                }
                execLinks[from] = to;
                hasIncomingExec[size_t(node.site)] = true;
            }
        }
    }

    // Data loops among pure nodes, which could never be worked out.
    void loops() {
        std::vector<uint8_t> state(program->sites.size(), 0);   // 0 new, 1 visiting, 2 done
        std::vector<uint32_t> path;
        std::set<std::string> reported;
        std::function<void(uint32_t)> visit = [&](uint32_t site) {
            state[site] = 1;
            path.push_back(site);
            for (uint32_t dep : pureDeps[site]) {
                if (state[dep] == 1) {
                    std::string loop;
                    const auto start = std::find(path.begin(), path.end(), dep);
                    for (auto it = start; it != path.end(); ++it) loop += program->sites[*it].name + " -> ";
                    loop += program->sites[dep].name;
                    if (reported.insert(program->sites[dep].name).second)
                        error(program->sites[dep].line, "data loop: " + loop + "; a value can't depend on itself");
                } else if (state[dep] == 0)
                    visit(dep);
            }
            path.pop_back();
            state[site] = 2;
        };
        for (uint32_t site = 0; site < program->sites.size(); ++site)
            if (state[site] == 0) visit(site);
    }

    void unreachable() {
        for (const Node& node : nodes) {
            if (node.site < 0) continue;
            const NodeDesc& desc = *node.desc;
            if (desc.event == CircuitEvent::None && !desc.pure() && !hasIncomingExec[size_t(node.site)])
                warning(node.src->line, describe(node) + " never runs: no exec wire leads to it");
            if (desc.pure() && !usedOutput[size_t(node.site)] && !desc.outputs.empty())
                warning(node.src->line, describe(node) + " isn't used: nothing reads its outputs");
        }
    }

    // Emits the pure nodes site needs, dependencies first, each once per block.
    void evals(uint32_t site, std::set<uint32_t>& done) {
        for (uint32_t dep : pureDeps[site]) {
            if (done.count(dep)) continue;
            done.insert(dep);
            evals(dep, done);
            for (const auto& [from, to] : conversions[dep])
                program->code.push_back({CircuitProgram::Op::IntToFloat, from, to});
            program->code.push_back({CircuitProgram::Op::Eval, dep, 0});
        }
    }

    void emit() {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> blocks;   // (site, exec input) -> pc
        std::set<std::pair<uint32_t, uint32_t>> targets;
        for (const auto& [from, to] : execLinks) targets.insert(to);
        for (const auto& target : targets) {
            blocks[target] = static_cast<uint32_t>(program->code.size());
            std::set<uint32_t> done;
            evals(target.first, done);
            for (const auto& [from, to] : conversions[target.first])
                program->code.push_back({CircuitProgram::Op::IntToFloat, from, to});
            program->code.push_back({CircuitProgram::Op::Fire, target.first, target.second});
        }
        for (const auto& [from, to] : execLinks) program->sites[from.first].exec[from.second] = int32_t(blocks[to]);
        for (uint32_t i = 0; i < program->sites.size(); ++i) {
            const auto& site = program->sites[i];
            const NodeDesc& desc = *site.node;
            if (desc.event != CircuitEvent::None) {
                CircuitProgram::EventEntry entry;
                entry.event = desc.event;
                entry.site = i;
                if (!site.inputs.empty()) entry.param = program->registers[site.inputs[0]].text;
                program->events.push_back(std::move(entry));
            }
            if (desc.component) {
                if (desc.readsComponent) program->reads.push_back(desc.component);
                if (desc.writesComponent) program->writes.push_back(desc.component);
            }
        }
        for (auto* list : {&program->reads, &program->writes}) {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end()), list->end());
        }
    }

    std::shared_ptr<const CircuitProgram> run(std::string path) {
        program->path = std::move(path);
        program->kind = kind;
        program->nodeVersion = nodeRegistryVersion();
        variables();
        resolve();
        sites();
        inputs();
        execs();
        loops();
        if (failed) return nullptr;
        unreachable();
        emit();
        return program;
    }
};

}   // namespace

std::shared_ptr<const CircuitProgram> compileCircuit(
    const CircuitDocument& document, std::string path, CircuitKind kind, std::vector<CircuitProblem>& problems) {
    Compiler compiler(document, kind, problems);
    auto program = compiler.run(std::move(path));
    std::stable_sort(problems.begin(), problems.end(), [](const CircuitProblem& a, const CircuitProblem& b) {
        return a.line < b.line;
    });
    return program;
}

std::string dumpCircuit(const CircuitProgram& program) {
    std::ostringstream out;
    out << "circuit " << program.path << " (" << (program.kind == CircuitKind::Game ? "game" : "entity") << ")\n";
    out << program.registers.size() << " registers, " << program.sites.size() << " nodes, " << program.code.size()
        << " instructions\n";
    for (const auto& variable : program.variables)
        out << "var r" << variable.reg << " " << variable.name << " " << pinTypeName(variable.type) << " = "
            << formatValue(program.registers[variable.reg]) << "\n";
    auto names = [&](const std::vector<uint32_t>& ids) {
        std::string list;
        for (uint32_t id : ids)
            for (const ComponentInfo& info : registeredComponents())
                if (info.id == id) list += (list.empty() ? "" : ", ") + info.name;
        return list.empty() ? std::string("nothing") : list;
    };
    out << "reads " << names(program.reads) << "; writes " << names(program.writes) << "\n";
    auto target = [](int32_t pc) { return pc < 0 ? std::string("end") : std::to_string(pc); };
    for (const auto& event : program.events) {
        const auto& site = program.sites[event.site];
        out << "on " << circuitEventName(event.event);
        if (!event.param.empty()) out << " \"" << event.param << "\"";
        out << " (" << site.name << ") -> " << target(site.exec.empty() ? -1 : site.exec[0]) << "\n";
    }
    auto regs = [](const std::vector<uint32_t>& list) {
        std::string text;
        for (uint32_t r : list) text += " r" + std::to_string(r);
        return text;
    };
    for (size_t pc = 0; pc < program.code.size(); ++pc) {
        const auto& in = program.code[pc];
        char number[32];
        std::snprintf(number, sizeof(number), "%04zu  ", pc);
        out << number;
        if (in.op == CircuitProgram::Op::IntToFloat) {
            out << "float  r" << in.b << " = r" << in.a << "\n";
            continue;
        }
        const auto& site = program.sites[in.a];
        out << (in.op == CircuitProgram::Op::Eval ? "eval   " : "fire   ") << site.name << " (" << site.node->id << ")";
        if (in.op == CircuitProgram::Op::Fire && site.node->execInputs.size() > 1)
            out << " via " << site.node->execInputs[in.b];
        out << " in" << regs(site.inputs) << " out" << regs(site.outputs);
        if (in.op == CircuitProgram::Op::Fire) {
            out << " ->";
            for (size_t k = 0; k < site.exec.size(); ++k)
                out << " " << site.node->execOutputs[k] << ":" << target(site.exec[k]);
        }
        out << "\n";
    }
    return out.str();
}

// ---- CircuitLibrary -----------------------------------------------------------------------

namespace {
std::string entryKey(std::string_view path, CircuitKind kind) {
    return std::string(kind == CircuitKind::Game ? "game:" : "entity:") + std::string(path);
}
}   // namespace

std::filesystem::path CircuitLibrary::file(std::string_view path) const {
    return (prefabs().contentFolder() / std::filesystem::path(std::string(path))).lexically_normal();
}

CircuitLibrary::Entry CircuitLibrary::compile(const std::string& path, CircuitKind kind) const {
    Entry entry;
    entry.nodeVersion = nodeRegistryVersion();
    const std::filesystem::path source = file(path);
    std::error_code ec;
    entry.exists = std::filesystem::is_regular_file(source, ec);
    if (!entry.exists) {
        entry.problems.push_back(path + ": no such circuit in Content");
        return entry;
    }
    entry.modified = std::filesystem::last_write_time(source, ec);
    std::ifstream in(source, std::ios::binary);
    CircuitDocument document;
    std::string error;
    if (!in || !parseCircuit(in, path, document, error)) {
        entry.problems.push_back(error.empty() ? path + ": can't read it" : error);
        return entry;
    }
    std::vector<CircuitProblem> problems;
    entry.program = compileCircuit(document, path, kind, problems);
    for (const auto& problem : problems)
        entry.problems.push_back(
            path + ":" + std::to_string(problem.line) + ": " + (problem.warning ? "warning: " : "") + problem.message);
    return entry;
}

std::shared_ptr<const CircuitProgram>
CircuitLibrary::load(std::string_view path, CircuitKind kind, std::vector<std::string>* problems) {
    std::lock_guard lock(m_mutex);
    const std::string key = entryKey(path, kind);
    auto found = m_entries.find(key);
    if (found == m_entries.end() || found->second.nodeVersion != nodeRegistryVersion()) {
        Entry entry = compile(std::string(path), kind);
        if (found != m_entries.end()) ++m_generation;
        found = m_entries.insert_or_assign(key, std::move(entry)).first;
    }
    if (problems) *problems = found->second.problems;
    return found->second.program;
}

std::vector<std::string> CircuitLibrary::poll(double frameDt, double interval) {
    std::vector<std::string> reloaded;
    std::lock_guard lock(m_mutex);
    m_pollTimer += frameDt;
    if (m_pollTimer < interval) return reloaded;
    m_pollTimer = 0.0;
    const uint64_t version = nodeRegistryVersion();
    for (auto& [key, entry] : m_entries) {
        const std::string path = key.substr(key.find(':') + 1);
        const CircuitKind kind = key.rfind("game:", 0) == 0 ? CircuitKind::Game : CircuitKind::Entity;
        std::error_code ec;
        const std::filesystem::path source = file(path);
        const bool exists = std::filesystem::is_regular_file(source, ec);
        const auto modified = exists ? std::filesystem::last_write_time(source, ec) : std::filesystem::file_time_type{};
        if (entry.nodeVersion == version && exists == entry.exists && (!exists || modified == entry.modified)) continue;
        entry = compile(path, kind);
        reloaded.push_back(path);
        for (const std::string& problem : entry.problems) SW_LOG(LogCircuits, Warning, "{}", problem);
    }
    if (!reloaded.empty()) ++m_generation;
    return reloaded;
}

uint64_t CircuitLibrary::generation() const {
    std::lock_guard lock(m_mutex);
    return m_generation;
}

void CircuitLibrary::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
    ++m_generation;
}

// Never destroyed, like the node registry: compiled circuits can hold nodes from game
// modules that are unloaded before exit.
CircuitLibrary& circuitLibrary() {
    static CircuitLibrary* library = new CircuitLibrary();
    return *library;
}

}   // namespace shockwave
