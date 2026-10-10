#include <Assets/Assets.h>

#include <mutex>

namespace shockwave {

Assets::Assets() { m_textures.emplace_back("white", TextureHandle{}); }

void Assets::addMesh(std::string name, MeshHandle mesh, std::shared_ptr<const MeshData> data) {
    std::unique_lock lock(m_mutex);
    for (size_t i = 0; i < m_meshes.size(); ++i) {
        if (m_meshes[i].first == name) {
            m_meshes[i].second = mesh;
            m_meshData[i] = std::move(data);
            return;
        }
    }
    m_meshes.emplace_back(std::move(name), mesh);
    m_meshData.push_back(std::move(data));
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

ModelHandle Assets::addModel(std::string name, std::shared_ptr<const ModelAsset> model) {
    std::unique_lock lock(m_mutex);
    for (uint32_t i = 0; i < m_models.size(); ++i) {
        if (m_models[i].first == name) {
            m_models[i].second = std::move(model);
            return {i};
        }
    }
    m_models.emplace_back(std::move(name), std::move(model));
    return {static_cast<uint32_t>(m_models.size() - 1)};
}
ModelHandle Assets::findModel(std::string_view name) const {
    std::shared_lock lock(m_mutex);
    for (uint32_t i = 0; i < m_models.size(); ++i)
        if (m_models[i].first == name) return {i};
    return {};
}
std::shared_ptr<const ModelAsset> Assets::model(ModelHandle handle) const {
    std::shared_lock lock(m_mutex);
    return handle.id < m_models.size() ? m_models[handle.id].second : nullptr;
}
std::string Assets::modelName(ModelHandle handle) const {
    std::shared_lock lock(m_mutex);
    return handle.id < m_models.size() ? m_models[handle.id].first : std::string{};
}
std::vector<std::pair<std::string, ModelHandle>> Assets::models() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::pair<std::string, ModelHandle>> result;
    for (uint32_t i = 0; i < m_models.size(); ++i) result.emplace_back(m_models[i].first, ModelHandle{i});
    return result;
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

std::shared_ptr<const MeshData> Assets::meshData(MeshHandle mesh) const {
    std::shared_lock lock(m_mutex);
    for (size_t i = 0; i < m_meshes.size(); ++i) {
        if (m_meshes[i].second.id == mesh.id) return m_meshData[i];
    }
    return nullptr;
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

void Assets::addAudioClip(std::string name, std::shared_ptr<const AudioClip> clip) {
    std::unique_lock lock(m_mutex);
    for (auto& [key, value] : m_audio)
        if (key == name) {
            value = std::move(clip);
            return;
        }
    m_audio.emplace_back(std::move(name), std::move(clip));
}
std::shared_ptr<const AudioClip> Assets::audioClip(std::string_view name) const {
    std::shared_lock lock(m_mutex);
    for (const auto& [key, value] : m_audio)
        if (key == name) return value;
    return nullptr;
}
std::vector<std::string> Assets::audioClips() const {
    std::shared_lock lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [name, value] : m_audio)
        if (value) result.push_back(name);
    return result;
}

}   // namespace shockwave
