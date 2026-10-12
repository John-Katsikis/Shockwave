// Running circuits during Play: instances, events and the bytecode VM.
#include <Circuits/CircuitWorld.h>

#include <Engine/Components.h>
#include <GameUI/UIWorld.h>
#include <Input/Input.h>
#include <Physics/Physics.h>

#include <algorithm>
#include <exception>

namespace shockwave {

namespace {
// How deep Sequence and ForLoop may nest inside one another in one event.
constexpr size_t kMaxResumeDepth = 256;
}   // namespace

CircuitWorld::CircuitWorld(World& world, CircuitHost host, std::string gameCircuit)
    : m_world(world), m_host(host), m_gamePath(std::move(gameCircuit)) {}

CircuitWorld::~CircuitWorld() { *m_alive = false; }

void CircuitWorld::report(const std::string& problem) {
    if (!m_reported.insert(problem).second) return;
    m_diagnostics.push_back(problem);
    SW_LOG(LogCircuits, Warning, "{}", problem);
}

std::string CircuitWorld::entityName(Entity e) const {
    const Name* name = m_world.get<Name>(e);
    return name && !name->value.empty() ? name->value : "#" + std::to_string(e.index);
}

CircuitWorld::Instance* CircuitWorld::instanceOf(Entity e) {
    const auto found = m_instances.find(key(e));
    return found == m_instances.end() ? nullptr : &found->second;
}

void CircuitWorld::start(Instance& instance, std::shared_ptr<const CircuitProgram> program) {
    instance.program = std::move(program);
    instance.regs = instance.program->registers;
    instance.dirty = false;
}

// A circuit compiled again: start from its new registers, keeping variables whose name and
// type are unchanged.
void CircuitWorld::migrate(Instance& instance, std::shared_ptr<const CircuitProgram> program) {
    const auto old = instance.program;
    const std::vector<Value> oldRegs = std::move(instance.regs);
    instance.program = std::move(program);
    instance.regs = instance.program->registers;
    if (!old) return;
    for (const auto& variable : instance.program->variables) {
        const auto* before = old->findVariable(variable.name);
        if (before && before->type == variable.type && before->reg < oldRegs.size())
            instance.regs[variable.reg] = oldRegs[before->reg];
    }
}

void CircuitWorld::applyVars(Instance& instance, Entity entity) {
    const CircuitVars* vars = m_world.get<CircuitVars>(entity);
    if (!vars || !instance.program) return;
    for (const auto& [name, text] : parseCircuitVars(vars->values)) {
        const auto* variable = instance.program->findVariable(name);
        const std::string where = instance.path + ": entity '" + entityName(entity) + "': CircuitVars " + name;
        if (!variable) {
            report(where + ": the circuit has no such variable");
            continue;
        }
        Value value;
        std::string why;
        if (parseValue(text, variable->type, value, why))
            instance.regs[variable->reg] = std::move(value);
        else
            report(where + ": " + why);
    }
}

// Writes the variables that differ from the circuit's defaults into CircuitVars.
void CircuitWorld::storeVars(Instance& instance, Entity entity) {
    if (!instance.dirty || !instance.program || !m_world.alive(entity)) return;
    instance.dirty = false;
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& variable : instance.program->variables) {
        const std::string now = formatValue(instance.regs[variable.reg]);
        if (now != formatValue(instance.program->registers[variable.reg])) values.emplace_back(variable.name, now);
    }
    const std::string text = formatCircuitVars(values);
    if (CircuitVars* vars = m_world.get<CircuitVars>(entity))
        vars->values = text;
    else if (!text.empty())
        m_world.add<CircuitVars>(entity, {text});
}

void CircuitWorld::storeAll() {
    for (const Entity e : m_order)
        if (Instance* instance = instanceOf(e)) storeVars(*instance, e);
}

void CircuitWorld::refresh() {
    CircuitLibrary& library = circuitLibrary();
    const uint64_t generation = library.generation();
    if (!m_gamePath.empty() && (!m_game.program || m_game.generation != generation)) {
        std::vector<std::string> problems;
        auto program = library.load(m_gamePath, CircuitKind::Game, &problems);
        for (const std::string& problem : problems) report(problem);
        if (program && program != m_game.program) {
            if (m_game.program)
                migrate(m_game, std::move(program));
            else {
                m_game.path = m_gamePath;
                start(m_game, std::move(program));
            }
        }
        m_game.generation = generation;
    }

    std::vector<std::pair<Entity, std::string>> found;
    m_world.each<const Circuit>([&](Entity e, const Circuit& circuit) {
        if (circuit.enabled && !circuit.circuit.empty()) found.emplace_back(e, circuit.circuit);
    });
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first.index < b.first.index; });
    std::unordered_map<uint64_t, Instance> next;
    m_order.clear();
    for (auto& [entity, path] : found) {
        const uint64_t k = key(entity);
        auto existing = m_instances.find(k);
        if (existing != m_instances.end() && existing->second.path == path) {
            Instance& instance = existing->second;
            if (instance.generation != generation) {
                auto program = library.load(path, CircuitKind::Entity);
                if (program && program != instance.program) migrate(instance, std::move(program));
                instance.generation = generation;
            }
            next.emplace(k, std::move(instance));
            m_order.push_back(entity);
            continue;
        }
        // New, or its Circuit names another file now: start it from scratch.
        std::vector<std::string> problems;
        auto program = library.load(path, CircuitKind::Entity, &problems);
        for (const std::string& problem : problems) report(problem + " (entity '" + entityName(entity) + "')");
        if (!program) continue;
        Instance instance;
        instance.path = path;
        instance.generation = generation;
        start(instance, std::move(program));
        applyVars(instance, entity);
        next.emplace(k, std::move(instance));
        m_order.push_back(entity);
    }
    m_instances = std::move(next);   // entities that lost their Circuit, or were destroyed, drop out
}

void CircuitWorld::beginNew() {
    if (!m_playing) return;
    if (m_game.program && !m_game.begun) {
        m_game.begun = true;
        // UI commands reach the game circuit through UIWorld handlers, one per command.
        if (m_host.ui)
            for (const auto& entry : m_game.program->events) {
                if (entry.event != CircuitEvent::UICommand || !m_uiCommands.insert(entry.param).second) continue;
                m_host.ui->on(entry.param, [this, alive = m_alive, command = entry.param](const UIEvent& e) {
                    if (!*alive || !m_game.program) return;
                    dispatch(
                        m_game, kNullEntity, CircuitEvent::UICommand, command,
                        [&](const CircuitProgram::CallSite& site, std::vector<Value>& regs) {
                            if (site.outputs.size() < 3) return;
                            regs[site.outputs[0]] = Value::string(std::string(e.value));
                            regs[site.outputs[1]] = Value::string(std::string(e.element));
                            regs[site.outputs[2]] = Value::string(std::string(e.document));
                        });
                });
            }
        dispatch(m_game, kNullEntity, CircuitEvent::BeginPlay);
    }
    const std::vector<Entity> order = m_order;   // BeginPlay may spawn entities
    for (const Entity e : order) {
        Instance* instance = instanceOf(e);
        if (!instance || instance->begun) continue;
        instance->begun = true;
        dispatch(*instance, e, CircuitEvent::BeginPlay);
        if ((instance = instanceOf(e))) storeVars(*instance, e);
    }
}

void CircuitWorld::dispatch(
    Instance& instance, Entity self, CircuitEvent event, std::string_view param,
    const std::function<void(const CircuitProgram::CallSite&, std::vector<Value>&)>& data) {
    if (m_debugPaused) return;
    const auto program = instance.program;   // kept alive while it runs
    if (!program) return;
    for (const auto& entry : program->events) {
        if (entry.event != event || (!param.empty() && entry.param != param)) continue;
        const auto& site = program->sites[entry.site];
        if (data) data(site, instance.regs);
        if (site.exec.empty() || site.exec[0] < 0) continue;
        run(instance, self, static_cast<uint32_t>(site.exec[0]));
        if (m_debugPaused) return;
    }
}

void CircuitWorld::dispatchAll(
    CircuitEvent event, std::string_view param,
    const std::function<void(const CircuitProgram::CallSite&, std::vector<Value>&)>& data) {
    if (m_game.program) dispatch(m_game, kNullEntity, event, param, data);
    const std::vector<Entity> order = m_order;
    for (const Entity e : order) {
        if (!m_world.alive(e)) continue;
        Instance* instance = instanceOf(e);
        if (!instance || !instance->begun) continue;
        dispatch(*instance, e, event, param, data);
        if ((instance = instanceOf(e))) storeVars(*instance, e);
    }
}

void CircuitWorld::run(Instance& instance, Entity self, uint32_t entry) {
    const auto program = instance.program;
    const std::vector<CircuitProgram::Instruction>& code = program->code;
    if (!instance.suspended) {
        instance.pc = entry;
        instance.resumeStack.clear();
        instance.resumed = false;
        instance.budget = kInstructionBudget;
    }
    instance.suspended = false;
    auto& resumeStack = instance.resumeStack;
    auto& pc = instance.pc;
    auto& resumed = instance.resumed;
    auto& budget = instance.budget;
    const CircuitProgram::CallSite* current = nullptr;
    try {
        while (true) {
            if (pc < 0) {   // this wire ended: back to the Sequence or ForLoop that started it, if any
                if (resumeStack.empty()) return;
                pc = resumeStack.back();
                resumeStack.pop_back();
                resumed = true;
            }
            if (static_cast<size_t>(pc) >= code.size()) return;
            const auto& pending = code[size_t(pc)];
            if (pending.op != CircuitProgram::Op::IntToFloat) {
                const auto& site = program->sites[pending.a];
                if (!m_skipBreakpoint &&
                    (m_singleStep || (!m_breakpoints.empty() && hasBreakpoint(program->path, site.name)))) {
                    m_location = {self, program->path, site.name, site.line, {}, {}};
                    for (size_t i = 0; i < site.inputs.size(); ++i)
                        m_location.inputs.emplace_back(
                            site.node->inputs[i].name, formatValue(instance.regs[site.inputs[i]]));
                    instance.suspended = true;
                    m_debugPaused = true;
                    m_singleStep = false;
                    return;
                }
                m_skipBreakpoint = false;
            }
            if (--budget == 0) {
                report(
                    program->path + ":" + std::to_string(current ? current->line : 0) + ": node '" +
                    (current ? current->name : std::string("?")) + "' ran over " + std::to_string(kInstructionBudget) +
                    " steps in one event and was stopped (an endless loop?)");
                return;
            }
            const auto& instruction = code[size_t(pc)];
            switch (instruction.op) {
            case CircuitProgram::Op::IntToFloat:
                instance.regs[instruction.b] = Value::number(float(instance.regs[instruction.a].i));
                ++pc;
                break;
            case CircuitProgram::Op::Eval:
            case CircuitProgram::Op::Fire: {
                const auto& site = program->sites[instruction.a];
                current = &site;
                const bool fire = instruction.op == CircuitProgram::Op::Fire;
                CircuitCall call{
                    .world = m_world,
                    .self = self,
                    .host = m_host,
                    .node = *site.node,
                    .circuit = program->path,
                    .execInput = fire ? int(instruction.b) : 0,
                    .resumed = fire && resumed,
                    .resume = false,
                    .regs = std::span<Value>(instance.regs),
                    .inputs = site.inputs.data(),
                    .outputs = site.outputs.data(),
                    .state = site.state,
                    .site = &site,
                };
                resumed = false;
                DebugEntry trace;
                if (m_tracing) {
                    trace = {self, program->path, site.name, site.line, {}, {}};
                    for (size_t i = 0; i < site.inputs.size(); ++i)
                        trace.inputs.emplace_back(
                            site.node->inputs[i].name, formatValue(instance.regs[site.inputs[i]]));
                }
                const int output = site.node->run ? site.node->run(call) : 0;
                if (m_tracing) {
                    for (size_t i = 0; i < site.outputs.size(); ++i)
                        trace.outputs.emplace_back(
                            site.node->outputs[i].name, formatValue(instance.regs[site.outputs[i]]));
                    if (m_trace.size() == 256) m_trace.pop_front();
                    m_trace.push_back(std::move(trace));
                }
                if (!fire) {
                    ++pc;
                    break;
                }
                if (site.node->setsVariable) instance.dirty = true;
                if (call.resume) {
                    if (resumeStack.size() >= kMaxResumeDepth) {
                        report(
                            program->path + ":" + std::to_string(site.line) + ": node '" + site.name +
                            "': loops and sequences nest too deep; stopped");
                        return;
                    }
                    resumeStack.push_back(static_cast<uint32_t>(pc));
                }
                pc = output >= 0 && size_t(output) < site.exec.size() ? site.exec[size_t(output)] : -1;
                break;
            }
            }
        }
    } catch (const std::exception& e) {
        report(
            program->path + ":" + std::to_string(current ? current->line : 0) + ": node '" +
            (current ? current->name : std::string("?")) + "' failed: " + e.what());
    }
}

void CircuitWorld::setBreakpoint(std::string circuit, std::string node, bool enabled) {
    const auto key = std::make_pair(std::move(circuit), std::move(node));
    if (enabled)
        m_breakpoints.insert(key);
    else
        m_breakpoints.erase(key);
}

bool CircuitWorld::hasBreakpoint(std::string_view circuit, std::string_view node) const {
    return m_breakpoints.contains({std::string(circuit), std::string(node)});
}

void CircuitWorld::debugContinue(bool singleStep) {
    if (!m_debugPaused) return;
    Instance* instance = m_location.entity.valid() ? instanceOf(m_location.entity) : &m_game;
    m_debugPaused = false;
    m_singleStep = singleStep;
    m_skipBreakpoint = true;
    if (!instance || !instance->suspended || (m_location.entity.valid() && !m_world.alive(m_location.entity))) {
        m_singleStep = m_skipBreakpoint = false;
        return;
    }
    run(*instance, m_location.entity, uint32_t(instance->pc));
    m_singleStep = m_skipBreakpoint = false;
    if (m_location.entity.valid()) storeVars(*instance, m_location.entity);
}

// ---- Events -------------------------------------------------------------------------------

void CircuitWorld::beginPlay() {
    m_playing = true;
    circuitLibrary().poll(0.0, 0.0);   // circuits saved since the last Play
    refresh();
    beginNew();
}

void CircuitWorld::endPlay() {
    if (!m_playing) return;
    // Stop cancels a suspended wire, but still delivers EndPlay without stopping on a breakpoint.
    m_debugPaused = m_singleStep = m_skipBreakpoint = false;
    m_breakpoints.clear();
    m_game.suspended = false;
    for (auto& [key, instance] : m_instances) instance.suspended = false;
    refresh();
    dispatchAll(CircuitEvent::EndPlay);
    m_playing = false;
}

void CircuitWorld::fixedTick(double dt) {
    if (!m_playing || m_debugPaused) return;
    refresh();
    beginNew();
    const float seconds = static_cast<float>(dt);
    dispatchAll(CircuitEvent::FixedTick, {}, [seconds](const CircuitProgram::CallSite& site, std::vector<Value>& regs) {
        if (!site.outputs.empty()) regs[site.outputs[0]] = Value::number(seconds);
    });
    if (!m_host.physics) return;
    // Sensors touched in the step that just ran: both entities hear about the other.
    const std::vector<ContactEvent> contacts = m_host.physics->contacts();
    for (const ContactEvent& contact : contacts) {
        if (!contact.sensor || contact.type == ContactEvent::Type::Stay) continue;
        const CircuitEvent event =
            contact.type == ContactEvent::Type::Begin ? CircuitEvent::TriggerEnter : CircuitEvent::TriggerExit;
        for (int side = 0; side < 2; ++side) {
            const Entity self = side == 0 ? contact.a : contact.b;
            const Entity other = side == 0 ? contact.b : contact.a;
            Instance* instance = instanceOf(self);
            if (!instance || !instance->begun || !instance->program || !instance->program->handles(event)) continue;
            dispatch(
                *instance, self, event, {}, [other](const CircuitProgram::CallSite& site, std::vector<Value>& regs) {
                    if (!site.outputs.empty()) regs[site.outputs[0]] = Value::entityValue(other);
                });
            if ((instance = instanceOf(self))) storeVars(*instance, self);
        }
    }
}

void CircuitWorld::frame(double dt) {
    if (!m_playing || m_debugPaused) return;
    circuitLibrary().poll(dt);
    refresh();
    beginNew();
    if (m_host.input) {
        // Actions any circuit listens for, each checked once.
        std::set<std::string> pressed, released;
        auto collect = [&](const Instance& instance) {
            if (!instance.program) return;
            for (const auto& entry : instance.program->events) {
                if (entry.event == CircuitEvent::ActionPressed) pressed.insert(entry.param);
                if (entry.event == CircuitEvent::ActionReleased) released.insert(entry.param);
            }
        };
        collect(m_game);
        for (const auto& [k, instance] : m_instances) collect(instance);
        for (const std::string& action : pressed)
            if (m_host.input->wasActionPressed(action)) dispatchAll(CircuitEvent::ActionPressed, action);
        for (const std::string& action : released)
            if (m_host.input->wasActionReleased(action)) dispatchAll(CircuitEvent::ActionReleased, action);
    }
    const float seconds = static_cast<float>(dt);
    dispatchAll(CircuitEvent::Frame, {}, [seconds](const CircuitProgram::CallSite& site, std::vector<Value>& regs) {
        if (!site.outputs.empty()) regs[site.outputs[0]] = Value::number(seconds);
    });
}

void CircuitWorld::beforeSave(const std::function<void(const std::string& key, const std::string& value)>& set) {
    if (m_game.program) {
        dispatch(m_game, kNullEntity, CircuitEvent::BeforeSave);
        for (const auto& variable : m_game.program->variables)
            set("circuit." + variable.name, formatValue(m_game.regs[variable.reg]));
    }
    storeAll();
}

void CircuitWorld::afterLoad(const std::function<bool(const std::string& key, std::string& value)>& get) {
    // The world was replaced: entity circuits start over from their entities' CircuitVars,
    // without running BeginPlay again.
    for (const Entity e : m_order) {
        Instance* instance = instanceOf(e);
        if (!instance || !instance->program || !m_world.alive(e)) continue;
        start(*instance, instance->program);
        applyVars(*instance, e);
    }
    refresh();
    if (!m_game.program) return;
    for (const auto& variable : m_game.program->variables) {
        std::string text;
        if (!get("circuit." + variable.name, text)) continue;
        Value value;
        std::string why;
        if (parseValue(text, variable.type, value, why))
            m_game.regs[variable.reg] = std::move(value);
        else
            report(m_game.path + ": saved value of " + variable.name + ": " + why);
    }
    dispatch(m_game, kNullEntity, CircuitEvent::AfterLoad);
}

const Value* CircuitWorld::variable(Entity entity, std::string_view name) const {
    const Instance* instance = nullptr;
    if (!entity.valid()) {
        instance = m_game.program ? &m_game : nullptr;
    } else if (const auto found = m_instances.find(key(entity)); found != m_instances.end()) {
        instance = &found->second;
    }
    if (!instance || !instance->program) return nullptr;
    const auto* v = instance->program->findVariable(name);
    return v ? &instance->regs[v->reg] : nullptr;
}

}   // namespace shockwave
