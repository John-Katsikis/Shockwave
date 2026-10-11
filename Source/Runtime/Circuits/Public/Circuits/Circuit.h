#pragma once

// Circuits: Shockwave's visual scripting, like Unreal's Blueprints. A circuit is a node
// graph in a .swcircuit file inside Content. White "exec" wires say what happens in which
// order, starting from an event (BeginPlay, FixedTick, an input action...); data wires
// carry values between pins. docs/Circuits.md is the guide.
//
// Two kinds:
//   - an entity circuit runs on every entity with a Circuit component that names it;
//   - the game circuit (game_circuit in the .swproject) runs once for the whole game,
//     alongside the C++ GameMode.
//
// Loading compiles a circuit into register bytecode shared by everything that runs it
// (CircuitProgram.h); CircuitWorld runs it during Play. The file is plain text, so people,
// diffs and AI tools can all read it:
//
//   shockwave_circuit 1
//   var speed Float 90
//
//   node tick Event.FixedTick
//
//   node spin Component.Transform.Set.rotation
//     exec tick.then
//     in rotation = turned.result
//
//   node turned Rotation.Combine
//     in first = now.rotation
//     in second = step.result
//   ...
//
//   editor
//     tick -320 0
//     spin 0 0
//   end

#include <Core/Log.h>
#include <Engine/World.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

SW_LOG_CATEGORY(LogCircuits);

// ---- Values ----------------------------------------------------------------------------

// What a pin carries. Exec pins carry no value, only "this happens next".
enum class PinType : uint8_t {
    Exec,
    Bool,
    Int,
    Float,
    Vec2,
    Vec3,
    Color,
    Rotation,
    String,
    Entity,
    Mesh,
    Texture,
    Model
};
const char* pinTypeName(PinType type);   // "Float", "Vec3", ...
bool parsePinType(std::string_view name, PinType& type);

// One register of a running circuit. Small and plain on purpose: the VM copies them.
struct Value {
    PinType type = PinType::Bool;
    // Float: x. Vec2: xy. Vec3: xyz. Color: rgba. Rotation: a quaternion as x y z w.
    glm::vec4 v{0.0f};
    int32_t i = 0;   // Bool (0 or 1), Int; Mesh, Texture and Model handle ids
    // Entity: the entity, if it's known. Otherwise text says which one: "self", or the
    // Name of the entity to look up when it's used. Empty text and no entity is "none".
    Entity entity;
    // String; for Entity, see above; for Mesh, Texture and Model, an asset name to look up
    // when the id isn't known yet.
    std::string text;

    static Value boolean(bool b);
    static Value integer(int32_t n);
    static Value number(float f);
    static Value vec2(const glm::vec2& v);
    static Value vec3(const glm::vec3& v);
    static Value color(const glm::vec4& c);
    static Value rotation(const glm::quat& q);
    static Value string(std::string s);
    static Value entityValue(Entity e);
    static Value self();   // "the entity this circuit runs on"

    bool asBool() const { return i != 0; }
    glm::vec2 asVec2() const { return glm::vec2(v); }
    glm::vec3 asVec3() const { return glm::vec3(v); }
    glm::quat asRotation() const { return glm::quat(v.w, v.x, v.y, v.z); }
};

// The value a pin of this type has when nothing is connected and nothing is written.
Value defaultValue(PinType type);
// Literal text, as written after `in pin =` and in var lines: true, 3, 2.5, "text",
// 1 2 3 (vectors), 0 90 0 (rotations, Euler degrees), self or "Name" (entities).
bool parseValue(std::string_view text, PinType type, Value& value, std::string& error);
std::string formatValue(const Value& value);

// ---- Components --------------------------------------------------------------------------

// Runs an entity circuit on this entity during Play. Saved as the "Circuit" component.
struct Circuit {
    std::string circuit;   // inside Content, forward slashes: "Circuits/Door.swcircuit"
    bool enabled = true;
};

// This entity's circuit variables, where they differ from the circuit's defaults:
// `speed=2.5; open=true`. Written while playing, so they show in the Inspector and go
// into save games; set them in a scene to give one entity its own values.
struct CircuitVars {
    std::string values;
};

// Registers Circuit and CircuitVars for scene files, the Inspector and player --schema.
void registerCircuitComponents();

// `name=value; name=value` <-> pairs, values as formatValue writes them. Quotes protect
// ; and = inside strings.
std::vector<std::pair<std::string, std::string>> parseCircuitVars(std::string_view text);
std::string formatCircuitVars(const std::vector<std::pair<std::string, std::string>>& values);

// ---- The file ------------------------------------------------------------------------------

// A .swcircuit as written, before it's compiled. parseCircuit only checks the syntax;
// compileCircuit (CircuitProgram.h) checks that it makes sense.
struct CircuitDocument {
    struct Variable {
        std::string name;
        PinType type = PinType::Float;
        std::string value;   // literal text; empty: the type's default
        int line = 0;
    };
    // `in pin = text`: a literal, a variable's name, or node.pin.
    struct Input {
        std::string pin, text;
        int line = 0;
    };
    // `exec node.output` or `exec pin = node.output`: what runs this node.
    struct Exec {
        std::string pin;   // this node's exec input; empty for its first
        std::string node, output;
        int line = 0;
    };
    struct Node {
        std::string name, type;
        std::vector<Input> inputs;
        std::vector<Exec> execs;
        glm::vec2 position{0.0f};   // from the editor block
        bool placed = false;
        int line = 0;
    };
    std::string description;   // the comment lines before the first var or node
    std::vector<Variable> variables;
    std::vector<Node> nodes;
};

// file names the source in errors ("file:line: message").
bool parseCircuit(std::istream& in, const std::string& file, CircuitDocument& document, std::string& error);
bool parseCircuitText(std::string_view text, const std::string& file, CircuitDocument& document, std::string& error);
void writeCircuit(std::ostream& out, const CircuitDocument& document);

}   // namespace shockwave
