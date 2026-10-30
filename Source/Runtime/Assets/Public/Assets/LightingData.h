#pragma once

// Baked lighting data (G05, G06; docs/Lighting.md, generated docs/LightingSchema.md): the
// persistent identities that bind a bake to scene objects, the .swlighting manifest, the
// .swbake receiver and .swprobe probe payloads, linear RGBA16F .ktx2 lightmaps, and the
// spherical harmonics probes store. CPU only: inspecting and checking a bake needs no GPU.
//
// Every stored diffuse value is B = E / pi, linear scene RGB before exposure and tone
// mapping: the light arriving at a surface, not the surface's colour times it. The
// renderer's environment irradiance cube uses the same convention, so a baked value
// replaces it one for one.

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

// ---- Persistent identity ----

// 128 bits, written as 32 lowercase hexadecimal digits. Scene objects that take part in a
// bake carry one, so a bake finds the same wall after the scene is reordered, renamed or
// has walls duplicated. Zero is "none".
struct BindingId {
    uint64_t hi = 0, lo = 0;
    bool valid() const { return hi != 0 || lo != 0; }
    auto operator<=>(const BindingId&) const = default;
};
// Exactly 32 lowercase hex digits; anything else (upper case, dashes, short) is refused.
bool parseBindingId(std::string_view text, BindingId& id);
std::string formatBindingId(const BindingId& id);
BindingId newBindingId();   // random, never zero
// The identity of an object inside a prefab copy: the copy's own id combined with the
// object's id in the prefab file, so two copies of one prefab never share lighting. Two
// 64-bit FNV-1a hashes (offset bases 0xcbf29ce484222325 and 0x84222325cbf29ce4) of the
// namespace "shockwave.bake.prefab.1", then instance and local, each hi then lo, little-
// endian. Applied once per enclosing prefab copy, innermost first.
BindingId deriveBindingId(const BindingId& instance, const BindingId& local);

// ---- Content hashes ----

// 64-bit FNV-1a over explicit little-endian values: the same scene hashes the same on every
// machine. Floats hash by their bits (with -0 made +0), never by printed text.
class LightingHasher {
public:
    LightingHasher& bytes(std::span<const uint8_t> data);
    LightingHasher& u32(uint32_t value);
    LightingHasher& u64(uint64_t value);
    LightingHasher& f32(float value);
    LightingHasher& vec(const glm::vec2& v) { return f32(v.x).f32(v.y); }
    LightingHasher& vec(const glm::vec3& v) { return f32(v.x).f32(v.y).f32(v.z); }
    LightingHasher& vec(const glm::vec4& v) { return f32(v.x).f32(v.y).f32(v.z).f32(v.w); }
    LightingHasher& mat(const glm::mat4& m);
    LightingHasher& text(std::string_view s);   // length, then the bytes
    LightingHasher& id(const BindingId& b) { return u64(b.hi).u64(b.lo); }
    uint64_t value() const { return m_hash; }

private:
    uint64_t m_hash = 0xcbf29ce484222325ull;
};
uint64_t hashLightingBytes(std::span<const uint8_t> data);
std::string formatLightingHash(uint64_t hash);   // 16 lowercase hex digits
bool parseLightingHash(std::string_view text, uint64_t& hash);

// ---- Spherical harmonics (G06) ----

// Nine real coefficients per colour channel, in this order (x, y, z a unit direction in the
// engine's world axes; the ordering is mathematical, Y stays up):
//   0 Y00   0.282095
//   1 Y1-1  0.488603 y        2 Y10  0.488603 z        3 Y11  0.488603 x
//   4 Y2-2  1.092548 xy       5 Y2-1 1.092548 yz       6 Y20  0.315392 (3z^2 - 1)
//   7 Y21   1.092548 xz       8 Y22  0.546274 (x^2 - y^2)
using Sh9 = std::array<glm::vec3, 9>;
std::array<float, 9> shBasis(const glm::vec3& direction);
// Radiance coefficients (L_i = 4 pi / N * sum L(d) Y_i(d) over uniform directions) to
// coefficients of irradiance / pi: bands scaled by 1, 2/3 and 1/4.
Sh9 shRadianceToIrradiance(const Sh9& radiance);
// Irradiance / pi for a surface facing normal; negative ringing is clamped to zero, the
// coefficients themselves never are.
glm::vec3 shEvaluate(const Sh9& irradiance, const glm::vec3& normal);
// A constant radiance c everywhere, as irradiance / pi coefficients: evaluates to c at every normal.
Sh9 shConstant(const glm::vec3& c);

// ---- Lightmap payload: uncompressed linear RGBA16F .ktx2 ----

// One level, VK_FORMAT_R16G16B16A16_SFLOAT, no supercompression, a basic data format
// descriptor (linear, RGBA). rgba is width * height * 4 floats, rows top to bottom; alpha
// is 1 where a chart covers the texel and 0 in unused space. Values round to half floats.
bool encodeLightmapKtx2(
    uint32_t width, uint32_t height, std::span<const float> rgba, std::vector<uint8_t>& ktx2, std::string& error);
// Only what encodeLightmapKtx2 writes (and any KTX2 of that exact shape): anything else,
// a truncated file or an overflowing size, is an error. rgba comes back as floats.
bool decodeLightmapKtx2(
    std::span<const uint8_t> ktx2, uint32_t& width, uint32_t& height, std::vector<float>& rgba, std::string& error);
// The half-float bits the renderer uploads, from decodeLightmapKtx2's floats.
std::vector<uint8_t> lightmapHalfFloats(std::span<const float> rgba);

// ---- Receivers payload (.swbake) ----

// One baked surface: an object's mesh (part: modelPartKey of a model's node and mesh, 0 for
// a MeshRenderer) re-indexed with lightmap UVs. Generated charts split vertices, so each
// output vertex names the source vertex it copies (all its attributes, UV1 included) and
// adds its own lightmap UV in its atlas (0..1, texel centres at (i + 0.5) / size).
struct LightingReceiver {
    BindingId binding;
    uint32_t part = 0;
    uint32_t atlas = 0;   // Lightmap_<atlas>.ktx2
    uint64_t geometryHash = 0;   // the source mesh, as baked (lightingMeshHash)
    std::vector<uint32_t> sourceVertex;
    std::vector<glm::vec2> lightmapUv;   // one per output vertex
    std::vector<uint32_t> indices;   // into the output vertices
};
// Layout, all little-endian, no padding:
//   "SWBAKE\0\0" u32 version (1) u32 receiverCount
//   per receiver: u64 binding.hi u64 binding.lo u32 part u32 atlas u64 geometryHash
//                 u32 vertexCount u32 indexCount
//                 vertexCount x (u32 sourceVertex f32 u f32 v)   indexCount x u32
bool writeLightingReceivers(std::span<const LightingReceiver> receivers, std::vector<uint8_t>& out);
bool readLightingReceivers(
    std::span<const uint8_t> bytes, std::vector<LightingReceiver>& receivers, std::string& error);

// ---- Probes payload (.swprobe) ----

// Directional distance per probe: an 8 x 8 octahedral map of the mean distance to the
// first hit and its square (metres, misses count as maxDistance), from the probe's rays.
inline constexpr uint32_t kProbeVisibilitySize = 8;
inline constexpr uint32_t kProbeVisibilityTexels = kProbeVisibilitySize * kProbeVisibilitySize;
// The largest grid one volume may bake (predicted and refused before baking).
inline constexpr uint32_t kMaxProbesPerVolume = 32768;
struct LightingProbe {
    bool valid = true;   // false: inside geometry, never used
    Sh9 sh{};   // irradiance / pi
    std::array<glm::vec2, kProbeVisibilityTexels> distance{};   // mean, mean of squares
};
// A regular grid filling a box: local -halfExtents..halfExtents, placed by toWorld (the
// volume's transform when baked), counts probes along each local axis (1 puts one probe in
// the middle). Probe (x, y, z) is probes[x + counts.x * (y + counts.y * z)].
struct LightingProbeVolume {
    BindingId binding;
    glm::mat4 toWorld{1.0f};
    glm::vec3 halfExtents{1.0f};
    glm::uvec3 counts{1u};
    float blendDistance = 0.5f;   // metres inside the box over which it fades out at the boundary
    int32_t priority = 0;   // higher wins where volumes overlap
    float maxDistance = 1.0f;   // the visibility map's miss distance
    std::vector<LightingProbe> probes;
    glm::vec3 probePosition(uint32_t x, uint32_t y, uint32_t z) const;
};
struct LightingProbeSet {
    Sh9 environment{};   // the scene's surroundings as irradiance / pi: the fallback outside volumes
    std::vector<LightingProbeVolume> volumes;
};
// Layout, little-endian, no padding:
//   "SWPROBE\0" u32 version (1) f32 environment[27] (coefficient-major, rgb) u32 volumeCount
//   per volume: u64 hi u64 lo f32 toWorld[16] (column-major) f32 halfExtents[3]
//               u32 counts[3] f32 blendDistance i32 priority f32 maxDistance
//               per probe: u32 valid f32 sh[27] f32 distance[128] (mean, square per texel)
bool writeLightingProbes(const LightingProbeSet& probes, std::vector<uint8_t>& out);
bool readLightingProbes(std::span<const uint8_t> bytes, LightingProbeSet& probes, std::string& error);

// Octahedral mapping of unit directions to 0..1 squares, for the visibility maps.
glm::vec2 octahedralEncode(const glm::vec3& direction);
glm::vec3 octahedralDecode(const glm::vec2& uv);

// Diffuse lighting from the probes at a point, as irradiance / pi coefficients. Per volume:
// the 8 surrounding probes weighted trilinearly, by validity, and by visibility (a
// Chebyshev bound from the distance moments, so a probe behind a wall drops out); weights
// normalised. If all of a volume's weights vanish, its nearest valid probe that sees the
// point stands in. Volumes blend by priority and fade over blendDistance at their faces;
// whatever coverage is left over goes to the environment. coverage reports how much of the
// result came from volumes: 0 means "outside every volume" and sh is the environment.
struct ProbeSample {
    Sh9 sh{};
    float coverage = 0;
    uint32_t volumesUsed = 0;
    bool fallback = false;   // a volume's weights all vanished and its nearest probe stood in
};
ProbeSample sampleProbes(const LightingProbeSet& probes, const glm::vec3& point);
// One probe's visibility weight for a point at distance d in direction dir from it.
float probeVisibility(const LightingProbe& probe, const glm::vec3& direction, float distance, float spacing);

// ---- Manifest (.swlighting) ----

// One generated file the manifest vouches for, by content hash.
struct LightingResource {
    enum class Kind { Lightmap, Receivers, Probes };
    Kind kind = Kind::Lightmap;
    std::string path;   // inside Content, forward slashes
    uint64_t hash = 0;   // hashLightingBytes of the whole file
    uint64_t bytes = 0;
    uint32_t width = 0, height = 0;   // lightmaps only
};
const char* lightingResourceKindName(LightingResource::Kind kind);   // "lightmap", "receivers", "probes"
// What a receiver or probe volume looked like when baked: its own dependencies (geometry,
// placement, material, density), so moving or editing it invalidates it alone.
struct LightingBinding {
    BindingId binding;
    uint32_t part = 0;
    uint32_t atlas = 0;
    uint64_t hash = 0;
};
struct LightingManifest {
    std::string scene;   // the scene file baked, inside Content
    uint32_t algorithm = 1;   // estimator and unwrap version; a different one is stale
    std::string quality = "preview";   // preview or final
    uint32_t samples = 64;   // per texel
    uint32_t bounces = 2;   // diffuse reflections a path may make
    uint32_t seed = 1;
    uint32_t atlasSize = 1024;
    uint32_t probeRays = 256;
    std::string colorConvention = "linear_rec709_irradiance_over_pi";
    // Everything the whole bake depends on: static lights, environment, settings.
    uint64_t lightingHash = 0;
    // Every static occluder's geometry, placement and material. A change leaves receivers
    // usable (their own hashes still match) but the bake out of date.
    uint64_t geometryHash = 0;
    std::string tool;   // what baked it: "shockwave lighting 1"
    std::vector<LightingResource> resources;
    std::vector<LightingBinding> receivers;
    std::vector<LightingBinding> volumes;   // part and atlas unused
    const LightingResource* resource(LightingResource::Kind kind, uint32_t index = 0) const;
};
// The text format, documented in lightingManifestSchema(). Strict: unknown or repeated keys,
// bad values and missing required fields are errors with file:line (file is only used for
// messages).
bool parseLightingManifest(
    std::string_view text, const std::string& file, LightingManifest& manifest, std::string& error);
std::string lightingManifestText(const LightingManifest& manifest);
bool loadLightingManifest(const std::filesystem::path& file, LightingManifest& manifest, std::string& error);
// docs/LightingSchema.md: the manifest grammar, payload layouts, components and conventions.
std::string lightingManifestSchema();

// ---- Inspect, export and import ----

// Readable JSON with every float written to round-trip exactly (9 significant digits),
// non-finite numbers refused. Export then import gives back the same bytes.
std::string receiversJson(std::span<const LightingReceiver> receivers);
bool receiversFromJson(std::string_view json, std::vector<LightingReceiver>& receivers, std::string& error);
std::string probesJson(const LightingProbeSet& probes);
bool probesFromJson(std::string_view json, LightingProbeSet& probes, std::string& error);
std::string lightmapJson(uint32_t width, uint32_t height, std::span<const float> rgba);
bool lightmapFromJson(
    std::string_view json, uint32_t& width, uint32_t& height, std::vector<float>& rgba, std::string& error);

bool readLightingFile(const std::filesystem::path& file, std::vector<uint8_t>& bytes, std::string& error);
// Writes to a temporary sibling, then renames over file: readers see the old file or the
// whole new one.
bool writeLightingFileAtomic(const std::filesystem::path& file, std::span<const uint8_t> bytes, std::string& error);

}   // namespace shockwave
