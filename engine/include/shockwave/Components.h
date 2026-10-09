#pragma once

#include <shockwave/Renderer.h>
#include <shockwave/World.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

// ---- Built-in components -------------------------------------------------------------

struct Name {
    std::string value;
};

// Where an entity is, relative to its Parent if it has one (otherwise to the world).
// Rotation is a quaternion; files and the editor show it as Euler angles in degrees
// (X, then Y, then Z). worldMatrix() in Hierarchy.h gives the final placement.
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    glm::mat4 matrix() const;
    // Splits a translate * rotate * scale matrix (no shear) back into parts.
    static Transform fromMatrix(const glm::mat4& m);
    static Transform blend(const Transform& a, const Transform& b, float t);
};

// Attaches an entity to another: its Transform becomes relative to the parent's, so it
// moves, turns and scales with it. Use setParent (Hierarchy.h) to attach without the
// child jumping. A parent that's destroyed leaves its children at the top level.
struct Parent {
    Entity entity;
};

struct MeshRenderer {
    MeshHandle mesh;
    TextureHandle texture;
    glm::vec4 tint{1.0f};
    bool visible = true;
};

// Scene-wide settings. Give one entity one; with several, any of them may win.
struct Environment {
    glm::vec4 skyColor{0.32f, 0.42f, 0.58f, 1.0f};
};

// Turns around an axis (world space) at a steady rate.
struct Spin {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float speed = 90.0f;   // degrees per second
};

// Floats up and down on a sine wave.
struct Bob {
    float amplitude = 0.25f;   // units
    float frequency = 0.5f;    // cycles per second
    float phase = 0.0f;        // radians
    // Runtime state, not saved:
    float time = 0.0f;
    float offset = 0.0f;       // height currently applied to the Transform
};

// The Transform as of the previous tick, kept by storePreviousTransforms so rendering
// can blend between ticks. Engine-managed and never saved.
struct PreviousTransform {
    Transform value;
};

// ---- Reflection ----------------------------------------------------------------------
// Registering a component with its fields puts it in scene files and gives it an
// editor inspector, with no other code:
//
//   struct Health { float current = 100, max = 100; };
//   registerComponent<Health>("Health", {field("current", &Health::current), field("max", &Health::max)});

// Entity fields refer to another entity in the same world; scene files save them as
// the position of that entity's block in the file.
enum class FieldType { Bool, Int, Float, Vec3, Color, Rotation, String, Mesh, Texture, Entity };

struct Field {
    std::string name;
    FieldType type;
    size_t offset;
    float speed = 0.0f;   // editor drag speed; 0 picks a default for the type
    float min = 0.0f;     // editor limits; min == max means none
    float max = 0.0f;
};

template <class T>
constexpr FieldType fieldTypeOf() {
    if constexpr (std::is_same_v<T, bool>) return FieldType::Bool;
    else if constexpr (std::is_same_v<T, int>) return FieldType::Int;
    else if constexpr (std::is_same_v<T, float>) return FieldType::Float;
    else if constexpr (std::is_same_v<T, glm::vec3>) return FieldType::Vec3;
    else if constexpr (std::is_same_v<T, glm::vec4>) return FieldType::Color;
    else if constexpr (std::is_same_v<T, glm::quat>) return FieldType::Rotation;
    else if constexpr (std::is_same_v<T, std::string>) return FieldType::String;
    else if constexpr (std::is_same_v<T, MeshHandle>) return FieldType::Mesh;
    else if constexpr (std::is_same_v<T, TextureHandle>) return FieldType::Texture;
    else if constexpr (std::is_same_v<T, Entity>) return FieldType::Entity;
    else static_assert(sizeof(T) == 0, "unsupported field type");
}

template <class C, class M>
Field field(std::string name, M C::*member, float speed = 0.0f, float min = 0.0f, float max = 0.0f) {
    const C probe{};
    const size_t offset = static_cast<size_t>(reinterpret_cast<const char*>(&(probe.*member)) -
                                              reinterpret_cast<const char*>(&probe));
    return Field{std::move(name), fieldTypeOf<M>(), offset, speed, min, max};
}

// Type-erased access to a registered component.
struct ComponentInfo {
    std::string name;
    uint32_t id = 0;
    std::vector<Field> fields;
    std::function<bool(const World&, Entity)> has;
    std::function<void*(World&, Entity)> get;            // nullptr if missing
    std::function<const void*(const World&, Entity)> getConst;
    std::function<void(World&, Entity)> add;             // adds a default-constructed one
    std::function<void(World&, Entity)> remove;
};

namespace detail {
void addComponentInfo(ComponentInfo info);
}

// Registers (or re-registers) T. Do it at startup on the main thread; the built-in
// components above are registered already.
template <class T>
void registerComponent(std::string name, std::vector<Field> fields) {
    ComponentInfo info;
    info.name = std::move(name);
    info.id = componentId<T>();
    info.fields = std::move(fields);
    info.has = [](const World& w, Entity e) { return w.has<T>(e); };
    info.get = [](World& w, Entity e) -> void* { return w.get<T>(e); };
    info.getConst = [](const World& w, Entity e) -> const void* { return w.get<T>(e); };
    info.add = [](World& w, Entity e) {
        if (!w.has<T>(e)) w.add<T>(e);
    };
    info.remove = [](World& w, Entity e) { w.remove<T>(e); };
    detail::addComponentInfo(std::move(info));
}

// In registration order: built-ins first (Name, Transform, Parent, MeshRenderer,
// Environment, Spin, Bob), then the game's own.
const std::vector<ComponentInfo>& registeredComponents();
const ComponentInfo* findComponent(std::string_view name);

// Euler angles in degrees <-> quaternion, the convention files and the inspector use.
glm::vec3 eulerDegrees(const glm::quat& q);
glm::quat fromEulerDegrees(const glm::vec3& degrees);

} // namespace shockwave
