#pragma once

#include <Assets/Model.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/World.h>
#include <Renderer/Renderer.h>

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
    glm::vec2 tiling{1.0f};   // texture repeats across the surface, per axis
    bool visible = true;
    float roughness = 0.6f;   // 0 mirror-smooth .. 1 fully matte
    float metallic = 0.0f;   // 0 plastic, wood, stone .. 1 bare metal
    glm::vec4 emissive{0, 0, 0, 1};   // light the surface gives off itself (rgb)
    float emissiveIntensity = 1.0f;   // multiplies emissive; above 1 for bright glows
    bool castShadows = true;
};

// A reusable imported model. Its internal nodes/materials stay in the asset;
// this entity's transform places the complete model, and tint multiplies colors.
struct ModelRenderer {
    ModelHandle model;
    glm::vec4 tint{1.0f};
    bool visible = true;
    bool castShadows = true;
};

// What the player sees: a game renders the scene from the primary Camera's world
// transform, looking down its -Z axis (see primaryCamera in Systems.h). The editor's
// Play button looks through it too.
struct Camera {
    float fov = 60.0f;   // vertical field of view, degrees
    float nearPlane = 0.1f;   // units; there is no far plane (reverse-Z, infinite)
    bool primary = true;   // with several cameras, the first primary one wins
};

// Scene-wide settings. Give one entity one; with several, any of them may win.
// Scene-wide look: the sky behind everything, the light that arrives from all around
// (image-based lighting: it fills shadows and shows in reflections), and how HDR light
// becomes screen colors. One per scene; without one the defaults below apply.
// Without an environment map, the surroundings are skyColor above the horizon and
// groundColor below it, times ambientIntensity. docs/Lighting.md explains both.
struct Environment {
    enum class Tonemapper : int { ACES, AgX, Reinhard, None };
    glm::vec4 skyColor{0.32f, 0.42f, 0.58f, 1.0f};
    glm::vec4 groundColor{0.18f, 0.16f, 0.14f, 1.0f};
    float ambientIntensity = 0.6f;
    // A panorama of the surroundings (equirectangular, 2:1, ideally a .hdr file) that lights
    // the scene and shows in reflections in place of the sky and ground colors.
    TextureHandle environmentMap{};   // 0, the white texture: none
    float environmentIntensity = 1.0f;   // multiplies the map's light
    float environmentRotation = 0.0f;   // degrees about the vertical axis
    bool showEnvironment = true;   // draw the map behind everything instead of skyColor
    float exposure = 0.0f;   // in stops: +1 doubles brightness, -1 halves it
    Tonemapper tonemapper = Tonemapper::ACES;
    // Very bright light (the sun in a reflection, glowing emissives) spreading into its
    // surroundings like it does through a camera lens. 0 turns it off.
    float bloomIntensity = 0.04f;
    // The sun's shadows: how far from the camera they reach, and in how many cascades
    // (more keeps nearby shadows sharp across long distances).
    float shadowDistance = 80.0f;   // meters
    int shadowCascades = 4;   // 1..4
};

// A light source, placed and aimed by the entity's Transform: directional and spot
// lights shine along its forward axis (-Z). Intensity 1 lights a white surface facing
// the light to full white; point and spot lights fade to nothing at range.
struct Light {
    enum class Type : int { Directional, Point, Spot };
    Type type = Type::Point;
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;   // point and spot: meters
    float spotAngle = 30.0f;   // spot: half-angle of the cone, degrees
    float spotSoftness = 0.2f;   // spot: 0 hard edge .. 1 fades from the center
    bool enabled = true;
    // Shadows. The first shadow-casting directional light gets the sun's cascaded shadows;
    // spot and point lights cast while there's room in the shadow atlas, nearest first (a
    // point light costs six times a spot light).
    bool castShadows = true;
    float shadowStrength = 1.0f;   // 0 no shadow .. 1 fully dark
    float shadowSoftness = 0.3f;   // 0 sharp .. 1 very soft edges
    float shadowBias = 1.0f;   // raise if lit surfaces show dark speckles; lower if shadows detach
};

// Turns around an axis (world space) at a steady rate.
struct Spin {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};
    float speed = 90.0f;   // degrees per second
};

// Floats up and down on a sine wave.
struct Bob {
    float amplitude = 0.25f;   // units
    float frequency = 0.5f;   // cycles per second
    float phase = 0.0f;   // radians
    // Runtime state, not saved:
    float time = 0.0f;
    float offset = 0.0f;   // height currently applied to the Transform
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
enum class FieldType {
    Bool,
    Int,
    Enum,
    Float,
    Vec2,
    Vec3,
    Color,
    Rotation,
    String,
    AudioClip,
    Mesh,
    Texture,
    Model,
    Entity,
    UIDocument   // a std::string: an .rml document path inside Content (GameUI)
};

struct Field {
    std::string name;
    FieldType type;
    size_t offset;
    float speed = 0.0f;   // editor drag speed; 0 picks a default for the type
    float min = 0.0f;   // editor limits; min == max means none
    float max = 0.0f;
    std::vector<std::string> options{};   // named values for int-backed enums
};

template <class T>
constexpr FieldType fieldTypeOf() {
    if constexpr (std::is_same_v<T, bool>)
        return FieldType::Bool;
    else if constexpr (std::is_enum_v<T>) {
        static_assert(std::is_same_v<std::underlying_type_t<T>, int>, "reflected enums must use int storage");
        return FieldType::Enum;
    } else if constexpr (std::is_same_v<T, int>)
        return FieldType::Int;
    else if constexpr (std::is_same_v<T, float>)
        return FieldType::Float;
    else if constexpr (std::is_same_v<T, glm::vec2>)
        return FieldType::Vec2;
    else if constexpr (std::is_same_v<T, glm::vec3>)
        return FieldType::Vec3;
    else if constexpr (std::is_same_v<T, glm::vec4>)
        return FieldType::Color;
    else if constexpr (std::is_same_v<T, glm::quat>)
        return FieldType::Rotation;
    else if constexpr (std::is_same_v<T, std::string>)
        return FieldType::String;
    else if constexpr (std::is_same_v<T, MeshHandle>)
        return FieldType::Mesh;
    else if constexpr (std::is_same_v<T, TextureHandle>)
        return FieldType::Texture;
    else if constexpr (std::is_same_v<T, ModelHandle>)
        return FieldType::Model;
    else if constexpr (std::is_same_v<T, Entity>)
        return FieldType::Entity;
    else
        static_assert(sizeof(T) == 0, "unsupported field type");
}

template <class C, class M>
Field field(std::string name, M C::*member, float speed = 0.0f, float min = 0.0f, float max = 0.0f) {
    const C probe{};
    const size_t offset =
        static_cast<size_t>(reinterpret_cast<const char*>(&(probe.*member)) - reinterpret_cast<const char*>(&probe));
    return Field{std::move(name), fieldTypeOf<M>(), offset, speed, min, max, {}};
}

template <class C, class E>
Field enumField(std::string name, E C::*member, std::vector<std::string> options) {
    static_assert(std::is_enum_v<E>);
    auto result = field(std::move(name), member);
    result.options = std::move(options);
    return result;
}

// Type-erased access to a registered component.
struct ComponentInfo {
    std::string name;
    std::string category;   // groups the editor's Add Component menu ("Rendering", say)
    uint32_t id = 0;
    std::vector<Field> fields;
    std::function<bool(const World&, Entity)> has;
    std::function<void*(World&, Entity)> get;   // nullptr if missing
    std::function<const void*(const World&, Entity)> getConst;
    std::function<void(World&, Entity)> add;   // adds a default-constructed one
    std::function<void(World&, Entity)> remove;
    std::function<void(const World& from, Entity, World& to, Entity)> copy;   // between worlds (prefabs)
};

namespace detail {
void addComponentInfo(ComponentInfo info);
// Unregisters a component, if it's still registered under that id: game code that's
// unloaded removes its types, unless a reloaded copy has registered them again.
void removeComponentInfo(std::string_view name, uint32_t id);
}   // namespace detail

// Registers (or re-registers) T. Do it at startup on the main thread; the built-in
// components above are registered already.
template <class T>
void registerComponent(std::string name, std::vector<Field> fields, std::string category = "Gameplay") {
    ComponentInfo info;
    info.name = std::move(name);
    info.category = std::move(category);
    info.id = componentId<T>();
    info.fields = std::move(fields);
    info.has = [](const World& w, Entity e) { return w.has<T>(e); };
    info.get = [](World& w, Entity e) -> void* { return w.get<T>(e); };
    info.getConst = [](const World& w, Entity e) -> const void* { return w.get<T>(e); };
    info.add = [](World& w, Entity e) {
        if (!w.has<T>(e)) w.add<T>(e);
    };
    info.remove = [](World& w, Entity e) { w.remove<T>(e); };
    info.copy = [](const World& from, Entity a, World& to, Entity b) {
        if (const T* c = from.get<T>(a)) to.add<T>(b, *c);
    };
    detail::addComponentInfo(std::move(info));
}

// In registration order: built-ins first (Name, Transform, Parent, MeshRenderer, Camera,
// Environment, Spin, Bob), then the game's own.
const std::vector<ComponentInfo>& registeredComponents();
const ComponentInfo* findComponent(std::string_view name);

// Euler angles in degrees <-> quaternion, the convention files and the inspector use.
glm::vec3 eulerDegrees(const glm::quat& q);
glm::quat fromEulerDegrees(const glm::vec3& degrees);

}   // namespace shockwave
