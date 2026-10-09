#include <shockwave/Assets.h>

#include <mutex>

namespace shockwave {

Assets::Assets() {
    m_textures.emplace_back("white", TextureHandle{});
}

void Assets::addMesh(std::string name, MeshHandle mesh) {
    std::unique_lock lock(m_mutex);
    for (auto& [n, h] : m_meshes) {
        if (n == name) {
            h = mesh;
            return;
        }
    }
    m_meshes.emplace_back(std::move(name), mesh);
}

void Assets::addTexture(std::string name, TextureHandle texture) {
    std::unique_lock lock(m_mutex);
    for (auto& [n, h] : m_textures) {
        if (n == name) {
            h = texture;
            return;
        }
    }
    m_textures.emplace_back(std::move(name), texture);
}

MeshHandle Assets::findMesh(std::string_view name) const {
    std::shared_lock lock(m_mutex);
    for (const auto& [n, h] : m_meshes) {
        if (n == name) return h;
    }
    return {};
}

std::optional<TextureHandle> Assets::findTexture(std::string_view name) const {
    std::shared_lock lock(m_mutex);
    for (const auto& [n, h] : m_textures) {
        if (n == name) return h;
    }
    return std::nullopt;
}

std::string Assets::meshName(MeshHandle mesh) const {
    std::shared_lock lock(m_mutex);
    for (const auto& [n, h] : m_meshes) {
        if (h.id == mesh.id) return n;
    }
    return {};
}

std::string Assets::textureName(TextureHandle texture) const {
    std::shared_lock lock(m_mutex);
    for (const auto& [n, h] : m_textures) {
        if (h.id == texture.id) return n;
    }
    return {};
}

std::vector<std::pair<std::string, MeshHandle>> Assets::meshes() const {
    std::shared_lock lock(m_mutex);
    return m_meshes;
}

std::vector<std::pair<std::string, TextureHandle>> Assets::textures() const {
    std::shared_lock lock(m_mutex);
    return m_textures;
}

} // namespace shockwave
