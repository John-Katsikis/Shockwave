#pragma once

// The nodes circuits are built from. Three sources:
//   - built in: events, flow (Branch, Sequence, DoOnce, Gate...), math, entities, input,
//     audio, UI (CircuitNodes.cpp);
//   - made from component reflection: for every registered component, including a game's
//     SW_COMPONENTs, Component.<Name>.Get / Set / Has / Add / Remove, and
//     Component.<Name>.Get.<field> / Set.<field> for each field;
//   - a game's C++ functions, with SW_NODE:
//
//         SW_NODE("Combat", "ApplyDamage", [](shockwave::CircuitCall& call, shockwave::Entity target, float amount) {
//             if (auto* health = call.world.get<Health>(target)) health->current -= amount;
//         }, "target", "amount")
//
//     makes the node Combat.ApplyDamage with inputs target and amount. A non-void result
//     is the output "result". SW_PURE_NODE makes a node without exec pins, for values.
//     They come and go with the game module, hot reload included.
//
// `player --node-schema docs/CircuitNodes.md` lists every node with its pins.

#include <Circuits/Circuit.h>
#include <Engine/Components.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace shockwave {

class AudioWorld;
class Assets;
class Input;
class PhysicsWorld;
class UIWorld;

// What circuits can reach while they run: the session's services. Any may be null (tools
// and tests); nodes that need a missing one do nothing.
struct CircuitHost {
    Input* input = nullptr;
    AudioWorld* audio = nullptr;
    UIWorld* ui = nullptr;
    PhysicsWorld* physics = nullptr;
    const Assets* assets = nullptr;
};

// Events start a circuit running. Which kind of circuit may use which is checked when it
// compiles (eventAllowed).
enum class CircuitEvent : uint8_t {
    None,
    BeginPlay,
    EndPlay,
    FixedTick,
    Frame,
    ActionPressed,
    ActionReleased,
    TriggerEnter,
    TriggerExit,
    BeforeSave,
    AfterLoad,
    UICommand
};
enum class CircuitKind : uint8_t { Entity, Game };
const char* circuitEventName(CircuitEvent event);
bool eventAllowed(CircuitEvent event, CircuitKind kind);

struct PinDesc {
    std::string name;
    PinType type = PinType::Float;
    std::string defaultValue;   // literal text; empty: the type's default
    bool required = false;   // must be connected or given a value
    bool constant = false;   // must be a literal (an event's action name, say)
    std::vector<std::string> options{};   // reflected int-backed enum labels
};

struct NodeDesc;

// A node running: its pins' registers, and the world it runs in.
struct CircuitCall {
    World& world;
    Entity self;   // kNullEntity in the game circuit
    const CircuitHost& host;
    const NodeDesc& node;
    std::string_view circuit;   // the file, for messages
    int execInput = 0;   // which exec input started it (Gate: enter, open, close, toggle)
    // Set when the node asked to resume (see resume) and the wire it fired has finished.
    bool resumed = false;
    // The node sets this to run again, with resumed set, once everything after the exec
    // output it returns has finished: how Sequence and ForLoop run one wire after another.
    bool resume = false;

    std::span<Value> regs;
    const uint32_t* inputs = nullptr;
    const uint32_t* outputs = nullptr;
    uint32_t state = 0;   // first of the node's state registers (NodeDesc::stateSlots)
    const void* site = nullptr;   // the compiled call site (reflected nodes read their fields)

    const Value& in(size_t pin) const { return regs[inputs[pin]]; }
    Value& out(size_t pin) const { return regs[outputs[pin]]; }
    Value& stateValue(size_t slot) const { return regs[state + slot]; }
    bool boolean(size_t pin) const { return in(pin).asBool(); }
    int32_t integer(size_t pin) const { return in(pin).i; }
    float number(size_t pin) const { return in(pin).v.x; }
    glm::vec3 vec3(size_t pin) const { return in(pin).asVec3(); }
    const std::string& text(size_t pin) const { return in(pin).text; }
    // An Entity input made real: self, a name looked up, or kNullEntity.
    Entity entity(size_t pin) const;
};

// Returns the exec output to follow (its index in execOutputs), or -1 to stop. Pure nodes
// return anything; it's ignored.
using NodeFn = std::function<int(CircuitCall&)>;

struct NodeDesc {
    std::string id;   // "Flow.Branch": the type written after `node <name>`
    std::string category;   // "Flow"
    std::string description;
    std::vector<std::string> execInputs;   // empty for pure nodes and events
    std::vector<std::string> execOutputs;
    std::vector<PinDesc> inputs, outputs;
    CircuitEvent event = CircuitEvent::None;   // event nodes start a wire; they have no exec inputs
    uint32_t stateSlots = 0;   // registers the node keeps between runs (DoOnce's "done")
    bool setsVariable = false;   // Set.<variable>: marks the circuit's variables changed
    // Reflected component nodes: which component, and what the node reads or writes.
    uint32_t component = 0;
    bool readsComponent = false, writesComponent = false;
    NodeFn run;

    bool pure() const { return execInputs.empty() && event == CircuitEvent::None; }
};

// Registers a node, replacing one with the same id. The token unregisters it.
uint64_t registerNode(NodeDesc node);
// Removes the node if it's still the one registered with this token (a hot-reloaded game
// module registers its nodes again before the old one goes).
void unregisterNode(std::string_view id, uint64_t token);
// Built-in, registered and reflected nodes. Null when there's no such node.
std::shared_ptr<const NodeDesc> findNode(std::string_view id);
// Every node, sorted by id. Reflected ones are made for the components registered now.
std::vector<std::shared_ptr<const NodeDesc>> allNodes();
// Changes whenever nodes or components are registered or removed: compiled circuits made
// before are out of date.
uint64_t nodeRegistryVersion();
// The markdown reference player --node-schema writes.
void writeNodeSchema(std::ostream& out);

// ---- SW_NODE: C++ functions as nodes -------------------------------------------------------

namespace detail {

template <class T>
using NodeArg = std::remove_cvref_t<T>;

template <class T>
constexpr PinType pinTypeOf() {
    using U = NodeArg<T>;
    if constexpr (std::is_same_v<U, bool>)
        return PinType::Bool;
    else if constexpr (std::is_same_v<U, int> || std::is_same_v<U, int32_t>)
        return PinType::Int;
    else if constexpr (std::is_same_v<U, float>)
        return PinType::Float;
    else if constexpr (std::is_same_v<U, glm::vec2>)
        return PinType::Vec2;
    else if constexpr (std::is_same_v<U, glm::vec3>)
        return PinType::Vec3;
    else if constexpr (std::is_same_v<U, glm::vec4>)
        return PinType::Color;
    else if constexpr (std::is_same_v<U, glm::quat>)
        return PinType::Rotation;
    else if constexpr (std::is_same_v<U, std::string>)
        return PinType::String;
    else if constexpr (std::is_same_v<U, Entity>)
        return PinType::Entity;
    else
        static_assert(
            sizeof(U) == 0, "SW_NODE pins can be bool, int, float, glm::vec2/vec3/vec4/quat, "
                            "std::string or Entity");
}

template <class T>
NodeArg<T> fromValue(const CircuitCall& call, size_t pin) {
    using U = NodeArg<T>;
    const Value& v = call.in(pin);
    if constexpr (std::is_same_v<U, bool>)
        return v.asBool();
    else if constexpr (std::is_same_v<U, int> || std::is_same_v<U, int32_t>)
        return v.i;
    else if constexpr (std::is_same_v<U, float>)
        return v.v.x;
    else if constexpr (std::is_same_v<U, glm::vec2>)
        return v.asVec2();
    else if constexpr (std::is_same_v<U, glm::vec3>)
        return v.asVec3();
    else if constexpr (std::is_same_v<U, glm::vec4>)
        return v.v;
    else if constexpr (std::is_same_v<U, glm::quat>)
        return v.asRotation();
    else if constexpr (std::is_same_v<U, std::string>)
        return v.text;
    else
        return call.entity(pin);
}

template <class T>
Value toValue(const T& value) {
    using U = NodeArg<T>;
    if constexpr (std::is_same_v<U, bool>)
        return Value::boolean(value);
    else if constexpr (std::is_same_v<U, int> || std::is_same_v<U, int32_t>)
        return Value::integer(value);
    else if constexpr (std::is_same_v<U, float>)
        return Value::number(value);
    else if constexpr (std::is_same_v<U, glm::vec2>)
        return Value::vec2(value);
    else if constexpr (std::is_same_v<U, glm::vec3>)
        return Value::vec3(value);
    else if constexpr (std::is_same_v<U, glm::vec4>)
        return Value::color(value);
    else if constexpr (std::is_same_v<U, glm::quat>)
        return Value::rotation(value);
    else if constexpr (std::is_same_v<U, std::string>)
        return Value::string(value);
    else
        return Value::entityValue(value);
}

// The parameter list and result of a lambda or function pointer.
template <class F>
struct Signature : Signature<decltype(&F::operator())> {};
template <class C, class R, class... A>
struct Signature<R (C::*)(A...) const> {
    using Result = R;
    using Args = std::tuple<A...>;
};
template <class C, class R, class... A>
struct Signature<R (C::*)(A...)> {
    using Result = R;
    using Args = std::tuple<A...>;
};
template <class R, class... A>
struct Signature<R (*)(A...)> {
    using Result = R;
    using Args = std::tuple<A...>;
};

template <class Tuple>
constexpr bool takesCall() {
    if constexpr (std::tuple_size_v<Tuple> == 0)
        return false;
    else
        return std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0, Tuple>>, CircuitCall>;
}

// Builds a NodeDesc whose pins come from fn's parameters (after an optional leading
// CircuitCall&) and result. names name the inputs in order; missing ones are a, b, c...
template <class F>
NodeDesc
makeFunctionNode(std::string category, std::string name, F fn, bool pure, std::vector<std::string> names = {}) {
    using Sig = Signature<std::decay_t<F>>;
    using Args = typename Sig::Args;
    using Result = typename Sig::Result;
    constexpr bool withCall = takesCall<Args>();
    constexpr size_t first = withCall ? 1 : 0;
    constexpr size_t count = std::tuple_size_v<Args> - first;
    NodeDesc node;
    node.category = category;
    node.id = category + "." + name;
    node.description = "C++ function " + name + ".";
    if (!pure) {
        node.execInputs = {"exec"};
        node.execOutputs = {"then"};
    }
    [&]<size_t... I>(std::index_sequence<I...>) {
        (node.inputs.push_back(
             PinDesc{
                 I < names.size() ? names[I] : std::string(1, char('a' + I)),
                 pinTypeOf<std::tuple_element_t<I + first, Args>>(),
                 {},
                 false,
                 false}),
         ...);
    }(std::make_index_sequence<count>{});
    if constexpr (!std::is_void_v<Result>)
        node.outputs.push_back(PinDesc{"result", pinTypeOf<Result>(), {}, false, false});
    node.run = [fn = std::move(fn)](CircuitCall& call) -> int {
        auto invoke = [&]<size_t... I>(std::index_sequence<I...>) {
            if constexpr (withCall)
                return fn(call, fromValue<std::tuple_element_t<I + first, Args>>(call, I)...);
            else
                return fn(fromValue<std::tuple_element_t<I + first, Args>>(call, I)...);
        };
        if constexpr (std::is_void_v<Result>)
            invoke(std::make_index_sequence<count>{});
        else
            call.out(0) = toValue(invoke(std::make_index_sequence<count>{}));
        return 0;
    };
    return node;
}

// What SW_NODE expands to. The function and the pin names arrive together, so commas
// inside the function's body don't confuse the preprocessor.
template <class F, class... Names>
NodeDesc functionNode(std::string category, std::string name, bool pure, F fn, const Names&... names) {
    return makeFunctionNode(
        std::move(category), std::move(name), std::move(fn), pure, std::vector<std::string>{std::string(names)...});
}

// One per SW_NODE: registers when the game module loads, unregisters when it unloads.
struct AutoNode {
    std::string id;
    uint64_t token = 0;
    explicit AutoNode(NodeDesc node) : id(node.id), token(registerNode(std::move(node))) {}
    ~AutoNode() { unregisterNode(id, token); }
    AutoNode(const AutoNode&) = delete;
    AutoNode& operator=(const AutoNode&) = delete;
};

}   // namespace detail
}   // namespace shockwave

#define SW_NODE_CONCAT_INNER(a, b) a##b
#define SW_NODE_CONCAT(a, b) SW_NODE_CONCAT_INNER(a, b)

// A C++ function as a node with exec pins: SW_NODE("Category", "Name", fn, "pin", ...).
#define SW_NODE(category, name, ...)                                                                                   \
    static const ::shockwave::detail::AutoNode SW_NODE_CONCAT(swNode_, __COUNTER__){                                   \
        ::shockwave::detail::functionNode(category, name, false, __VA_ARGS__)};
// A C++ function as a pure node (no exec pins), for values: SW_PURE_NODE("Math", "Wave", fn).
#define SW_PURE_NODE(category, name, ...)                                                                              \
    static const ::shockwave::detail::AutoNode SW_NODE_CONCAT(swNode_, __COUNTER__){                                   \
        ::shockwave::detail::functionNode(category, name, true, __VA_ARGS__)};
