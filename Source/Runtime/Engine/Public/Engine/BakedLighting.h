#pragma once

// Baked lighting in a scene (G05, G06; docs/Lighting.md): which objects take part and with
// what identity, what a bake depends on, and the runtime that draws a scene with its bake.
// The baker itself is the Lighting module; the files are Assets/LightingData.h.

#include <Assets/Image.h>
#include <Assets/LightingData.h>
#include <Engine/Components.h>
#include <Renderer/Mesh.h>
#include <Renderer/RenderTypes.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace shockwave {

class Assets;
class Renderer;
struct ModelAsset;

// ---- Identity ----

// The binding id an object is baked under: its own (BakeGeometry or DiffuseProbeVolume
// binding_id) combined with every enclosing prefab copy's bake_id, innermost first
// (deriveBindingId). Invalid, with the reason in problem, when an id is missing or malformed.
BindingId effectiveBindingId(const World& world, Entity e, const std::string& localId, std::string* problem = nullptr);
// One message per missing, malformed or duplicate id among the scene's bake participants
// (static BakeGeometry and DiffuseProbeVolume), and per prefab copy holding participants
// without a bake_id. Empty when every participant can be bound.
std::vector<std::string> checkLightingIds(const World& world);
// Gives every participant that needs one a fresh id (missing, malformed, or a duplicate
// after the first), and prefab copies holding participants a bake_id. Returns how many it
// set. Never called by checking or baking: the editor does it when objects are made or
// duplicated, and `player --lighting-assign-ids` on request.
size_t assignLightingIds(World& world);
// The editor after a duplicate or paste: fresh ids for e and everything under it.
void refreshLightingIds(World& world, Entity e);

// ---- What a bake sees ----

// The material a bake bounces light off: a diffuse approximation of the runtime one.
struct BakeMaterial {
    glm::vec3 albedo{0.8f};   // base colour times tint, linear
    float alpha = 1.0f;   // base colour alpha times tint alpha, for masks
    float metallic = 0.0f;
    glm::vec3 emissive{0.0f};   // linear, already times intensity
    float alphaCutoff = -1.0f;   // >= 0: masked (holes let light through)
    bool blend = false;   // see-through surfaces are left out of the bake, with a warning
    bool doubleSided = false;
    // The base colour texture, decoded, when it could be: its colour (sRGB) multiplies
    // albedo and its alpha drives the mask. Null: the factors alone.
    std::shared_ptr<const Image> baseColor;
    TextureBinding binding;   // UV set, transform and sampler of the base colour slot
    glm::vec2 uvScale{1.0f};
    // rho = albedo * (1 - metallic) * 0.96, clamped to 0..0.96: never amplifies light.
    glm::vec3 reflectance(const glm::vec3& texel = glm::vec3(1.0f)) const;
};

// One mesh of a static participant, in world space.
struct BakeSurface {
    Entity entity;
    BindingId binding;
    uint32_t part = 0;   // modelPartKey for a model's node and mesh, 0 for a MeshRenderer
    std::shared_ptr<const MeshData> mesh;   // its vertices and triangles as rendered
    std::shared_ptr<const ModelAsset> model;   // keeps a model part's mesh alive
    glm::mat4 toWorld{1.0f};
    bool receiver = false;   // gets a lightmap
    bool occluder = true;   // blocks and bounces light
    float texelsPerMeter = 16.0f;
    BakeMaterial material;
    uint64_t geometryHash = 0;   // lightingMeshHash(*mesh)
    uint64_t hash = 0;   // geometry, placement, material and density: its own dependencies
};
// A light whose bounce is baked (a Light with BakeLight include_indirect).
struct BakeLightSource {
    Entity entity;
    LightDesc light;
};
// The scene's surroundings as rendered: the environment map, or sky above and ground below.
struct BakeEnvironment {
    glm::vec3 sky{0.0f}, ground{0.0f};   // already times ambient_intensity
    std::shared_ptr<const HdrImage> map;   // decoded environment map, if any
    std::string mapName;
    float intensity = 1.0f;
    float rotation = 0.0f;   // radians about +Y
    // Radiance arriving from direction (pointing away from the receiver), exactly as the
    // renderer's environment cube is built (shaders/environment_sky.comp).
    glm::vec3 radiance(const glm::vec3& direction) const;
};
struct BakeVolume {
    Entity entity;
    BindingId binding;
    DiffuseProbeVolume volume;
    glm::mat4 toWorld{1.0f};
    uint64_t hash = 0;
};
struct BakeSceneDescription {
    std::vector<BakeSurface> surfaces;
    std::vector<BakeLightSource> lights;
    BakeEnvironment environment;
    std::vector<BakeVolume> volumes;
    // Lights and surroundings (not settings: lightingManifestHash adds those).
    uint64_t lightingHash = 0;
    // Every static occluder and receiver (their own hashes, in binding order).
    uint64_t geometryHash = 0;
    std::vector<std::string> warnings;   // skipped objects, missing ids, unsupported textures
    const BakeSurface* surface(const BindingId& binding, uint32_t part) const;
};
// Gathers the scene's static participants, baked lights and environment. contentRoot is
// where texture and environment map names are found, for decoding their pixels; empty
// leaves them out (factors alone, no map: the hashes still cover their names). Decoded
// images are cached by file and modification time, so calling it every frame is cheap.
BakeSceneDescription
describeBakeScene(const World& world, const Assets& assets, const std::filesystem::path& contentRoot);
// What a mesh contributes to a bake: positions, normals, UVs and triangles.
uint64_t lightingMeshHash(const MeshData& mesh);
// The manifest's lighting_hash: the scene's lighting hash with the bake settings that change
// every texel.
uint64_t lightingManifestHash(uint64_t sceneLightingHash, const LightingManifest& settings);
// The source mesh re-indexed for a receiver: vertex i copies receiver.sourceVertex[i] (UV1
// too) and gets its lightmap UV. False when the indices don't fit the source mesh.
bool receiverMesh(const MeshData& source, const LightingReceiver& receiver, MeshData& out);

// ---- Drawing with a bake ----

// Loads a scene's bake and hands renderWorld the lighting each object draws with: static
// receivers their own mesh with lightmap UVs and their atlas, other objects a probe sample.
// Everything not covered draws as before. Replacing a bake uploads the new one completely
// before switching, and frees the old after; a bake that fails to load keeps the last good one.
class BakedLightingRuntime {
public:
    BakedLightingRuntime();
    BakedLightingRuntime(const BakedLightingRuntime&) = delete;
    BakedLightingRuntime& operator=(const BakedLightingRuntime&) = delete;
    ~BakedLightingRuntime();

    // Main thread, between frames, before renderWorld: follows the scene's BakedLighting
    // asset, reloads it when its files change on disk (checking at most every interval
    // seconds; dt is the frame time), and checks each receiver still matches its bake.
    void update(const World& world, const Assets& assets, Renderer& renderer, double dt = 0.0, double interval = 0.5);
    // Frees everything it made. Main thread; before the renderer goes.
    void release();
    // Loads again on the next update, whether or not the files changed (after a bake).
    void reload() { m_forceReload = true; }

    // During renderWorld, from any thread: what a static receiver's part draws with
    // (its own mesh and lightmap), or null to draw it as usual.
    struct Receiver {
        MeshHandle mesh;
        IndirectLighting lighting;
    };
    const Receiver* receiver(Entity e, uint32_t part) const;
    // A moving object's probe sample at point (its bounds centre). False outside every
    // volume: draw it with the environment, as usual.
    bool probeLighting(const glm::vec3& point, IndirectLighting& out) const;
    // Whether e draws with probes at all (no static BakeGeometry of its own).
    bool usesProbes() const { return m_probesReady; }

    enum class State { None, Disabled, Missing, Invalid, Stale, Ready };
    struct Status {
        State state = State::None;
        std::string asset;   // the manifest, inside Content
        std::string reason;   // why it isn't Ready
        uint32_t receivers = 0;   // in the bake
        uint32_t boundReceivers = 0;   // drawn with their lightmap this frame
        uint32_t invalidReceivers = 0;   // moved or changed since: drawn with the environment
        uint32_t volumes = 0, probes = 0;
        uint64_t gpuBytes = 0;   // lightmaps and receiver meshes
        uint64_t generation = 0;   // counts successful loads
    };
    const Status& status() const { return m_status; }
    static const char* stateName(State state);   // "none", "disabled", ...

private:
    struct Loaded;
    void load(
        const World& world, const Assets& assets, Renderer& renderer, const std::string& asset,
        const std::filesystem::path& contentRoot);
    void free(Loaded& loaded);
    void report(const std::string& message);

    Renderer* m_renderer = nullptr;
    std::unique_ptr<Loaded> m_loaded;
    std::unordered_map<uint64_t, Receiver> m_bound;   // by entity index and part, this frame
    LightingProbeSet m_probes;   // the loaded bake's volumes still where they were baked
    std::vector<bool> m_probeMask;   // which of them, as of m_probeGeneration
    uint64_t m_probeGeneration = 0;
    bool m_probesReady = false;
    State m_loadState = State::None;   // why the last load failed, while nothing is loaded
    std::string m_loadError;
    Status m_status;
    std::string m_asset;
    std::filesystem::file_time_type m_stamp{};
    double m_poll = 0.0;
    bool m_forceReload = false;
    std::string m_lastReport;
};

// What `player --lighting-info` and `--check` say about a scene's bake, without a GPU: the
// manifest, whether its payloads are whole, and which receivers and volumes still match.
struct LightingBakeInfo {
    BakedLightingRuntime::State state = BakedLightingRuntime::State::None;
    std::string asset;   // the scene's BakedLighting asset; empty: the scene has none
    std::string reason;   // why it isn't Ready
    LightingManifest manifest;   // when it loaded
    uint64_t payloadBytes = 0;
    uint32_t currentReceivers = 0;   // still as baked
    std::vector<std::string> changedReceivers;   // moved, edited or gone since: "Wall (part 0)"
    std::vector<std::string> unbakedReceivers;   // static receivers the bake doesn't have
    uint32_t currentVolumes = 0;
    std::vector<std::string> changedVolumes;   // moved, resized, gone or new
    bool geometryChanged = false;   // some static occluder changed: bounces are out of date
    // The manifest or a payload is damaged (not merely out of date): a rebake is the fix.
    bool broken = false;
};
LightingBakeInfo
inspectLightingBake(const World& world, const Assets& assets, const std::filesystem::path& contentRoot);

}   // namespace shockwave
