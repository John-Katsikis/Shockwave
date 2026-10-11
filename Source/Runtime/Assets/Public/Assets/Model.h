#pragma once

#include <Assets/Image.h>
#include <Renderer/Mesh.h>
#include <Renderer/Renderer.h>

#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {
struct ModelHandle {
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
};
// Bounds comes from Mesh.h (through Renderer.h).
struct ModelTexture {
    std::string source;   // external path or embedded texture identifier
    Image image;
};
struct ModelMaterial {
    std::string name;
    glm::vec4 baseColor{1.0f};
    glm::vec3 emissive{0.0f};
    float metallic = 0.0f, roughness = 1.0f;
    int baseColorTexture = -1, normalTexture = -1, metallicRoughnessTexture = -1;
    int emissiveTexture = -1, occlusionTexture = -1;
    enum class Alpha { Opaque, Mask, Blend };
    Alpha alpha = Alpha::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};
// A joint that bends a skinned mesh: a node of the model, and the matrix that takes the
// mesh as modelled into that node's space (its inverse bind matrix).
struct ModelBone {
    uint32_t node = 0;
    glm::mat4 offset{1.0f};
};
struct ModelMesh {
    std::string name;
    MeshData data;
    Bounds bounds;
    uint32_t material = 0;
    // Skinned meshes only: one VertexSkin per vertex, whose joints index bones. Skinned
    // vertices end up in the model's own space, so the mesh's node doesn't move them.
    std::vector<VertexSkin> skin;
    std::vector<ModelBone> bones;
    bool skinned() const { return !bones.empty(); }
};
// One node's keyframes in an animation clip. Times are seconds from the clip's start.
struct ModelVectorKey {
    float time = 0.0f;
    glm::vec3 value{0.0f};
};
struct ModelRotationKey {
    float time = 0.0f;
    glm::quat value{1.0f, 0.0f, 0.0f, 0.0f};
};
struct ModelChannel {
    uint32_t node = 0;
    std::vector<ModelVectorKey> positions;
    std::vector<ModelRotationKey> rotations;
    std::vector<ModelVectorKey> scales;
};
// An animation clip (a walk, a jump): keyframes for some of the model's nodes. Nodes it
// has no keys for stay where the model puts them.
struct ModelAnimation {
    std::string name;
    float duration = 0.0f;   // seconds
    std::vector<ModelChannel> channels;
};
struct ModelNode {
    std::string name;
    int parent = -1;   // nodes are in parent-first order
    glm::mat4 local{1.0f}, world{1.0f};
    std::vector<uint32_t> meshes;
};
// Owns its data independently of Assimp. Import is CPU-only and worker-safe.
// Node matrices preserve hierarchy/instances without decomposing away shear.
struct ModelData {
    std::vector<ModelMesh> meshes;
    std::vector<ModelNode> nodes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    std::vector<ModelAnimation> animations;   // clips, in file order; Animation module plays them
    Bounds bounds;
    std::vector<std::filesystem::path> dependencies;
    std::vector<std::string> warnings;
    uint64_t triangles = 0;
};
// The clip called name, or null. Names are case-sensitive, as in the file.
const ModelAnimation* findAnimation(const ModelData& model, std::string_view name);
bool hasSkinnedMeshes(const ModelData& model);

struct ModelAsset {
    ModelData data;
    std::vector<MeshHandle> meshes;
    std::vector<TextureHandle> textures;
};

bool isModelFile(const std::filesystem::path& path);
std::string modelExtensions();   // formats enabled in this build
// Destination changes only on success. Failed imports still return dependencies
// in attemptedDependencies so saving a missing .bin/.mtl/texture retries the model.
// .blend: Blender on PATH (or SHOCKWAVE_BLENDER) converts modern files to GLB;
// without it, the deprecated native reader is tried with an explicit diagnostic.
bool loadModel(
    const std::filesystem::path& path, ModelData& model, std::string& error,
    std::vector<std::filesystem::path>* attemptedDependencies = nullptr);
// Ray is in model-local space. Tests real triangles, respecting node transforms.
bool intersectModel(const ModelData& model, const glm::vec3& origin, const glm::vec3& direction, float& distance);
}   // namespace shockwave
