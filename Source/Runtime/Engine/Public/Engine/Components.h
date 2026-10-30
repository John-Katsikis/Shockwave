#pragma once

#include <Assets/Material.h>
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
    // A material asset (.swmaterial, A02). When set it decides the surface: texture,
    // roughness, metallic and emissive above are ignored, tint multiplies its base color
    // and tiling its UVs.
    MaterialHandle material{};
};

// A reusable imported model. Its internal nodes/materials stay in the asset;
// this entity's transform places the complete model, and tint multiplies colors.
// A material asset in place of one of a model's own materials (A02): part is the source
// material's name, as the model file has it. Names, not positions, so reordering the
// materials in the modelling tool keeps every override.
struct MaterialOverride {
    std::string part;
    MaterialHandle material;
    bool operator==(const MaterialOverride&) const = default;
};
using MaterialOverrides = std::vector<MaterialOverride>;

struct ModelRenderer {
    ModelHandle model;
    glm::vec4 tint{1.0f};
    bool visible = true;
    bool castShadows = true;
    MaterialOverrides materials{};   // parts drawn with a material asset instead of their own
};
// The override for a model's source material, or null.
const MaterialOverride* findMaterialOverride(const MaterialOverrides& overrides, std::string_view part);

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
    // Screen-space ambient occlusion, when the player's ambient_occlusion setting is on: how
    // far (meters) a surface looks for nearby geometry blocking light from its surroundings,
    // and how strongly that darkens it (0 none, 1 as measured, more darker).
    float aoRadius = 0.3f;
    float aoIntensity = 1.0f;
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

// ---- Baked lighting (G05, G06; docs/Lighting.md) ----
// Light that bounces around a scene's walls is baked offline into lightmaps (for static
// surfaces) and probe volumes (for things that move). Nothing is baked unless the scene
// says so; scenes without these components light exactly as before.

// Whether an object takes part in a bake. Only static objects do: they occlude and bounce
// light in the bake, and with receive_lightmap get a lightmap of their own. Being still
// isn't enough; a dynamic object never blocks baked light. binding_id ties the object to
// its bake: 32 lowercase hex digits, assigned by the editor (and `player
// --lighting-assign-ids`), unique in the scene, kept when the scene is reordered.
struct BakeGeometry {
    enum class Mobility : int { Static, Dynamic };
    std::string bindingId;
    Mobility mobility = Mobility::Dynamic;
    bool castIndirect = true;   // blocks and bounces light in the bake
    bool receiveLightmap = false;   // gets a lightmap (static only)
    float texelsPerMeter = 16.0f;   // lightmap density
};
// On an entity with a Light: the light it gives static surfaces is bounced around in the
// bake. Its direct light stays real-time, with shadows, either way.
struct BakeLight {
    bool includeIndirect = true;
};
// The scene's bake: asset is the .swlighting manifest inside Content ("Lighting/Arena.swlighting").
// One per scene. enabled false keeps the bake but draws without it.
// reflections is the scene's captured reflection probes, a .swreflection manifest (G07,
// "Lighting/Arena.swreflection"), drawn whether or not the scene has a lightmap bake.
struct BakedLighting {
    std::string asset;
    bool enabled = true;
    std::string reflections;
};
// A box of light probes, for diffuse light on moving objects: placed and turned by the
// entity's Transform, probes every spacing metres, fading out blend_distance inside its
// faces. Where volumes overlap the higher priority wins.
struct DiffuseProbeVolume {
    glm::vec3 halfExtents{4.0f, 2.0f, 4.0f};
    float spacing = 1.0f;   // metres between probes
    float blendDistance = 0.5f;
    int priority = 0;
    bool enabled = true;
    std::string bindingId;
};

// A box that local reflections come from (G07): surfaces inside it reflect a cube captured
// offline from a point in it, box-projected so nearby walls line up, instead of the global
// environment. Placed and turned by the entity's Transform (its scale stretches the box),
// fading out blend_distance inside its faces; where boxes overlap the higher priority goes
// over the lower one. Captured with `player --capture-reflections` or the editor's Lighting
// window, never automatically: it shows static surfaces as they were then, not players.
struct ReflectionProbe {
    enum class Resolution : int { R64, R128, R256, R512 };
    glm::vec3 halfExtents{4.0f, 2.0f, 4.0f};   // the box, along the entity's own axes
    glm::vec3 captureOffset{0.0f};   // where it's captured from, in the box's own frame; inside it
    float blendDistance = 0.5f;   // metres
    int priority = 0;
    Resolution resolution = Resolution::R128;   // per face; 128 is about 1 MiB on the GPU
    float nearPlane = 0.05f;   // what the capture sees: from this far from the capture point...
    float farPlane = 500.0f;   // ...to this far
    bool enabled = true;
    std::string bindingId;   // ties it to its captured cube, like BakeGeometry's
};
uint32_t reflectionProbeSize(ReflectionProbe::Resolution resolution);   // 64, 128, 256 or 512

// ---- Decals (G08, A03; docs/Decals.md) ----

// A projected decal: a box, placed and turned by the entity's Transform (its scale stretches
// it too) and sized by size, that paints onto the opaque surfaces inside it. It projects
// along the entity's -Z, so it lands on surfaces facing its +Z; texture u runs along +X and
// v down -Y. Blended surfaces never take decals. Draws when the player's decals_quality
// setting allows, nearest first.
struct Decal {
    // A .swmaterial: its base colour texture and colour (alpha is coverage), and with
    // roughness_metallic its roughness and metallic. Without one, texture, tint and the
    // values below do it. Normal and emissive maps aren't decal channels and are ignored.
    MaterialHandle material{};
    TextureHandle texture{};
    glm::vec3 size{1.0f, 1.0f, 0.2f};   // metres: width, height, projection depth
    float opacity = 1.0f;   // 0..1
    glm::vec4 tint{1.0f};   // multiplies the colour (linear)
    bool albedo = true;   // changes base colour
    bool roughnessMetallic = false;   // changes roughness and metallic, together
    float roughness = 0.5f, metallic = 0.0f;   // without a material
    int sortOrder = 0;   // higher draws over lower; ties go to the one made later
    int receiverMask = 0xFF;   // lands on surfaces whose DecalReceiver layers share a bit
    float angleFade = 60.0f;   // degrees turned away before it starts fading; gone side-on
    bool essential = false;   // a gameplay marker: kept first on low decal budgets
    bool enabled = true;
};
// Which decal layers a mesh or model is on (eight bits). Without one, layer 1; layers 0
// takes no decals. A decal on a door can then land on the door alone.
struct DecalReceiver {
    int layers = 1;
};
// A decal that goes away by itself (impact marks): fades out over the last fade seconds
// of its lifetime and is then destroyed. It's also destroyed when the entity it was stuck
// to is, so a reused entity never inherits another's marks. 0 seconds lasts until removed.
struct DecalLifetime {
    float seconds = 30.0f;
    float fade = 2.0f;
    // Runtime, not saved:
    float age = 0.0f;
    Entity receiver{};   // what it was stuck to, if anything
    uint64_t spawn = 0;   // counts up per impact decal, for drawing order and eviction
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

// Where to draw an entity this frame, when something other than the last two ticks
// decides it: a network client's interpolated remote objects and its predicted character
// with a correction being smoothed out (NetworkWorld::present). renderWorld uses it in
// place of blending PreviousTransform and Transform; gameplay and physics never read it.
// Runtime only and never saved.
struct PresentationTransform {
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
    UIDocument,   // a std::string: an .rml document path inside Content (GameUI)
    Material,   // a MaterialHandle: a .swmaterial in Content (A02)
    MaterialOverrides   // ModelRenderer's per-part materials
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
    else if constexpr (std::is_same_v<T, MaterialHandle>)
        return FieldType::Material;
    else if constexpr (std::is_same_v<T, MaterialOverrides>)
        return FieldType::MaterialOverrides;
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
