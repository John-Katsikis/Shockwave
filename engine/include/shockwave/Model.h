#pragma once

#include <shockwave/Image.h>
#include <shockwave/Mesh.h>
#include <shockwave/Renderer.h>
#include <filesystem>
#include <string>
#include <vector>

namespace shockwave {
struct ModelHandle {
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
};
// Bounds comes from Mesh.h (through Renderer.h).
struct ModelTexture {
    std::string source; // external path or embedded texture identifier
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
struct ModelMesh {
    std::string name;
    MeshData data;
    Bounds bounds;
    uint32_t material = 0;
};
struct ModelNode {
    std::string name;
    int parent = -1; // nodes are in parent-first order
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
    Bounds bounds;
    std::vector<std::filesystem::path> dependencies;
    std::vector<std::string> warnings;
    uint64_t triangles = 0;
};
struct ModelAsset {
    ModelData data;
    std::vector<MeshHandle> meshes;
    std::vector<TextureHandle> textures;
};

bool isModelFile(const std::filesystem::path& path);
std::string modelExtensions(); // formats enabled in this build
// Destination changes only on success. Failed imports still return dependencies
// in attemptedDependencies so saving a missing .bin/.mtl/texture retries the model.
// .blend: Blender on PATH (or SHOCKWAVE_BLENDER) converts modern files to GLB;
// without it, the deprecated native reader is tried with an explicit diagnostic.
bool loadModel(const std::filesystem::path& path, ModelData& model, std::string& error,
               std::vector<std::filesystem::path>* attemptedDependencies = nullptr);
// Ray is in model-local space. Tests real triangles, respecting node transforms.
bool intersectModel(const ModelData& model, const glm::vec3& origin, const glm::vec3& direction, float& distance);
} // namespace shockwave
