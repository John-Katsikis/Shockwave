#include <Assets/AssetFolder.h>

#include <Assets/Image.h>
#include <Assets/TextureCompression.h>
#include <Core/Jobs.h>
#include <Renderer/Renderer.h>

#include "TextureCompressor.h"

#include <algorithm>
#include <atomic>
#include <exception>

namespace shockwave {

namespace fs = std::filesystem;
namespace {
AssetFolder::Dependency stamp(const fs::path& path) {
    AssetFolder::Dependency d;
    d.path = path;
    std::error_code ec;
    d.exists = fs::is_regular_file(path, ec);
    if (d.exists) {
        d.bytes = fs::file_size(path, ec);
        d.modified = fs::last_write_time(path, ec);
    }
    return d;
}
bool changed(const AssetFolder::Dependency& old) {
    const auto now = stamp(old.path);
    return old.exists != now.exists || old.bytes != now.bytes || old.modified != now.modified;
}
void release(Renderer& renderer, const ModelAsset& model) {
    for (auto handle : model.meshes) renderer.destroyMesh(handle);
    for (auto handle : model.textures) renderer.destroyTexture(handle);
}
}   // namespace

struct AssetFolder::AudioImport {
    File file;
    std::shared_ptr<AudioClip> clip = std::make_shared<AudioClip>();
    std::string error;
    std::atomic<bool> complete{false};
};

AssetFolder::AssetFolder(Renderer& renderer, Assets& assets, JobSystem& jobs)
    : m_renderer(&renderer), m_assets(assets), m_jobs(jobs) {}

AssetFolder::AssetFolder(Assets& assets, JobSystem& jobs) : m_assets(assets), m_jobs(jobs) {}
AssetFolder::~AssetFolder() { m_jobs.wait(m_audioJobs); }

// ---- Optimisation (docs/Optimisation.md) ----

namespace {
// RGBA8 with a full mip chain: what an uncompressed texture takes on the GPU.
uint64_t rgbaBytes(uint32_t width, uint32_t height) {
    return TextureData{TextureFormat::RGBA8, true, width, height, {{width, height, 0, 0}}, {}}.gpuBytes();
}
}   // namespace

void AssetFolder::setImportDefaults(const ImportSettings& defaults) {
    if (defaults == m_defaults) return;
    m_defaults = defaults;
    m_reimportAll = true;
}

size_t AssetFolder::compressing() const { return m_compressor ? m_compressor->pending() : 0; }

std::vector<AssetFolder::Change> AssetFolder::finishCompressing() {
    if (m_compressor) m_compressor->finish(m_jobs);
    return refresh();
}

AssetFolder::File* AssetFolder::findFile(std::string_view name) {
    for (File& f : m_files)
        if (f.name == name) return &f;
    return nullptr;
}

// Swaps finished background compression into the textures it was for, if they're still
// the version it was made from.
void AssetFolder::publishCompressed(std::vector<Change>& changes) {
    if (!m_compressor || !m_renderer) return;
    for (const auto& task : m_compressor->takeFinished()) {
        File* f = findFile(task->asset);
        if (!f || f->modified != task->modified || f->bytes != task->bytes) continue;
        f->compressing -= std::min(f->compressing, 1u);
        if (!task->texture.valid()) {
            f->warnings.push_back("Texture compression failed: " + task->error);
            continue;
        }
        TextureHandle target = f->texture;
        if (task->modelTexture >= 0) {
            const auto model = m_assets.model(f->model);
            if (!model || model.get() != task->owner.get()) continue;
            target = model->textures[static_cast<size_t>(task->modelTexture)];
        }
        try {
            m_renderer->updateTexture(target, task->texture);
        } catch (const std::exception& e) {
            f->warnings.push_back(std::string("Compressed texture couldn't be used: ") + e.what());
            continue;
        }
        if (!task->error.empty()) f->warnings.push_back(task->error);
        const uint64_t raw = rgbaBytes(task->texture.width, task->texture.height);
        f->gpuBytes = (f->gpuBytes > raw ? f->gpuBytes - raw : 0) + task->texture.gpuBytes();
        f->compressedTextures += task->texture.format == TextureFormat::BC7;
        f->compressedFileBytes += task->compressedBytes;
        changes.push_back({Change::What::Compressed, f->name, {}});
    }
}

std::vector<AssetFolder::Change> AssetFolder::open(fs::path root) {
    m_jobs.wait(m_audioJobs);
    m_audioImports.clear();
    if (m_compressor) m_compressor->clear();
    for (const auto& old : m_files)
        if (old.kind == Kind::Audio) m_assets.addAudioClip(old.name, nullptr);
    m_root = std::move(root);
    m_assets.setContentRoot(m_root);
    m_files.clear();
    m_pollTimer = 0.0;
    m_initialImport = true;
    try {
        auto changes = refresh();
        m_initialImport = false;
        return changes;
    } catch (...) {
        m_initialImport = false;
        throw;
    }
}

std::vector<AssetFolder::Change> AssetFolder::poll(double frameDt, double interval) {
    m_pollTimer += frameDt;
    if (m_pollTimer < interval) return {};
    m_pollTimer = 0.0;
    return refresh();
}

const AssetFolder::File* AssetFolder::find(std::string_view name) const {
    for (const File& f : m_files) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

std::vector<AssetFolder::Change> AssetFolder::refresh(bool force) {
    std::vector<Change> changes;
    if (m_root.empty()) return changes;

    // Publish completed background audio loads between frames. Failed/stale results
    // leave the last-good clip intact; a newer save will schedule another decode.
    for (auto it = m_audioImports.begin(); it != m_audioImports.end();) {
        const auto& job = **it;
        if (!job.complete.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        for (auto& file : m_files) {
            if (file.name != job.file.name || file.modified != job.file.modified || file.bytes != job.file.bytes)
                continue;
            file.error = job.error;
            if (job.error.empty()) {
                m_assets.addAudioClip(file.name, job.clip);
                changes.push_back({file.imported ? Change::What::Reloaded : Change::What::Added, file.name, {}});
                file.imported = true;
            } else
                changes.push_back({Change::What::Failed, file.name, job.error});
            break;
        }
        it = m_audioImports.erase(it);
    }
    publishCompressed(changes);
    force = force || std::exchange(m_reimportAll, false);

    // What's on disk now. Hidden files and folders (editor swap files, .git) are skipped.
    std::vector<File> found;
    std::error_code ec;
    if (fs::is_directory(m_root, ec)) {
        for (auto it = fs::recursive_directory_iterator(m_root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string leaf = it->path().filename().string();
            if (!leaf.empty() && leaf[0] == '.') {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            // A .swimport file is part of its asset; one whose asset is gone is listed as failed.
            fs::path asset = it->path();
            const bool orphan = isImportSettingsFile(asset) && !fs::exists(asset.replace_extension(), ec);
            if (isImportSettingsFile(it->path()) && !orphan) continue;
            File f;
            if (orphan)
                f.error = "import settings for " + asset.filename().string() +
                          ", which doesn't exist; rename or delete this file";
            f.path = it->path();
            f.name = fs::relative(f.path, m_root, ec).generic_string();
            f.kind = isImageFile(f.path) || isKtx2File(f.path) ? Kind::Texture
                     : isModelFile(f.path) ? Kind::Model
                     : isAudioFile(f.path) ? Kind::Audio
                                           : Kind::Other;
            f.bytes = it->file_size(ec);
            f.modified = it->last_write_time(ec);
            found.push_back(std::move(f));
        }
    }
    std::sort(found.begin(), found.end(), [](const File& a, const File& b) { return a.name < b.name; });

    // Carry over what's already imported, and list what needs (re)importing.
    struct Import {
        size_t file = 0;
        bool reload = false;
        AudioClip audio;
        Image image;
        ModelData model;
        std::vector<fs::path> dependencies;
        std::string error;
        // Optimisation: settings, textures found ready in the cache (or a .ktx2 asset),
        // and what optimising the model's meshes did.
        ImportSettings settings;
        TextureData texture;
        std::vector<TextureData> modelTextures;
        uint64_t cacheKey = 0;   // a texture file's, from its bytes
        uint64_t compressedFileBytes = 0;
        MeshOptimiseReport meshes;
    };
    std::vector<Import> imports;
    for (size_t i = 0; i < found.size(); ++i) {
        File& f = found[i];
        const File* old = find(f.name);
        if (old) {
            const bool needsImport = force || old->modified != f.modified || old->bytes != f.bytes ||
                                     std::any_of(old->dependencies.begin(), old->dependencies.end(), changed);
            f.imported = old->imported;
            f.error = old->error;
            f.texture = old->texture;
            f.width = old->width;
            f.height = old->height;
            f.model = old->model;
            f.dependencies = old->dependencies;
            f.warnings = old->warnings;
            f.settings = old->settings;
            f.textures = old->textures;
            f.compressedTextures = old->compressedTextures;
            f.compressing = old->compressing;
            f.gpuBytes = old->gpuBytes;
            f.uncompressedBytes = old->uncompressedBytes;
            f.compressedFileBytes = old->compressedFileBytes;
            f.meshes = old->meshes;
            if (!needsImport) continue;
        }
        if (f.kind == Kind::Other) {
            if (!old) changes.push_back({f.error.empty() ? Change::What::Added : Change::What::Failed, f.name, f.error});
            continue;
        }
        if (f.kind == Kind::Audio && !m_initialImport) {
            auto job = std::make_shared<AudioImport>();
            job->file = f;
            m_audioImports.push_back(job);
            m_jobs.run(
                [job] {
                    try {
                        loadAudioClip(job->file.path, *job->clip, job->error);
                    } catch (const std::exception& e) {
                        job->error = e.what();
                    } catch (...) {
                        job->error = "audio import failed";
                    }
                    job->complete.store(true, std::memory_order_release);
                },
                &m_audioJobs);
            continue;
        }
        imports.emplace_back();
        imports.back().file = i;
        imports.back().reload = f.imported;
    }
    for (const File& old : m_files) {
        const bool gone = std::none_of(found.begin(), found.end(), [&](const File& f) { return f.name == old.name; });
        // Its texture stays registered, so scenes using it keep their last good copy.
        if (gone) {
            if (old.kind == Kind::Audio) m_assets.addAudioClip(old.name, nullptr);
            changes.push_back({Change::What::Removed, old.name, {}});
        }
    }

    // Optimisation (docs/Optimisation.md). A compressed copy in the cache is used straight
    // away; anything else is drawn uncompressed while the background compressor works.
    const bool bc7 = m_renderer && m_renderer->supportsTextureFormat(TextureFormat::BC7);
    auto cached = [&](std::string_view asset, uint64_t key, TextureData& texture, uint64_t& fileBytes) {
        if (m_cacheFolder.empty()) return false;
        std::vector<uint8_t> ktx2;
        std::string ignored;
        const fs::path file = textureCachePath(m_cacheFolder, asset, key);
        std::error_code ec;
        if (!fs::is_regular_file(file, ec) || !readFileBytes(file, ktx2, ignored) ||
            !transcodeKtx2(ktx2, bc7, texture, ignored))
            return false;
        fileBytes += ktx2.size();
        return true;
    };
    auto importTexture = [&](const File& f, Import& job) {
        std::vector<uint8_t> bytes;
        if (!readFileBytes(f.path, bytes, job.error)) return;
        if (isKtx2File(f.path)) {
            if (transcodeKtx2(bytes, bc7, job.texture, job.error)) job.compressedFileBytes = bytes.size();
            return;
        }
        if (job.settings.compression != TextureCompression::Off) {
            job.cacheKey = textureCacheKey(bytes, job.settings.compression);
            if (cached(f.name, job.cacheKey, job.texture, job.compressedFileBytes)) return;
        }
        decodeImage(bytes, job.image, job.error);
    };
    auto optimiseImportedModel = [&](const File& f, Import& job) {
        MeshOptimiseSettings meshSettings;
        meshSettings.lods = job.settings.lods;
        job.meshes = optimiseModel(job.model, meshSettings);
        job.modelTextures.resize(job.model.textures.size());
        if (job.settings.compression == TextureCompression::Off) return;
        for (const ModelMaterial& material : job.model.materials) {
            const int index = material.baseColorTexture;   // the only maps the renderer samples
            if (index < 0 || job.modelTextures[index].valid()) continue;
            const uint64_t key = textureCacheKey(job.model.textures[index].image.rgba, job.settings.compression);
            cached(f.name + "#" + std::to_string(index), key, job.modelTextures[index], job.compressedFileBytes);
        }
    };

    // CPU imports run in parallel with one Assimp instance per job. Publication and
    // GPU resource retirement run between frames on the main thread.
    m_jobs.parallelFor(imports.size(), 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            auto& job = imports[i];
            const auto& f = found[job.file];
            job.settings = m_defaults;
            if (f.kind != Kind::Audio &&
                !loadImportSettings(importSettingsPath(f.path), job.settings, job.error))
                continue;
            if (f.kind == Kind::Audio)
                loadAudioClip(f.path, job.audio, job.error);
            else if (f.kind == Kind::Model)
                loadModel(f.path, job.model, job.error, &job.dependencies);
            else
                importTexture(f, job);
            if (f.kind == Kind::Model && job.error.empty()) optimiseImportedModel(f, job);
        }
    });

    // After a successful import: what its textures take on the GPU now, and which are
    // left for the background compressor.
    auto recordOptimisation = [&](File& f, Import& job) {
        if (f.kind != Kind::Texture && f.kind != Kind::Model) return;
        f.textures = f.compressedTextures = f.compressing = 0;
        f.gpuBytes = f.uncompressedBytes = 0;
        f.compressedFileBytes = job.compressedFileBytes;
        f.meshes = job.meshes;
        const TextureCompression mode = job.settings.compression;
        auto account = [&](const TextureData& ready, uint32_t width, uint32_t height) {
            ++f.textures;
            f.uncompressedBytes += rgbaBytes(width, height);
            f.gpuBytes += ready.valid() ? ready.gpuBytes() : rgbaBytes(width, height);
            f.compressedTextures += ready.valid() && ready.format == TextureFormat::BC7;
        };
        auto compress = [&](Image image, int modelTexture, std::shared_ptr<const void> owner,
                            const std::string& cacheName, uint64_t key) {
            if (!m_renderer || !m_backgroundCompression || mode == TextureCompression::Off || !image.width) return;
            auto task = std::make_shared<TextureCompressor::Task>();
            task->asset = f.name;
            task->modified = f.modified;
            task->bytes = f.bytes;
            task->modelTexture = modelTexture;
            task->owner = std::move(owner);
            task->image = std::move(image);
            task->mode = mode;
            task->bc7 = bc7;
            if (!m_cacheFolder.empty()) task->cacheFile = textureCachePath(m_cacheFolder, cacheName, key);
            if (!m_compressor) m_compressor = std::make_unique<TextureCompressor>();
            m_compressor->add(std::move(task));
            ++f.compressing;
        };
        if (f.kind == Kind::Texture) {
            account(job.texture, f.width, f.height);
            if (!job.texture.valid()) compress(std::move(job.image), -1, nullptr, f.name, job.cacheKey);
            return;
        }
        const auto model = m_assets.model(f.model);
        if (!model) return;
        for (size_t i = 0; i < model->textures.size(); ++i) {
            if (model->textures[i].id == 0) continue;   // not sampled, so never uploaded
            const Image& image = model->data.textures[i].image;
            const TextureData& ready = job.modelTextures[i];
            account(ready, image.width, image.height);
            if (!ready.valid())
                compress(image, static_cast<int>(i), model, f.name + "#" + std::to_string(i),
                         textureCacheKey(image.rgba, mode));
        }
    };

    for (Import& job : imports) {
        File& f = found[job.file];
        f.dependencies.clear();
        for (const auto& path : job.dependencies) f.dependencies.push_back(stamp(path));
        if (f.kind != Kind::Audio) f.dependencies.push_back(stamp(importSettingsPath(f.path)));
        f.settings = job.settings;
        if (job.error.empty()) {
            if (f.kind == Kind::Audio) {
                m_assets.addAudioClip(f.name, std::make_shared<const AudioClip>(std::move(job.audio)));
            } else if (!m_renderer) {
                job.error = "this audio-only asset folder cannot publish GPU assets";
            } else if (f.kind == Kind::Model) {
                auto uploaded = std::make_shared<ModelAsset>();
                uploaded->data = std::move(job.model);
                uploaded->meshes.reserve(uploaded->data.meshes.size());
                std::shared_ptr<const ModelAsset> previous;
                try {
                    for (const auto& mesh : uploaded->data.meshes)
                        uploaded->meshes.push_back(
                            mesh.skinned() ? m_renderer->createSkinnedMesh(mesh.data.vertices, mesh.data.indices, mesh.skin)
                                           : m_renderer->createMesh(mesh.data));
                    // Only base-color maps are currently sampled. Other decoded maps
                    // and PBR factors remain in ModelData for the lighting milestone.
                    uploaded->textures.resize(uploaded->data.textures.size());
                    for (const auto& material : uploaded->data.materials) {
                        const int index = material.baseColorTexture;
                        if (index < 0 || uploaded->textures[index].id != 0) continue;
                        const auto& image = uploaded->data.textures[index].image;
                        uploaded->textures[index] =
                            job.modelTextures[index].valid()
                                ? m_renderer->createTexture(job.modelTextures[index])
                                : m_renderer->createTexture(image.width, image.height, image.rgba);
                    }
                    // Simpler versions of each mesh for distant draws (MeshOptimise.h).
                    for (size_t i = 0; i < uploaded->meshes.size(); ++i) {
                        const MeshData& mesh = uploaded->data.meshes[i].data;
                        if (!mesh.lods.empty()) m_renderer->setMeshLods(uploaded->meshes[i], mesh.lods, mesh.lodIndices);
                    }
                    previous = m_assets.model(m_assets.findModel(f.name));
                    f.warnings = uploaded->data.warnings;
                    f.model = m_assets.addModel(f.name, uploaded);
                } catch (const std::exception& e) {
                    release(*m_renderer, *uploaded);
                    job.error = e.what();
                }
                if (job.error.empty() && previous) release(*m_renderer, *previous);
            } else {
                try {
                    if (job.texture.valid() && job.reload)
                        m_renderer->updateTexture(f.texture, job.texture);
                    else if (job.texture.valid())
                        f.texture = m_renderer->createTexture(job.texture);
                    else if (job.reload)
                        m_renderer->updateTexture(f.texture, job.image.width, job.image.height, job.image.rgba);
                    else
                        f.texture = m_renderer->createTexture(job.image.width, job.image.height, job.image.rgba);
                } catch (const std::exception& e) {
                    job.error = e.what();
                }
            }
        }
        if (!job.error.empty()) {
            // A file caught mid-save fails to decode; the next save changes it again and
            // it's retried then. Until then the last good version stays in use.
            f.error = job.error;
            changes.push_back({Change::What::Failed, f.name, job.error});
            continue;
        }
        f.error.clear();
        f.width = job.image.width;
        f.height = job.image.height;
        if (job.texture.valid()) {
            f.width = job.texture.width;
            f.height = job.texture.height;
        }
        recordOptimisation(f, job);
        if (f.kind == Kind::Texture && !job.reload) m_assets.addTexture(f.name, f.texture);
        changes.push_back({job.reload ? Change::What::Reloaded : Change::What::Added, f.name, {}});
        f.imported = true;
    }

    m_files = std::move(found);
    return changes;
}

}   // namespace shockwave
