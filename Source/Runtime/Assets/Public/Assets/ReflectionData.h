#pragma once

// Captured reflection probes (G07; docs/Lighting.md "Reflection probes", generated
// docs/LightingSchema.md): the cube payload (linear RGBA16F .ktx2, six faces, every
// roughness level), the CPU prefilter that turns a captured cube into those levels the way
// the renderer filters the global environment, the capture camera for each face, and the
// .swreflection manifest that binds cubes to a scene's ReflectionProbe components. CPU only:
// inspecting and checking probes needs no GPU; capturing them does.

#include <Assets/LightingData.h>
#include <Renderer/ReflectionProbes.h>
#include <Renderer/TextureData.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

// The resolutions a probe may capture at, per face.
inline constexpr uint32_t kReflectionResolutions[] = {64, 128, 256, 512};
bool validReflectionResolution(uint32_t size);
// GPU bytes of one cube at size with kReflectionLevels levels (RGBA16F): 128 is about 1 MiB.
uint64_t reflectionCubeBytes(uint32_t size);

// ---- The cube ----

// level[l] holds the six faces of level l one after another (+X, -X, +Y, -Y, +Z, -Z), each
// (size >> l) squared texels of RGBA, rows top to bottom; linear radiance before exposure.
// Level l is roughness l / (levels - 1), level 0 the mirror reflection.
struct ReflectionCube {
    uint32_t size = 0;
    std::vector<std::vector<float>> levels;
    uint32_t levelSize(uint32_t l) const { return std::max(1u, size >> l); }
    bool valid() const;   // a resolution kReflectionResolutions allows, kReflectionLevels whole levels, all finite
};
// The cube as the renderer takes it: TextureShape::Cube, RGBA16F, linear.
TextureData reflectionTextureData(const ReflectionCube& cube);
// Bilinear within a face at a level (no filtering across edges), the way the tests and the
// prefilter read a cube; lod is clamped to the levels there are and blends two.
glm::vec3 sampleReflectionCube(const ReflectionCube& cube, const glm::vec3& direction, float lod);
// Each face's mean colour at a level, +X first: what the orientation tests compare.
std::array<glm::vec3, 6> reflectionFaceMeans(const ReflectionCube& cube, uint32_t level = 0);

// Uncompressed .ktx2: VK_FORMAT_R16G16B16A16_SFLOAT, faces 6, levels kReflectionLevels,
// no supercompression, a basic linear RGBA data format descriptor; level data from the
// smallest level to the largest, as the KTX2 specification lays it out. Values round to
// half floats.
bool encodeReflectionKtx2(const ReflectionCube& cube, std::vector<uint8_t>& ktx2, std::string& error);
// Only that shape: a 2D file, a wrong face or level count, a truncated level or an
// overflowing size is an error.
bool decodeReflectionKtx2(std::span<const uint8_t> ktx2, ReflectionCube& cube, std::string& error);
// Whether a .ktx2 says it's a cube (six faces), so a tool can tell a probe's cube from a lightmap.
bool isReflectionKtx2(std::span<const uint8_t> ktx2);

// ---- Capture ----

// The camera that captures face (0..5) from position: a 90 degree square view along the
// face's axis, reverse-Z with the probe's near and far planes, ready for Renderer::setCamera.
void reflectionFaceCamera(
    uint32_t face, const glm::vec3& position, float nearPlane, float farPlane, glm::mat4& view, glm::mat4& projection);
// One face of level 0 from a SceneCapture taken through reflectionFaceCamera: each cube texel
// looks along its own direction, projected into the captured image and box-filtered there,
// so the result is right whatever the capture's size and aspect. Appends size squared RGBA
// texels to out. False (and why) when the capture is empty or the camera doesn't see the face.
bool resampleReflectionFace(
    const SceneCapture& capture, uint32_t face, const glm::mat4& viewProjection, uint32_t size, std::vector<float>& out,
    std::string& error);

// ---- Prefilter ----

// Every level from level 0 (the captured mirror image, which stays as it is): level l is the
// cube seen through a GGX lobe of roughness l / (kReflectionLevels - 1), importance sampled
// from a box-filtered chain of level 0 with the source level picked by each sample's
// density: the same estimator, samples and roughness mapping as the renderer's global
// specular cube (Shaders/environment_filter.comp), so local and global reflections blur
// alike. Deterministic: the same input gives the same bits.
inline constexpr uint32_t kReflectionFilterSamples = 96;
void prefilterReflectionCube(ReflectionCube& cube);

// ---- Manifest (.swreflection) ----

// One captured probe: its cube file and what it was captured as. Moving or editing the
// probe invalidates it alone (hash); anything static in the scene changing makes every
// probe stale but still usable (the manifest's scene_hash).
struct ReflectionProbeRecord {
    BindingId binding;
    std::string path;   // the .ktx2, inside Content, forward slashes
    uint64_t hash = 0;   // hashLightingBytes of the file
    uint64_t bytes = 0;
    uint32_t size = 128;   // per face
    uint64_t probeHash = 0;   // its own placement and settings when captured
};
struct ReflectionManifest {
    std::string scene;   // the scene captured, inside Content
    uint32_t algorithm = 1;   // capture and filter version; another one is stale
    std::string colorConvention = "linear_rec709_radiance";
    // Everything static the captures saw: the scene's geometry, lights, surroundings and its
    // lighting bake. A change leaves each probe usable but out of date.
    uint64_t sceneHash = 0;
    std::string tool;   // what captured it: "shockwave reflections 1"
    std::vector<ReflectionProbeRecord> probes;
    const ReflectionProbeRecord* probe(const BindingId& binding) const;
};
// Strict, like .swlighting: unknown or repeated keys, bad values and missing fields are
// errors with file:line.
bool parseReflectionManifest(
    std::string_view text, const std::string& file, ReflectionManifest& manifest, std::string& error);
std::string reflectionManifestText(const ReflectionManifest& manifest);
bool loadReflectionManifest(const std::filesystem::path& file, ReflectionManifest& manifest, std::string& error);
// The reflection part of docs/LightingSchema.md.
std::string reflectionManifestSchema();

// ---- Inspect, export and import ----

// Readable JSON: size, every level's floats (9 significant digits, so import gives back the
// same .ktx2 bytes) and each level-0 face's mean for a quick look at orientation.
std::string reflectionCubeJson(const ReflectionCube& cube);
bool reflectionCubeFromJson(std::string_view json, ReflectionCube& cube, std::string& error);

}   // namespace shockwave
