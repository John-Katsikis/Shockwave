#pragma once

#include <Assets/Material.h>
#include <Assets/Model.h>
#include <Assets/AudioClip.h>
#include <Renderer/Mesh.h>
#include <Renderer/Renderer.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

// Names for GPU resources, so scene files and the editor can refer to "cube" or
// "checker" instead of raw handles. Models use stable slots: replacing their
// immutable data updates every scene reference on the next frame.
//
// Thread-safe: register and look up from any thread.
class Assets {
public:
    void addAudioClip(std::string name, std::shared_ptr<const AudioClip> clip);
    std::shared_ptr<const AudioClip> audioClip(std::string_view name) const;
    std::vector<std::string> audioClips() const;

    Assets();   // "white" (the built-in texture) is always there

    // data: the mesh's vertices kept on the CPU, for picking and bounds. Optional.
    void addMesh(std::string name, MeshHandle mesh, std::shared_ptr<const MeshData> data = nullptr);
    void addTexture(std::string name, TextureHandle texture);

    ModelHandle addModel(std::string name, std::shared_ptr<const ModelAsset> model);
    ModelHandle findModel(std::string_view name) const;
    std::shared_ptr<const ModelAsset> model(ModelHandle handle) const;
    std::string modelName(ModelHandle handle) const;
    std::vector<std::pair<std::string, ModelHandle>> models() const;

    // Materials (.swmaterial, A02) keep their slot when replaced, like models: a reload
    // reaches every MeshRenderer and ModelRenderer using it on the next frame.
    MaterialHandle addMaterial(std::string name, std::shared_ptr<const MaterialAsset> material);
    MaterialHandle findMaterial(std::string_view name) const;
    std::shared_ptr<const MaterialAsset> material(MaterialHandle handle) const;
    std::string materialName(MaterialHandle handle) const;
    std::vector<std::pair<std::string, MaterialHandle>> materials() const;

    MeshHandle findMesh(std::string_view name) const;   // invalid if unknown
    std::optional<TextureHandle> findTexture(std::string_view name) const;
    std::string meshName(MeshHandle mesh) const;   // empty if unnamed
    std::shared_ptr<const MeshData> meshData(MeshHandle mesh) const;   // null if not kept
    std::string textureName(TextureHandle texture) const;

    std::vector<std::pair<std::string, MeshHandle>> meshes() const;
    std::vector<std::pair<std::string, TextureHandle>> textures() const;

    // The folder asset names are relative to (a project's Content/), for assets other
    // modules read from disk themselves, like particle effects. AssetFolder::open sets it.
    void setContentRoot(std::filesystem::path root);
    std::filesystem::path contentRoot() const;

private:
    std::filesystem::path m_contentRoot;
    mutable std::shared_mutex m_mutex;
    std::vector<std::pair<std::string, std::shared_ptr<const AudioClip>>> m_audio;
    std::vector<std::pair<std::string, std::shared_ptr<const ModelAsset>>> m_models;
    std::vector<std::pair<std::string, std::shared_ptr<const MaterialAsset>>> m_materials;
    std::vector<std::pair<std::string, MeshHandle>> m_meshes;
    std::vector<std::shared_ptr<const MeshData>> m_meshData;   // parallel to m_meshes
    std::vector<std::pair<std::string, TextureHandle>> m_textures;
};

}   // namespace shockwave
