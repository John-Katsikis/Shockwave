// The node registry: built-in nodes, nodes made from component reflection, and SW_NODEs.
#include <Circuits/CircuitNodes.h>

#include <Circuits/CircuitProgram.h>

#include <Assets/Assets.h>
#include <Audio/Audio.h>
#include <Engine/EntityRef.h>
#include <Engine/Hierarchy.h>
#include <Engine/Prefab.h>
#include <GameUI/UIWorld.h>
#include <Input/Input.h>
#include <Physics/Physics.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <ostream>
#include <random>

namespace shockwave {

// ---- CircuitCall -----------------------------------------------------------------------------

Entity CircuitCall::entity(size_t pin) const {
    const Value& v = in(pin);
    if (world.alive(v.entity)) return v.entity;
    if (v.text == "self") return self;
    if (!v.text.empty()) {
        const EntityRef found = findEntity(world, v.text);
        return found ? found.entity() : kNullEntity;
    }
    return kNullEntity;
}

const char* circuitEventName(CircuitEvent event) {
    switch (event) {
    case CircuitEvent::None: return "None";
    case CircuitEvent::BeginPlay: return "BeginPlay";
    case CircuitEvent::EndPlay: return "EndPlay";
    case CircuitEvent::FixedTick: return "FixedTick";
    case CircuitEvent::Frame: return "Frame";
    case CircuitEvent::ActionPressed: return "ActionPressed";
    case CircuitEvent::ActionReleased: return "ActionReleased";
    case CircuitEvent::TriggerEnter: return "TriggerEnter";
    case CircuitEvent::TriggerExit: return "TriggerExit";
    case CircuitEvent::BeforeSave: return "BeforeSave";
    case CircuitEvent::AfterLoad: return "AfterLoad";
    case CircuitEvent::UICommand: return "UICommand";
    }
    return "?";
}

bool eventAllowed(CircuitEvent event, CircuitKind kind) {
    switch (event) {
    case CircuitEvent::TriggerEnter:
    case CircuitEvent::TriggerExit: return kind == CircuitKind::Entity;
    case CircuitEvent::BeforeSave:
    case CircuitEvent::AfterLoad:
    case CircuitEvent::UICommand: return kind == CircuitKind::Game;
    case CircuitEvent::None: return false;
    default: return true;
    }
}

namespace {

// ---- Building nodes ----------------------------------------------------------------------------

PinDesc pin(std::string name, PinType type, std::string defaultValue = {}, bool required = false) {
    PinDesc p;
    p.name = std::move(name);
    p.type = type;
    p.defaultValue = std::move(defaultValue);
    p.required = required;
    return p;
}
PinDesc required(std::string name, PinType type) { return pin(std::move(name), type, {}, true); }
PinDesc self(std::string name = "target") { return pin(std::move(name), PinType::Entity, "self"); }

NodeDesc
pure(std::string id, std::string description, std::vector<PinDesc> inputs, std::vector<PinDesc> outputs, NodeFn run) {
    NodeDesc node;
    node.category = id.substr(0, id.find('.'));
    node.id = std::move(id);
    node.description = std::move(description);
    node.inputs = std::move(inputs);
    node.outputs = std::move(outputs);
    node.run = std::move(run);
    return node;
}

NodeDesc action(
    std::string id, std::string description, std::vector<PinDesc> inputs, std::vector<PinDesc> outputs, NodeFn run,
    std::vector<std::string> execOutputs = {"then"}, std::vector<std::string> execInputs = {"exec"}) {
    NodeDesc node = pure(std::move(id), std::move(description), std::move(inputs), std::move(outputs), std::move(run));
    node.execInputs = std::move(execInputs);
    node.execOutputs = std::move(execOutputs);
    return node;
}

NodeDesc event(
    std::string id, CircuitEvent kind, std::string description, std::vector<PinDesc> outputs,
    std::vector<PinDesc> constants = {}) {
    for (PinDesc& p : constants) p.constant = p.required = true;
    NodeDesc node =
        pure(std::move(id), std::move(description), std::move(constants), std::move(outputs), [](CircuitCall&) {
            return 0;
        });
    node.event = kind;
    node.execOutputs = {"then"};
    return node;
}

// A pure node of one Float output from Float inputs.
NodeDesc
floatMath(std::string id, std::string description, std::vector<PinDesc> inputs, float (*f)(const CircuitCall&)) {
    return pure(
        std::move(id), std::move(description), std::move(inputs), {pin("result", PinType::Float)}, [f](CircuitCall& c) {
            c.out(0) = Value::number(f(c));
            return 0;
        });
}

std::mt19937& randomEngine() {
    thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

void addBuiltins(std::map<std::string, std::shared_ptr<const NodeDesc>>& nodes) {
    std::vector<NodeDesc> list;
    const PinType F = PinType::Float, I = PinType::Int, B = PinType::Bool, V = PinType::Vec3, S = PinType::String,
                  E = PinType::Entity, R = PinType::Rotation;

    // ---- Events
    list.push_back(
        event("Event.BeginPlay", CircuitEvent::BeginPlay, "Play starts, or the entity appears later on.", {}));
    list.push_back(event("Event.EndPlay", CircuitEvent::EndPlay, "Play stops.", {}));
    list.push_back(event(
        "Event.FixedTick", CircuitEvent::FixedTick, "Every simulation tick, after the GameMode's fixedUpdate.",
        {pin("dt", F)}));
    list.push_back(
        event("Event.Frame", CircuitEvent::Frame, "Every rendered frame, after the GameMode's frame.", {pin("dt", F)}));
    list.push_back(event(
        "Event.ActionPressed", CircuitEvent::ActionPressed,
        "An input action was pressed this frame (Config/Input.swinput).", {}, {pin("action", S)}));
    list.push_back(event(
        "Event.ActionReleased", CircuitEvent::ActionReleased, "An input action was released this frame.", {},
        {pin("action", S)}));
    list.push_back(event(
        "Event.TriggerEnter", CircuitEvent::TriggerEnter,
        "Entity circuits: something started touching this entity's sensor collider.", {pin("other", E)}));
    list.push_back(event(
        "Event.TriggerExit", CircuitEvent::TriggerExit,
        "Entity circuits: something stopped touching this entity's sensor.", {pin("other", E)}));
    list.push_back(event(
        "Event.BeforeSave", CircuitEvent::BeforeSave,
        "The game circuit: a save game is about to be written. Its variables are saved right after.", {}));
    list.push_back(event(
        "Event.AfterLoad", CircuitEvent::AfterLoad,
        "The game circuit: a save game was loaded and its variables restored.", {}));
    list.push_back(event(
        "Event.UICommand", CircuitEvent::UICommand, "The game circuit: a UI element's onclick=\"command\" ran.",
        {pin("value", S), pin("element", S), pin("document", S)}, {pin("command", S)}));

    // ---- Flow
    list.push_back(action(
        "Flow.Branch", "Goes one way or the other.", {pin("condition", B)}, {},
        [](CircuitCall& c) { return c.boolean(0) ? 0 : 1; }, {"true", "false"}));
    {
        NodeDesc node = action(
            "Flow.Sequence", "Runs then0, then then1, then2 and then3, each to its end.", {}, {},
            [](CircuitCall& c) {
                const int next = c.resumed ? c.stateValue(0).i + 1 : 0;
                if (next >= 4) return -1;
                c.stateValue(0).i = next;
                c.resume = true;
                return next;
            },
            {"then0", "then1", "then2", "then3"});
        node.stateSlots = 1;
        list.push_back(std::move(node));
    }
    {
        NodeDesc node = action(
            "Flow.DoOnce", "Lets the first run through; reset lets one more.", {}, {},
            [](CircuitCall& c) {
                Value& done = c.stateValue(0);
                if (c.execInput == 1) {
                    done.i = 0;
                    return -1;
                }
                if (done.i) return -1;
                done.i = 1;
                return 0;
            },
            {"completed"}, {"exec", "reset"});
        node.stateSlots = 1;
        list.push_back(std::move(node));
    }
    {
        NodeDesc node = action(
            "Flow.Gate", "Passes enter on to exit while open; open, close and toggle change that.",
            {pin("start_closed", B, "false")}, {},
            [](CircuitCall& c) {
                Value& open = c.stateValue(0);
                Value& started = c.stateValue(1);
                if (!started.i) {
                    open.i = c.boolean(0) ? 0 : 1;
                    started.i = 1;
                }
                switch (c.execInput) {
                case 0: return open.i ? 0 : -1;
                case 1: open.i = 1; return -1;
                case 2: open.i = 0; return -1;
                default: open.i = open.i ? 0 : 1; return -1;
                }
            },
            {"exit"}, {"enter", "open", "close", "toggle"});
        node.stateSlots = 2;
        list.push_back(std::move(node));
    }
    {
        NodeDesc node = action(
            "Flow.FlipFlop", "Goes to a, then b, then a again...", {}, {pin("is_a", B)},
            [](CircuitCall& c) {
                Value& b = c.stateValue(0);
                c.out(0) = Value::boolean(!b.i);
                const int result = b.i ? 1 : 0;
                b.i = b.i ? 0 : 1;
                return result;
            },
            {"a", "b"});
        node.stateSlots = 1;
        list.push_back(std::move(node));
    }
    {
        NodeDesc node = action(
            "Flow.ForLoop", "Runs body for each index from first to last, then completed.",
            {pin("first", I, "0"), pin("last", I, "0")}, {pin("index", I)},
            [](CircuitCall& c) {
                Value& current = c.stateValue(0);
                current.i = c.resumed ? current.i + 1 : c.integer(0);
                if (current.i > c.integer(1)) return 1;
                c.out(0) = Value::integer(current.i);
                c.resume = true;
                return 0;
            },
            {"body", "completed"});
        node.stateSlots = 1;
        list.push_back(std::move(node));
    }

    // ---- Math: Float
    const auto a = pin("a", F), b = pin("b", F);
    list.push_back(
        floatMath("Math.Add", "a + b", {a, b}, [](const CircuitCall& c) { return c.number(0) + c.number(1); }));
    list.push_back(
        floatMath("Math.Subtract", "a - b", {a, b}, [](const CircuitCall& c) { return c.number(0) - c.number(1); }));
    list.push_back(floatMath("Math.Multiply", "a * b", {a, pin("b", F, "1")}, [](const CircuitCall& c) {
        return c.number(0) * c.number(1);
    }));
    list.push_back(floatMath("Math.Divide", "a / b; 0 when b is 0.", {a, pin("b", F, "1")}, [](const CircuitCall& c) {
        return c.number(1) != 0.0f ? c.number(0) / c.number(1) : 0.0f;
    }));
    list.push_back(floatMath("Math.Min", "The smaller of a and b.", {a, b}, [](const CircuitCall& c) {
        return std::min(c.number(0), c.number(1));
    }));
    list.push_back(floatMath("Math.Max", "The larger of a and b.", {a, b}, [](const CircuitCall& c) {
        return std::max(c.number(0), c.number(1));
    }));
    list.push_back(
        floatMath("Math.Abs", "a without its sign.", {a}, [](const CircuitCall& c) { return std::abs(c.number(0)); }));
    list.push_back(floatMath(
        "Math.Clamp", "value kept between min and max.", {pin("value", F), pin("min", F, "0"), pin("max", F, "1")},
        [](const CircuitCall& c) { return std::clamp(c.number(0), c.number(1), std::max(c.number(1), c.number(2))); }));
    list.push_back(floatMath(
        "Math.Lerp", "From a (alpha 0) to b (alpha 1).", {a, b, pin("alpha", F, "0.5")},
        [](const CircuitCall& c) { return c.number(0) + (c.number(1) - c.number(0)) * c.number(2); }));
    list.push_back(floatMath("Math.Sin", "Sine of degrees.", {pin("degrees", F)}, [](const CircuitCall& c) {
        return std::sin(glm::radians(c.number(0)));
    }));
    list.push_back(floatMath("Math.Cos", "Cosine of degrees.", {pin("degrees", F)}, [](const CircuitCall& c) {
        return std::cos(glm::radians(c.number(0)));
    }));
    list.push_back(floatMath(
        "Math.RandomFloat", "A random number from min to max.", {pin("min", F, "0"), pin("max", F, "1")},
        [](const CircuitCall& c) {
            const float lo = std::min(c.number(0), c.number(1)), hi = std::max(c.number(0), c.number(1));
            return std::uniform_real_distribution<float>(lo, hi)(randomEngine());
        }));
    // Comparisons and logic
    auto compare = [&](std::string id, std::string description, bool (*f)(float, float)) {
        list.push_back(pure(std::move(id), std::move(description), {a, b}, {pin("result", B)}, [f](CircuitCall& c) {
            c.out(0) = Value::boolean(f(c.number(0), c.number(1)));
            return 0;
        }));
    };
    compare("Math.Less", "a < b", [](float x, float y) { return x < y; });
    compare("Math.Greater", "a > b", [](float x, float y) { return x > y; });
    compare("Math.NearlyEqual", "a and b within 0.0001.", [](float x, float y) { return std::abs(x - y) <= 1e-4f; });
    list.push_back(pure("Logic.And", "a and b", {pin("a", B), pin("b", B)}, {pin("result", B)}, [](CircuitCall& c) {
        c.out(0) = Value::boolean(c.boolean(0) && c.boolean(1));
        return 0;
    }));
    list.push_back(pure("Logic.Or", "a or b", {pin("a", B), pin("b", B)}, {pin("result", B)}, [](CircuitCall& c) {
        c.out(0) = Value::boolean(c.boolean(0) || c.boolean(1));
        return 0;
    }));
    list.push_back(pure("Logic.Not", "not a", {pin("a", B)}, {pin("result", B)}, [](CircuitCall& c) {
        c.out(0) = Value::boolean(!c.boolean(0));
        return 0;
    }));

    // ---- Math: Int
    const auto ia = pin("a", I), ib = pin("b", I);
    auto intMath = [&](std::string id, std::string description, std::vector<PinDesc> inputs,
                       int32_t (*f)(int32_t, int32_t)) {
        list.push_back(
            pure(std::move(id), std::move(description), std::move(inputs), {pin("result", I)}, [f](CircuitCall& c) {
                c.out(0) = Value::integer(f(c.integer(0), c.integer(1)));
                return 0;
            }));
    };
    intMath("Math.AddInt", "a + b", {ia, ib}, [](int32_t x, int32_t y) { return int32_t(int64_t(x) + y); });
    intMath("Math.SubtractInt", "a - b", {ia, ib}, [](int32_t x, int32_t y) { return int32_t(int64_t(x) - y); });
    intMath("Math.MultiplyInt", "a * b", {ia, pin("b", I, "1")}, [](int32_t x, int32_t y) {
        return int32_t(int64_t(x) * y);
    });
    intMath(
        "Math.DivideInt", "a / b, rounded towards 0; 0 when b is 0.", {ia, pin("b", I, "1")},
        [](int32_t x, int32_t y) { return y != 0 && !(x == INT32_MIN && y == -1) ? x / y : 0; });
    intMath("Math.Modulo", "The remainder of a / b; 0 when b is 0.", {ia, pin("b", I, "1")}, [](int32_t x, int32_t y) {
        return y != 0 && y != -1 ? x % y : 0;
    });
    list.push_back(pure("Math.EqualInt", "a == b", {ia, ib}, {pin("result", B)}, [](CircuitCall& c) {
        c.out(0) = Value::boolean(c.integer(0) == c.integer(1));
        return 0;
    }));
    list.push_back(pure(
        "Math.RandomInt", "A random whole number from min to max, both included.",
        {pin("min", I, "0"), pin("max", I, "1")}, {pin("result", I)}, [](CircuitCall& c) {
            const int32_t lo = std::min(c.integer(0), c.integer(1)), hi = std::max(c.integer(0), c.integer(1));
            c.out(0) = Value::integer(std::uniform_int_distribution<int32_t>(lo, hi)(randomEngine()));
            return 0;
        }));

    // ---- Math: Vec3 and rotations
    const auto va = pin("a", V), vb = pin("b", V);
    auto vecMath = [&](std::string id, std::string description, std::vector<PinDesc> inputs,
                       glm::vec3 (*f)(const CircuitCall&)) {
        list.push_back(
            pure(std::move(id), std::move(description), std::move(inputs), {pin("result", V)}, [f](CircuitCall& c) {
                c.out(0) = Value::vec3(f(c));
                return 0;
            }));
    };
    vecMath(
        "Vector.Make", "A vector from x, y and z.", {pin("x", F), pin("y", F), pin("z", F)},
        [](const CircuitCall& c) { return glm::vec3(c.number(0), c.number(1), c.number(2)); });
    list.push_back(pure(
        "Vector.Break", "A vector's x, y and z.", {va}, {pin("x", F), pin("y", F), pin("z", F)}, [](CircuitCall& c) {
            const glm::vec3 v = c.vec3(0);
            c.out(0) = Value::number(v.x);
            c.out(1) = Value::number(v.y);
            c.out(2) = Value::number(v.z);
            return 0;
        }));
    vecMath("Vector.Add", "a + b", {va, vb}, [](const CircuitCall& c) { return c.vec3(0) + c.vec3(1); });
    vecMath("Vector.Subtract", "a - b", {va, vb}, [](const CircuitCall& c) { return c.vec3(0) - c.vec3(1); });
    vecMath("Vector.Scale", "vector * scale", {pin("vector", V), pin("scale", F, "1")}, [](const CircuitCall& c) {
        return c.vec3(0) * c.number(1);
    });
    vecMath(
        "Vector.Lerp", "From a (alpha 0) to b (alpha 1).", {va, vb, pin("alpha", F, "0.5")},
        [](const CircuitCall& c) { return c.vec3(0) + (c.vec3(1) - c.vec3(0)) * c.number(2); });
    vecMath(
        "Vector.Normalize", "The same direction, length 1; 0 0 0 stays.", {pin("vector", V)}, [](const CircuitCall& c) {
            const glm::vec3 v = c.vec3(0);
            const float length = glm::length(v);
            return length > 1e-12f ? v / length : glm::vec3(0.0f);
        });
    vecMath("Vector.Cross", "a x b", {va, vb}, [](const CircuitCall& c) { return glm::cross(c.vec3(0), c.vec3(1)); });
    list.push_back(floatMath("Vector.Length", "How long the vector is.", {pin("vector", V)}, [](const CircuitCall& c) {
        return glm::length(c.vec3(0));
    }));
    list.push_back(floatMath("Vector.Distance", "How far a is from b.", {va, vb}, [](const CircuitCall& c) {
        return glm::distance(c.vec3(0), c.vec3(1));
    }));
    list.push_back(floatMath(
        "Vector.Dot", "a . b", {va, vb}, [](const CircuitCall& c) { return glm::dot(c.vec3(0), c.vec3(1)); }));
    list.push_back(pure(
        "Rotation.FromEuler", "A rotation from Euler angles in degrees (X, then Y, then Z).", {pin("degrees", V)},
        {pin("result", R)}, [](CircuitCall& c) {
            c.out(0) = Value::rotation(fromEulerDegrees(c.vec3(0)));
            return 0;
        }));
    list.push_back(pure(
        "Rotation.ToEuler", "A rotation's Euler angles in degrees.", {pin("rotation", R)}, {pin("result", V)},
        [](CircuitCall& c) {
            c.out(0) = Value::vec3(eulerDegrees(c.in(0).asRotation()));
            return 0;
        }));
    list.push_back(pure(
        "Rotation.Combine", "first, then second.", {pin("first", R), pin("second", R)}, {pin("result", R)},
        [](CircuitCall& c) {
            c.out(0) = Value::rotation(glm::normalize(c.in(1).asRotation() * c.in(0).asRotation()));
            return 0;
        }));
    list.push_back(pure(
        "Rotation.AroundY", "A turn of degrees around the up axis.", {pin("degrees", F)}, {pin("result", R)},
        [](CircuitCall& c) {
            c.out(0) = Value::rotation(glm::angleAxis(glm::radians(c.number(0)), glm::vec3(0, 1, 0)));
            return 0;
        }));
    list.push_back(pure(
        "Rotation.RotateVector", "vector turned by rotation.", {pin("rotation", R), pin("vector", V)},
        {pin("result", V)}, [](CircuitCall& c) {
            c.out(0) = Value::vec3(c.in(0).asRotation() * c.vec3(1));
            return 0;
        }));

    // ---- Conversions and text
    list.push_back(pure(
        "Convert.IntToFloat", "The whole number as a number.", {pin("value", I)}, {pin("result", F)},
        [](CircuitCall& c) {
            c.out(0) = Value::number(float(c.integer(0)));
            return 0;
        }));
    list.push_back(
        pure("Convert.FloatToInt", "Rounded towards 0.", {pin("value", F)}, {pin("result", I)}, [](CircuitCall& c) {
            const float f = c.number(0);
            c.out(0) = Value::integer(std::isfinite(f) ? int32_t(std::clamp(f, -2147483520.0f, 2147483520.0f)) : 0);
            return 0;
        }));
    auto toText = [&](std::string id, PinType type) {
        list.push_back(pure(
            std::move(id), std::string("The ") + pinTypeName(type) + " as text.", {pin("value", type)},
            {pin("result", S)}, [](CircuitCall& c) {
                Value v = c.in(0);
                c.out(0) = Value::string(v.type == PinType::String ? v.text : formatValue(v));
                return 0;
            }));
    };
    toText("Text.FromFloat", F);
    toText("Text.FromInt", I);
    toText("Text.FromBool", B);
    toText("Text.FromVector", V);
    list.push_back(
        pure("Text.Append", "a followed by b.", {pin("a", S), pin("b", S)}, {pin("result", S)}, [](CircuitCall& c) {
            c.out(0) = Value::string(c.text(0) + c.text(1));
            return 0;
        }));
    list.push_back(pure(
        "Text.Equal", "a and b are the same text.", {pin("a", S), pin("b", S)}, {pin("result", B)}, [](CircuitCall& c) {
            c.out(0) = Value::boolean(c.text(0) == c.text(1));
            return 0;
        }));
    list.push_back(action(
        "Debug.Print", "Writes text to the log and the editor's Console.", {pin("text", S)}, {}, [](CircuitCall& c) {
            SW_LOG(LogCircuits, Info, "{}: {}", c.circuit, c.text(0));
            return 0;
        }));

    // ---- Entities
    list.push_back(pure(
        "Entity.Self", "The entity this circuit runs on (none in the game circuit).", {}, {pin("entity", E)},
        [](CircuitCall& c) {
            c.out(0) = Value::entityValue(c.self);
            return 0;
        }));
    list.push_back(pure(
        "Entity.FindByName", "The first entity with this Name.", {required("name", S)},
        {pin("entity", E), pin("found", B)}, [](CircuitCall& c) {
            const EntityRef found = findEntity(c.world, c.text(0));
            c.out(0) = Value::entityValue(found ? found.entity() : kNullEntity);
            c.out(1) = Value::boolean(static_cast<bool>(found));
            return 0;
        }));
    list.push_back(
        pure("Entity.IsValid", "The entity exists.", {pin("entity", E)}, {pin("result", B)}, [](CircuitCall& c) {
            c.out(0) = Value::boolean(c.world.alive(c.entity(0)));
            return 0;
        }));
    list.push_back(action(
        "Entity.Spawn", "A new copy of a prefab (\"Prefabs/Enemy.swprefab\"), where the prefab puts it.",
        {required("prefab", S)}, {pin("entity", E)}, [](CircuitCall& c) {
            c.out(0) = Value::entityValue(spawnPrefab(c.world, c.text(0)));
            return 0;
        }));
    list.push_back(action(
        "Entity.SpawnAt", "A new copy of a prefab at position.", {required("prefab", S), pin("position", V)},
        {pin("entity", E)}, [](CircuitCall& c) {
            Transform at;
            at.position = c.vec3(1);
            c.out(0) = Value::entityValue(spawnPrefab(c.world, c.text(0), &at));
            return 0;
        }));
    list.push_back(action(
        "Entity.Destroy", "Destroys the entity and everything parented under it.", {self()}, {}, [](CircuitCall& c) {
            const Entity target = c.entity(0);
            if (c.world.alive(target)) destroyWithChildren(c.world, target);
            return 0;
        }));

    // ---- Input
    list.push_back(pure(
        "Input.ActionDown", "The action is held.", {required("action", S)}, {pin("result", B)}, [](CircuitCall& c) {
            c.out(0) = Value::boolean(c.host.input && c.host.input->isActionDown(c.text(0)));
            return 0;
        }));
    list.push_back(pure(
        "Input.ActionValue", "How far the action is pressed, 0 to 1.", {required("action", S)}, {pin("result", F)},
        [](CircuitCall& c) {
            c.out(0) = Value::number(c.host.input ? c.host.input->actionValue(c.text(0)) : 0.0f);
            return 0;
        }));
    list.push_back(pure(
        "Input.Axis", "-1 to 1 from two actions, like move_left and move_right.",
        {required("negative", S), required("positive", S)}, {pin("result", F)}, [](CircuitCall& c) {
            c.out(0) = Value::number(c.host.input ? c.host.input->axis(c.text(0), c.text(1)) : 0.0f);
            return 0;
        }));

    // ---- Audio
    list.push_back(action(
        "Audio.PlaySound", "Plays a clip (\"Audio/click.wav\"), not placed in the world.",
        {required("clip", S), pin("volume", F, "1")}, {}, [](CircuitCall& c) {
            if (c.host.audio) c.host.audio->playClip(c.text(0), {.volume = c.number(1)});
            return 0;
        }));
    list.push_back(action(
        "Audio.PlaySoundAt", "Plays a clip from a place in the world.", {required("clip", S), pin("position", V)}, {},
        [](CircuitCall& c) {
            if (c.host.audio) c.host.audio->playAt(c.text(0), c.vec3(1));
            return 0;
        }));
    list.push_back(action(
        "Audio.PlaySoundOn", "Plays a clip that follows an entity.", {required("clip", S), self()}, {},
        [](CircuitCall& c) {
            if (c.host.audio) c.host.audio->playOn(c.text(0), c.entity(1));
            return 0;
        }));

    // ---- Game UI
    list.push_back(action(
        "UI.SetNumber", "Sets a {{value}} documents show.", {required("name", S), pin("value", F)}, {},
        [](CircuitCall& c) {
            if (c.host.ui) c.host.ui->set(c.text(0), double(c.number(1)));
            return 0;
        }));
    list.push_back(action(
        "UI.SetText", "Sets a {{value}} documents show, as text.", {required("name", S), pin("value", S)}, {},
        [](CircuitCall& c) {
            if (c.host.ui) c.host.ui->set(c.text(0), std::string_view(c.text(1)));
            return 0;
        }));
    list.push_back(action(
        "UI.SetBool", "Sets a true/false value documents use.", {required("name", S), pin("value", B)}, {},
        [](CircuitCall& c) {
            if (c.host.ui) c.host.ui->set(c.text(0), c.boolean(1));
            return 0;
        }));
    list.push_back(action(
        "UI.Show", "Shows a document (its UIDocument entity's name, or its file).", {required("document", S)}, {},
        [](CircuitCall& c) {
            if (c.host.ui) c.host.ui->show(c.text(0));
            return 0;
        }));
    list.push_back(action("UI.Hide", "Hides a document.", {required("document", S)}, {}, [](CircuitCall& c) {
        if (c.host.ui) c.host.ui->hide(c.text(0));
        return 0;
    }));
    list.push_back(action(
        "UI.SetElementText", "Sets the plain text of an element, by id.",
        {required("document", S), required("element", S), pin("text", S)}, {}, [](CircuitCall& c) {
            if (c.host.ui) c.host.ui->setText(c.text(0), c.text(1), c.text(2));
            return 0;
        }));

    // ---- Physics
    list.push_back(action(
        "Physics.AddImpulse", "A push to a dynamic body, in kg m/s.", {self(), pin("impulse", V)}, {},
        [](CircuitCall& c) {
            if (c.host.physics) c.host.physics->addImpulse(c.entity(0), c.vec3(1));
            return 0;
        }));
    list.push_back(action(
        "Physics.SetVelocity", "Sets a body's speed and direction, in m/s.", {self(), pin("velocity", V)}, {},
        [](CircuitCall& c) {
            if (c.host.physics) c.host.physics->setVelocity(c.entity(0), c.vec3(1));
            return 0;
        }));
    list.push_back(pure(
        "Physics.Raycast", "The first collider along a ray.",
        {pin("origin", V), pin("direction", V, "0 0 -1"), pin("distance", F, "100"), self("ignore")},
        {pin("hit", B), pin("entity", E), pin("point", V), pin("distance", F)}, [](CircuitCall& c) {
            std::optional<RayHit> hit;
            if (c.host.physics) hit = c.host.physics->raycast(c.vec3(0), c.vec3(1), c.number(2), c.entity(3));
            c.out(0) = Value::boolean(hit.has_value());
            c.out(1) = Value::entityValue(hit ? hit->entity : kNullEntity);
            c.out(2) = Value::vec3(hit ? hit->point : glm::vec3(0.0f));
            c.out(3) = Value::number(hit ? hit->distance : 0.0f);
            return 0;
        }));

    for (NodeDesc& node : list) {
        const std::string id = node.id;
        nodes[id] = std::make_shared<const NodeDesc>(std::move(node));
    }
}

// ---- Nodes made from component reflection ------------------------------------------------

bool pinTypeForField(FieldType type, PinType& pin) {
    switch (type) {
    case FieldType::Bool: pin = PinType::Bool; return true;
    case FieldType::Int:
    case FieldType::Enum: pin = PinType::Int; return true;
    case FieldType::Float: pin = PinType::Float; return true;
    case FieldType::Vec2: pin = PinType::Vec2; return true;
    case FieldType::Vec3: pin = PinType::Vec3; return true;
    case FieldType::Color: pin = PinType::Color; return true;
    case FieldType::Rotation: pin = PinType::Rotation; return true;
    case FieldType::String:
    case FieldType::AudioClip:
    case FieldType::UIDocument: pin = PinType::String; return true;
    case FieldType::Mesh: pin = PinType::Mesh; return true;
    case FieldType::Texture: pin = PinType::Texture; return true;
    case FieldType::Model: pin = PinType::Model; return true;
    case FieldType::Entity: pin = PinType::Entity; return true;
    }
    return false;
}

Value readField(const void* component, const Field& field) {
    const char* p = static_cast<const char*>(component) + field.offset;
    switch (field.type) {
    case FieldType::Bool: return Value::boolean(*reinterpret_cast<const bool*>(p));
    case FieldType::Int:
    case FieldType::Enum: return Value::integer(*reinterpret_cast<const int*>(p));
    case FieldType::Float: return Value::number(*reinterpret_cast<const float*>(p));
    case FieldType::Vec2: return Value::vec2(*reinterpret_cast<const glm::vec2*>(p));
    case FieldType::Vec3: return Value::vec3(*reinterpret_cast<const glm::vec3*>(p));
    case FieldType::Color: return Value::color(*reinterpret_cast<const glm::vec4*>(p));
    case FieldType::Rotation: return Value::rotation(*reinterpret_cast<const glm::quat*>(p));
    case FieldType::String:
    case FieldType::AudioClip:
    case FieldType::UIDocument: return Value::string(*reinterpret_cast<const std::string*>(p));
    case FieldType::Entity: return Value::entityValue(*reinterpret_cast<const Entity*>(p));
    case FieldType::Mesh: {
        Value v = defaultValue(PinType::Mesh);
        v.i = static_cast<int32_t>(reinterpret_cast<const MeshHandle*>(p)->id);
        return v;
    }
    case FieldType::Texture: {
        Value v = defaultValue(PinType::Texture);
        v.i = static_cast<int32_t>(reinterpret_cast<const TextureHandle*>(p)->id);
        return v;
    }
    case FieldType::Model: {
        Value v = defaultValue(PinType::Model);
        v.i = static_cast<int32_t>(reinterpret_cast<const ModelHandle*>(p)->id);
        return v;
    }
    }
    return {};
}

void writeField(void* component, const Field& field, const CircuitCall& call, size_t pinIndex) {
    char* p = static_cast<char*>(component) + field.offset;
    const Value& v = call.in(pinIndex);
    switch (field.type) {
    case FieldType::Bool: *reinterpret_cast<bool*>(p) = v.asBool(); break;
    case FieldType::Int: *reinterpret_cast<int*>(p) = v.i; break;
    case FieldType::Enum:
        // Enums only take their named values.
        if (v.i >= 0 && (field.options.empty() || size_t(v.i) < field.options.size())) *reinterpret_cast<int*>(p) = v.i;
        break;
    case FieldType::Float:
        if (std::isfinite(v.v.x)) {
            float f = v.v.x;
            if (field.max > field.min) f = std::clamp(f, field.min, field.max);
            *reinterpret_cast<float*>(p) = f;
        }
        break;
    case FieldType::Vec2: *reinterpret_cast<glm::vec2*>(p) = v.asVec2(); break;
    case FieldType::Vec3: *reinterpret_cast<glm::vec3*>(p) = v.asVec3(); break;
    case FieldType::Color: *reinterpret_cast<glm::vec4*>(p) = v.v; break;
    case FieldType::Rotation: {
        const glm::quat q = v.asRotation();
        const float length = glm::length(q);
        if (length > 1e-12f && std::isfinite(length)) *reinterpret_cast<glm::quat*>(p) = q / length;
        break;
    }
    case FieldType::String:
    case FieldType::AudioClip:
    case FieldType::UIDocument: *reinterpret_cast<std::string*>(p) = v.text; break;
    case FieldType::Entity: *reinterpret_cast<Entity*>(p) = call.entity(pinIndex); break;
    case FieldType::Mesh: {
        MeshHandle handle{static_cast<uint32_t>(v.i)};
        if (!v.text.empty() && call.host.assets) handle = call.host.assets->findMesh(v.text);
        *reinterpret_cast<MeshHandle*>(p) = handle;
        break;
    }
    case FieldType::Texture: {
        TextureHandle handle{static_cast<uint32_t>(v.i)};
        if (!v.text.empty() && call.host.assets)
            if (const auto found = call.host.assets->findTexture(v.text)) handle = *found;
        *reinterpret_cast<TextureHandle*>(p) = handle;
        break;
    }
    case FieldType::Model: {
        ModelHandle handle{static_cast<uint32_t>(v.i)};
        if (!v.text.empty() && call.host.assets) handle = call.host.assets->findModel(v.text);
        *reinterpret_cast<ModelHandle*>(p) = handle;
        break;
    }
    }
}

// A component's registration, looked up when a node runs. A game module's functions must
// never be copied into nodes: compiled circuits can outlive the module.
struct ComponentRef {
    std::string name;
    uint32_t id = 0;
    const ComponentInfo* info() const {
        const ComponentInfo* found = findComponent(name);
        return found && found->id == id ? found : nullptr;
    }
    void* get(World& world, Entity e) const {
        const ComponentInfo* i = info();
        return i && i->get ? i->get(world, e) : nullptr;
    }
    bool has(const World& world, Entity e) const {
        const ComponentInfo* i = info();
        return i && i->has && i->has(world, e);
    }
    void add(World& world, Entity e) const {
        const ComponentInfo* i = info();
        if (i && i->add && world.alive(e)) i->add(world, e);
    }
    void remove(World& world, Entity e) const {
        const ComponentInfo* i = info();
        if (i && i->remove && world.alive(e)) i->remove(world, e);
    }
};

void addReflected(std::map<std::string, std::shared_ptr<const NodeDesc>>& nodes, const ComponentInfo& info) {
    const std::string prefix = "Component." + info.name;
    struct Pinned {
        Field field;
        PinType type;
    };
    std::vector<Pinned> fields;
    for (const Field& field : info.fields) {
        PinType type;
        if (pinTypeForField(field.type, type)) fields.push_back({field, type});
    }
    const ComponentRef component{info.name, info.id};
    const uint32_t id = info.id;
    auto finish = [&](NodeDesc node, bool reads, bool writes) {
        node.category = "Component";
        node.component = id;
        node.readsComponent = reads;
        node.writesComponent = writes;
        const std::string key = node.id;
        nodes[key] = std::make_shared<const NodeDesc>(std::move(node));
    };

    {
        std::vector<PinDesc> outputs{pin("found", PinType::Bool)};
        for (const auto& f : fields) outputs.push_back(pin(f.field.name, f.type));
        finish(
            pure(
                prefix + ".Get", "Every field of the entity's " + info.name + "; found is false when it has none.",
                {self()}, outputs,
                [component, fields](CircuitCall& c) {
                    void* data = component.get(c.world, c.entity(0));
                    c.out(0) = Value::boolean(data != nullptr);
                    for (size_t i = 0; i < fields.size(); ++i)
                        c.out(i + 1) = data ? readField(data, fields[i].field) : defaultValue(fields[i].type);
                    return 0;
                }),
            true, false);
    }
    {
        std::vector<PinDesc> inputs{self()};
        for (const auto& f : fields) inputs.push_back(pin(f.field.name, f.type));
        finish(
            action(
                prefix + ".Set",
                "Sets the fields given (connected or written) of the entity's " + info.name +
                    "; the rest stay as they are. Nothing happens if it has none.",
                inputs, {},
                [component, fields](CircuitCall& c) {
                    void* data = component.get(c.world, c.entity(0));
                    if (!data) return 0;
                    const auto* site = static_cast<const CircuitProgram::CallSite*>(c.site);
                    for (size_t i = 0; i < fields.size(); ++i)
                        if (site && i + 1 < site->given.size() && site->given[i + 1])
                            writeField(data, fields[i].field, c, i + 1);
                    return 0;
                }),
            false, true);
    }
    finish(
        pure(
            prefix + ".Has", "The entity has a " + info.name + ".", {self()}, {pin("result", PinType::Bool)},
            [component](CircuitCall& c) {
                c.out(0) = Value::boolean(component.has(c.world, c.entity(0)));
                return 0;
            }),
        true, false);
    finish(
        action(
            prefix + ".Add", "Gives the entity a " + info.name + " with default values, if it has none.", {self()}, {},
            [component](CircuitCall& c) {
                component.add(c.world, c.entity(0));
                return 0;
            }),
        false, true);
    finish(
        action(
            prefix + ".Remove", "Takes the " + info.name + " off the entity.", {self()}, {},
            [component](CircuitCall& c) {
                component.remove(c.world, c.entity(0));
                return 0;
            }),
        false, true);
    for (const auto& f : fields) {
        const Field field = f.field;
        const PinType type = f.type;
        finish(
            pure(
                prefix + ".Get." + field.name, "The entity's " + info.name + " " + field.name + ".", {self()},
                {pin(field.name, type)},
                [component, field, type](CircuitCall& c) {
                    void* data = component.get(c.world, c.entity(0));
                    c.out(0) = data ? readField(data, field) : defaultValue(type);
                    return 0;
                }),
            true, false);
        finish(
            action(
                prefix + ".Set." + field.name, "Sets the entity's " + info.name + " " + field.name + ".",
                {self(), required(field.name, type)}, {},
                [component, field](CircuitCall& c) {
                    if (void* data = component.get(c.world, c.entity(0))) writeField(data, field, c, 1);
                    return 0;
                }),
            false, true);
    }
}

uint64_t componentSignature() {
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
    for (const ComponentInfo& info : registeredComponents()) {
        mix(info.id);
        mix(std::hash<std::string>{}(info.name));
        mix(info.fields.size());   // a reloaded game module's components get new ids, too
    }
    return hash;
}

struct Registry {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<const NodeDesc>> builtin;
    struct Registered {
        std::shared_ptr<const NodeDesc> node;
        uint64_t token = 0;
    };
    std::map<std::string, Registered> registered;
    uint64_t nextToken = 1;
    uint64_t version = 1;
    uint64_t signature = 0;
    std::map<std::string, std::shared_ptr<const NodeDesc>> reflected;

    Registry() { addBuiltins(builtin); }

    // Called with the lock held.
    void refreshReflected() {
        const uint64_t now = componentSignature();
        if (now == signature && !reflected.empty()) return;
        signature = now;
        reflected.clear();
        for (const ComponentInfo& info : registeredComponents()) addReflected(reflected, info);
    }
};

// Never destroyed: nodes from game modules that are already unloaded must not be torn
// down at exit.
Registry& registry() {
    static Registry* instance = new Registry();
    return *instance;
}

}   // namespace

uint64_t registerNode(NodeDesc node) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    if (node.category.empty()) node.category = node.id.substr(0, node.id.find('.'));
    const uint64_t token = r.nextToken++;
    const std::string id = node.id;
    r.registered[id] = {std::make_shared<const NodeDesc>(std::move(node)), token};
    ++r.version;
    return token;
}

void unregisterNode(std::string_view id, uint64_t token) {
    std::shared_ptr<const NodeDesc> removed;   // released after the lock
    {
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        const auto found = r.registered.find(std::string(id));
        if (found == r.registered.end() || found->second.token != token) return;
        removed = std::move(found->second.node);
        r.registered.erase(found);
        ++r.version;
    }
    // Compiled circuits may hold the node, whose code is about to be unloaded.
    circuitLibrary().clear();
}

std::shared_ptr<const NodeDesc> findNode(std::string_view id) {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    const std::string key(id);
    if (const auto found = r.registered.find(key); found != r.registered.end()) return found->second.node;
    if (const auto found = r.builtin.find(key); found != r.builtin.end()) return found->second;
    if (key.rfind("Component.", 0) == 0) {
        r.refreshReflected();
        if (const auto found = r.reflected.find(key); found != r.reflected.end()) return found->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<const NodeDesc>> allNodes() {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.refreshReflected();
    std::map<std::string, std::shared_ptr<const NodeDesc>> all = r.reflected;
    for (const auto& [id, node] : r.builtin) all[id] = node;
    for (const auto& [id, entry] : r.registered) all[id] = entry.node;
    std::vector<std::shared_ptr<const NodeDesc>> result;
    result.reserve(all.size());
    for (auto& [id, node] : all) result.push_back(node);
    return result;
}

uint64_t nodeRegistryVersion() {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    return (r.version * 0x9e3779b97f4a7c15ull) ^ componentSignature();
}

void writeNodeSchema(std::ostream& out) {
    const auto nodes = allNodes();
    out << "# Circuit nodes\n\n"
        << "Generated by `player --node-schema docs/CircuitNodes.md`; don't edit by hand. "
        << "[Circuits](Circuits.md) explains the file format and how circuits run.\n\n"
        << "Every node below is used as `node <name> <Id>`. Pins without a value use the default shown. "
        << "Required pins must be connected or given a value; constant pins must be a literal.\n\n"
        << "Besides these, every circuit variable has two nodes: `Get.<variable>` (output `value`) and "
        << "`Set.<variable>` (exec, input `value`, output `value`). The `Component.*` nodes are made from "
        << "the components registered when this was written, the game's own included.\n";
    std::string category;
    for (const auto& node : nodes) {
        if (node->category != category) {
            category = node->category;
            out << "\n## " << category << "\n";
        }
        out << "\n### " << node->id << "\n\n" << node->description << "\n\n";
        auto list = [&](const char* label, const std::vector<std::string>& names) {
            if (names.empty()) return;
            out << "- " << label << ":";
            for (const auto& name : names) out << " `" << name << "`";
            out << "\n";
        };
        if (node->event != CircuitEvent::None)
            out << "- Event: " << circuitEventName(node->event) << " ("
                << (eventAllowed(node->event, CircuitKind::Entity) && eventAllowed(node->event, CircuitKind::Game)
                        ? "entity and game circuits"
                    : eventAllowed(node->event, CircuitKind::Entity) ? "entity circuits"
                                                                     : "the game circuit")
                << ")\n";
        list("Exec in", node->execInputs);
        list("Exec out", node->execOutputs);
        for (const PinDesc& p : node->inputs) {
            out << "- In `" << p.name << "` " << pinTypeName(p.type);
            if (p.constant) out << ", constant";
            if (p.required)
                out << ", required";
            else if (!p.defaultValue.empty())
                out << ", default " << p.defaultValue;
            out << "\n";
        }
        for (const PinDesc& p : node->outputs) out << "- Out `" << p.name << "` " << pinTypeName(p.type) << "\n";
    }
}

}   // namespace shockwave
