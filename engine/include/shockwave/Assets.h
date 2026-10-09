#pragma once

#include <shockwave/Renderer.h>

#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

// Names for GPU resources, so scene files and the editor can refer to "cube" or
// "checker" instead of raw handles. The seed of the asset system: file import and
// hot reload will register here too.
//
// Thread-safe: register and look up from any thread.
class Assets {
public:
    Assets();   // "white" (the built-in texture) is always there

    void addMesh(std::string name, MeshHandle mesh);
    void addTexture(std::string name, TextureHandle texture);

    MeshHandle findMesh(std::string_view name) const;   // invalid if unknown
    std::optional<TextureHandle> findTexture(std::string_view name) const;
    std::string meshName(MeshHandle mesh) const;          // empty if unnamed
    std::string textureName(TextureHandle texture) const;

    std::vector<std::pair<std::string, MeshHandle>> meshes() const;
    std::vector<std::pair<std::string, TextureHandle>> textures() const;

private:
    mutable std::shared_mutex m_mutex;
    std::vector<std::pair<std::string, MeshHandle>> m_meshes;
    std::vector<std::pair<std::string, TextureHandle>> m_textures;
};

} // namespace shockwave
