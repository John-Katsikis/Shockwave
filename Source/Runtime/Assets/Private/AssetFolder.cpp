#include <Assets/AssetFolder.h>

#include <Assets/Image.h>
#include <Assets/TextureCompression.h>
#include <Core/Jobs.h>
#include <Renderer/Renderer.h>

#include "TextureCompressor.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <atomic>
#include <set>
#include <cmath>
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
TextureUsage textureUsage(const ImportSettings& settings) {
    return settings.category == TextureCategory::Ui ? TextureUsage::Fixed : TextureUsage::Material;
}
// One RGBA8 level of an image, for textureMipChain and createTexture(TextureData).
TextureData rgbaTexture(const Image& image, bool srgb) {
    TextureData texture;
    texture.srgb = srgb;
    texture.width = image.width;
    texture.height = image.height;
    texture.bytes = image.rgba;
    texture.levels = {{image.width, image.height, 0, image.rgba.size()}};
    return texture;
}
std::string variantKey(bool srgb, float coverageCutoff) {
    return srgb ? "coverage " + std::to_string(coverageCutoff) : "linear";
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

// One RGBA16F level; the GPU makes the mipmaps. Brighter than a half float holds is clamped.
TextureData halfFloatTexture(const HdrImage& image) {
    TextureData texture;
    texture.format = TextureFormat::RGBA16F;
    texture.srgb = false;
    texture.width = image.width;
    texture.height = image.height;
    const size_t size = textureLevelBytes(TextureFormat::RGBA16F, image.width, image.height);
    texture.levels = {{image.width, image.height, 0, size}};
    texture.bytes.resize(size);
    auto* out = reinterpret_cast<uint16_t*>(texture.bytes.data());
    for (size_t i = 0; i < image.rgba.size(); ++i) {
        const float value = image.rgba[i];
        out[i] = glm::packHalf1x16(std::isfinite(value) ? std::clamp(value, 0.0f, 65504.0f) : 0.0f);
    }
    return texture;
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
        if (!task->variant.empty()) {
            const auto variant = std::find_if(f->variants.begin(), f->variants.end(), [&](const TextureVariant& v) {
                return v.key == task->variant && v.modified == task->modified && v.bytes == task->bytes;
            });
            if (variant == f->variants.end()) continue;
            target = variant->texture;
            variant->gpuBytes = task->texture.gpuBytes();
        } else if (task->modelTexture >= 0) {
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
        if (!task->variant.empty()) continue;   // counted with the variant
        const uint64_t raw = rgbaBytes(task->texture.width, task->texture.height);
        f->gpuBytes = (f->gpuBytes > raw ? f->gpuBytes - raw : 0) + task->texture.gpuBytes();
        f->compressedTextures += task->texture.format == TextureFormat::BC7;
        f->compressedFileBytes += task->compressedBytes;
        changes.push_back({Change::What::Compressed, f->name, {}});
    }
}

// ---- Materials (A02) ----

void AssetFolder::resolveMaterials(std::vector<Change>& changes) {
    if (!m_renderer) return;
    std::vector<std::shared_ptr<const ModelAsset>> models;   // kept alive while resolving
    MaterialLookup lookup;
    lookup.file = [&](std::string_view name, MaterialFile& file, std::string& error) {
        const File* f = find(name);
        if (!f || f->kind != Kind::Material) {
            error = "can't open " + std::string(name) + ": it isn't in Content";
            return false;
        }
        if (!f->materialFile) {
            error = f->error;
            return false;
        }
        file = *f->materialFile;   // the last version that parsed
        return true;
    };
    lookup.model = [&](std::string_view name, std::string& error) -> const ModelData* {
        const File* f = find(name);
        if (!f || f->kind != Kind::Model) {
            error = "it isn't a model in Content";
            return nullptr;
        }
        auto model = m_assets.model(f->model);
        if (!model) {
            error = f->error.empty() ? "it isn't imported yet" : f->error;
            return nullptr;
        }
        models.push_back(model);
        return &models.back()->data;
    };
    lookup.texture = [&](std::string_view name) {
        const File* f = find(name);
        return f && f->kind == Kind::Texture && f->imported;
    };

    struct Resolved {
        File* file;
        MaterialDesc desc;
        MaterialProblem problem = MaterialProblem::None;
        std::string error;
    };
    std::vector<Resolved> resolved;
    for (File& f : m_files)
        if (f.kind == Kind::Material) {
            Resolved& r = resolved.emplace_back();
            r.file = &f;
            r.problem = resolveMaterial(f.name, lookup, r.desc, r.error);
        }
    if (resolved.empty()) return;

    // The decodings each slot needs: colour slots read a file as sRGB, data slots as linear,
    // and a masked material's base color gets coverage-keeping mipmaps (A02).
    struct VariantJob {
        File* file;
        bool srgb;
        float cutoff;
        std::string key;
        TextureData texture;
        Image image;   // to compress in the background, when it wasn't in the cache
        uint64_t cacheKey = 0;
        std::string error;
    };
    std::vector<VariantJob> jobs;
    auto wanted = [](const Resolved& r, size_t slot, bool& srgb, float& cutoff) {
        srgb = materialSlotInfo(MaterialSlot(slot)).srgb;
        cutoff = slot == size_t(MaterialSlot::BaseColor) && r.desc.params.alphaMode == int(MaterialAlpha::Mask)
                     ? r.desc.params.alphaCutoff
                     : -1.0f;
    };
    // The file's own texture serves when it already reads the way the slot needs.
    auto ownServes = [](const File& f, bool srgb, float cutoff) {
        return f.ownLevels || (srgb == f.srgb && cutoff < 0.0f);
    };
    for (const Resolved& r : resolved) {
        if (r.problem != MaterialProblem::None) continue;
        for (size_t slot = 0; slot < kAllMaterialSlots; ++slot) {
            const MaterialTexture& t = r.desc.slots[slot].texture;
            File* tf = t.empty() || t.modelTexture >= 0 ? nullptr : findFile(t.asset);
            if (!tf) continue;
            bool srgb;
            float cutoff;
            wanted(r, slot, srgb, cutoff);
            if (ownServes(*tf, srgb, cutoff)) continue;
            const std::string key = variantKey(srgb, cutoff);
            const bool fresh = std::any_of(tf->variants.begin(), tf->variants.end(), [&](const TextureVariant& v) {
                return v.key == key && v.modified == tf->modified && v.bytes == tf->bytes;
            });
            const bool queued = std::any_of(
                jobs.begin(), jobs.end(), [&](const VariantJob& j) { return j.file == tf && j.key == key; });
            if (!fresh && !queued) jobs.push_back({tf, srgb, cutoff, key, {}, {}, 0, {}});
        }
    }
    const bool bc7 = m_renderer->supportsTextureFormat(TextureFormat::BC7);
    m_jobs.parallelFor(jobs.size(), 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            VariantJob& job = jobs[i];
            const File& f = *job.file;
            std::vector<uint8_t> bytes;
            if (!readFileBytes(f.path, bytes, job.error)) continue;
            const TextureCompression mode = f.settings.compression;
            if (mode != TextureCompression::Off && !m_cacheFolder.empty()) {
                job.cacheKey = textureCacheKey(bytes, mode, job.srgb, job.cutoff);
                std::vector<uint8_t> ktx2;
                std::string ignored;
                const fs::path cache = textureCachePath(m_cacheFolder, f.name + "#" + job.key, job.cacheKey);
                std::error_code ec;
                if (fs::is_regular_file(cache, ec) && readFileBytes(cache, ktx2, ignored) &&
                    transcodeKtx2(ktx2, bc7, job.texture, ignored))
                    continue;
            } else if (mode != TextureCompression::Off) {
                job.cacheKey = textureCacheKey(bytes, mode, job.srgb, job.cutoff);
            }
            if (!decodeImage(bytes, job.image, job.error)) continue;
            job.texture = rgbaTexture(job.image, job.srgb);
            if (job.cutoff >= 0.0f) job.texture = textureMipChain(job.texture, job.cutoff);
        }
    });
    for (VariantJob& job : jobs) {
        File& f = *job.file;
        if (!job.error.empty() || !job.texture.valid()) {
            f.warnings.push_back("Couldn't decode it for a material (" + job.key + "): " + job.error);
            continue;
        }
        auto variant = std::find_if(
            f.variants.begin(), f.variants.end(), [&](const TextureVariant& v) { return v.key == job.key; });
        if (variant == f.variants.end()) {
            variant = f.variants.insert(f.variants.end(), TextureVariant{});
            variant->key = job.key;
        }
        variant->srgb = job.srgb;
        variant->coverageCutoff = job.cutoff;
        variant->modified = f.modified;
        variant->bytes = f.bytes;
        f.gpuBytes -= std::min(f.gpuBytes, variant->gpuBytes);
        variant->gpuBytes = job.texture.gpuBytes();
        f.gpuBytes += variant->gpuBytes;
        const TextureUsage usage = textureUsage(f.settings);
        try {
            if (variant->texture.id)
                m_renderer->updateTexture(variant->texture, job.texture, usage);
            else
                variant->texture = m_renderer->createTexture(job.texture, usage);
        } catch (const std::exception& e) {
            f.warnings.push_back(std::string("Couldn't upload it for a material: ") + e.what());
            continue;
        }
        if (job.image.width && m_backgroundCompression && f.settings.compression != TextureCompression::Off) {
            auto task = std::make_shared<TextureCompressor::Task>();
            task->asset = f.name;
            task->modified = f.modified;
            task->bytes = f.bytes;
            task->variant = job.key;
            task->image = std::move(job.image);
            task->mode = f.settings.compression;
            task->srgb = job.srgb;
            task->coverageCutoff = job.cutoff;
            task->bc7 = bc7;
            if (!m_cacheFolder.empty())
                task->cacheFile = textureCachePath(m_cacheFolder, f.name + "#" + job.key, job.cacheKey);
            if (!m_compressor) m_compressor = std::make_unique<TextureCompressor>();
            m_compressor->add(std::move(task));
            ++f.compressing;
        }
    }

    // Publish: each material's textures and samplers, as a new immutable version.
    std::set<std::pair<File*, std::string>> used;
    for (Resolved& r : resolved) {
        File& f = *r.file;
        const auto previous = f.material.valid() ? m_assets.material(f.material) : nullptr;
        std::shared_ptr<MaterialAsset> next;
        std::string error = r.error;
        if (r.problem == MaterialProblem::None) {
            next = std::make_shared<MaterialAsset>();
            next->desc = std::move(r.desc);
            std::array<TextureHandle, kAllMaterialSlots> textures{};
            std::array<SamplerHandle, kAllMaterialSlots> samplers{};
            for (size_t slot = 0; slot < kAllMaterialSlots; ++slot) {
                samplers[slot] = m_renderer->sampler(next->desc.slots[slot].binding.sampler);
                const MaterialTexture& t = next->desc.slots[slot].texture;
                if (t.empty()) continue;
                if (t.modelTexture >= 0) {
                    const File* mf = find(t.asset);
                    const auto model = mf ? m_assets.model(mf->model) : nullptr;
                    if (model && size_t(t.modelTexture) < model->textures.size())
                        textures[slot] = model->textures[size_t(t.modelTexture)];
                    continue;
                }
                File* tf = findFile(t.asset);
                if (!tf) continue;
                bool srgb;
                float cutoff;
                wanted({&f, next->desc, {}, {}}, slot, srgb, cutoff);
                textures[slot] = tf->texture;
                if (ownServes(*tf, srgb, cutoff)) {
                    if (tf->ownLevels && cutoff >= 0.0f)
                        next->desc.warnings.push_back(
                            t.asset + " has its own mipmaps, so they aren't adjusted to keep its cut-out coverage");
                    else if (tf->ownLevels && srgb != tf->srgb)
                        next->desc.warnings.push_back(
                            t.asset + " is stored as " + (tf->srgb ? "sRGB colour" : "linear data") + " but " +
                            materialSlotName(MaterialSlot(slot)) + " reads " + (srgb ? "colour" : "data") +
                            "; re-encode it to match");
                    continue;
                }
                const std::string key = variantKey(srgb, cutoff);
                for (const TextureVariant& v : tf->variants)
                    if (v.key == key) textures[slot] = v.texture;
                used.insert({tf, key});
            }
            buildDrawMaterial(*next, textures, samplers);
        } else if (r.problem == MaterialProblem::File && previous && !previous->fallback) {
            // Mid-save, say: the last good version stays until the file parses again.
            if (!f.materialParseFailed) f.error = error;
            continue;
        } else {
            next = std::make_shared<MaterialAsset>(fallbackMaterial(error));
        }
        next->generation = previous ? previous->generation + 1 : 1;
        if (!f.materialParseFailed) f.error = next->fallback ? error : std::string();
        f.warnings = next->desc.warnings;
        f.dependencies.clear();
        for (const auto& name : next->desc.dependencies) f.dependencies.push_back(stamp(m_root / name));
        f.dependencies.push_back(stamp(f.path));
        const bool same = previous && previous->fallback == next->fallback && previous->desc == next->desc &&
                          previous->error == next->error &&
                          std::equal(
                              previous->textures.begin(), previous->textures.end(), next->textures.begin(),
                              [](TextureHandle a, TextureHandle b) { return a.id == b.id; }) &&
                          std::equal(previous->samplers.begin(), previous->samplers.end(), next->samplers.begin());
        if (same) continue;
        f.material = m_assets.addMaterial(f.name, next);
        f.imported = true;
        const bool reported = std::any_of(changes.begin(), changes.end(), [&](const Change& c) {
            return c.name == f.name && c.what != Change::What::Compressed;
        });
        if (next->fallback)
            changes.push_back({Change::What::Failed, f.name, error});
        else if (!reported)
            changes.push_back({previous ? Change::What::Reloaded : Change::What::Added, f.name, {}});
    }
    // Decodings no material reads any more.
    for (File& f : m_files)
        std::erase_if(f.variants, [&](const TextureVariant& v) {
            if (used.count({&f, v.key})) return false;
            m_renderer->destroyTexture(v.texture);
            f.gpuBytes -= std::min(f.gpuBytes, v.gpuBytes);
            return true;
        });
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
            // A baked lightmap (Lighting/<Scene>/Lightmap_000.ktx2 beside Lighting/<Scene>.swlighting)
            // is loaded by the baked lighting runtime, not imported as a texture.
            const bool bakedPayload = isKtx2File(f.path) && f.path.has_parent_path() &&
                                      (fs::exists(f.path.parent_path().string() + ".swlighting", ec) ||
                                       (f.path.filename().string().rfind("Lightmap_", 0) == 0 &&
                                        f.path.parent_path().parent_path().filename() == "Lighting"));
            f.kind = bakedPayload                                ? Kind::Other
                     : isImageFile(f.path) || isKtx2File(f.path) ? Kind::Texture
                     : isModelFile(f.path)                       ? Kind::Model
                     : isAudioFile(f.path)                       ? Kind::Audio
                     : isMaterialFile(f.path)                    ? Kind::Material
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
        std::vector<float> coverage;   // per model texture: modelCoverageCutoffs
        std::shared_ptr<MaterialFile> material;
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
            f.materialFile = old->materialFile;
            f.materialParseFailed = old->materialParseFailed;
            f.material = old->material;
            f.srgb = old->srgb;
            f.ownLevels = old->ownLevels;
            f.variants = old->variants;
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
            if (!old)
                changes.push_back({f.error.empty() ? Change::What::Added : Change::What::Failed, f.name, f.error});
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
    // HDR images (environment maps) keep their full range as half floats, uncompressed.
    const bool halfFloat = m_renderer && m_renderer->supportsTextureFormat(TextureFormat::RGBA16F);
    auto importTexture = [&](const File& f, Import& job) {
        std::vector<uint8_t> bytes;
        if (!readFileBytes(f.path, bytes, job.error)) return;
        if (isKtx2File(f.path)) {
            if (transcodeKtx2(bytes, bc7, job.texture, job.error)) job.compressedFileBytes = bytes.size();
            return;
        }
        if (halfFloat && isHdrImage(bytes)) {
            HdrImage hdr;
            if (decodeHdrImage(bytes, hdr, job.error)) job.texture = halfFloatTexture(hdr);
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
        job.coverage = modelCoverageCutoffs(job.model);
        if (job.settings.compression == TextureCompression::Off) return;
        for (const ModelMaterial& material : job.model.materials)
            for (const int index : materialTextures(material)) {
                if (index < 0 || job.modelTextures[index].valid()) continue;
                const ModelTexture& texture = job.model.textures[index];
                const uint64_t key =
                    textureCacheKey(texture.image.rgba, job.settings.compression, texture.srgb, job.coverage[index]);
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
            if (f.kind != Kind::Audio && !loadImportSettings(importSettingsPath(f.path), job.settings, job.error))
                continue;
            if (f.kind == Kind::Audio)
                loadAudioClip(f.path, job.audio, job.error);
            else if (f.kind == Kind::Material) {
                job.material = std::make_shared<MaterialFile>();
                loadMaterialFile(f.path, *job.material, job.error);
            } else if (f.kind == Kind::Model)
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
        auto compress = [&](Image image, bool srgb, int modelTexture, std::shared_ptr<const void> owner,
                            const std::string& cacheName, uint64_t key, float coverage = -1.0f) {
            if (!m_renderer || !m_backgroundCompression || mode == TextureCompression::Off || !image.width) return;
            auto task = std::make_shared<TextureCompressor::Task>();
            task->asset = f.name;
            task->modified = f.modified;
            task->bytes = f.bytes;
            task->modelTexture = modelTexture;
            task->owner = std::move(owner);
            task->image = std::move(image);
            task->mode = mode;
            task->srgb = srgb;
            task->coverageCutoff = coverage;
            task->bc7 = bc7;
            if (!m_cacheFolder.empty()) task->cacheFile = textureCachePath(m_cacheFolder, cacheName, key);
            if (!m_compressor) m_compressor = std::make_unique<TextureCompressor>();
            m_compressor->add(std::move(task));
            ++f.compressing;
        };
        if (f.kind == Kind::Texture) {
            account(job.texture, f.width, f.height);
            if (!job.texture.valid()) compress(std::move(job.image), true, -1, nullptr, f.name, job.cacheKey);
            return;
        }
        const auto model = m_assets.model(f.model);
        if (!model) return;
        for (size_t i = 0; i < model->textures.size(); ++i) {
            if (model->textures[i].id == 0) continue;   // not sampled, so never uploaded
            const ModelTexture& texture = model->data.textures[i];
            const TextureData& ready = job.modelTextures[i];
            const float coverage = i < job.coverage.size() ? job.coverage[i] : -1.0f;
            account(ready, texture.image.width, texture.image.height);
            if (!ready.valid())
                compress(
                    texture.image, texture.srgb, static_cast<int>(i), model, f.name + "#" + std::to_string(i),
                    textureCacheKey(texture.image.rgba, mode, texture.srgb, coverage), coverage);
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
            } else if (f.kind == Kind::Material) {
                f.materialFile = std::move(job.material);   // resolved below, with everything else
                f.materialParseFailed = false;
            } else if (!m_renderer) {
                job.error = "this audio-only asset folder cannot publish GPU assets";
            } else if (f.kind == Kind::Model) {
                auto uploaded = std::make_shared<ModelAsset>();
                uploaded->data = std::move(job.model);
                uploaded->meshes.reserve(uploaded->data.meshes.size());
                std::shared_ptr<const ModelAsset> previous;
                try {
                    for (const auto& mesh : uploaded->data.meshes) {
                        uploaded->meshes.push_back(
                            mesh.skinned()
                                ? m_renderer->createSkinnedMesh(mesh.data.vertices, mesh.data.indices, mesh.skin)
                                : m_renderer->createMesh(mesh.data));
                        if (mesh.skinned()) m_renderer->setMeshUv1(uploaded->meshes.back(), mesh.data.uv1);
                    }
                    // Every map a material samples: colour maps as sRGB, data maps linear. They
                    // are material textures, so texture_quality may shrink them (A01).
                    // A masked material's base color gets mipmaps that keep its cut-out coverage.
                    uploaded->textures.resize(uploaded->data.textures.size());
                    const auto usage = textureUsage(job.settings);
                    for (const auto& material : uploaded->data.materials) {
                        auto& samplers = uploaded->samplers.emplace_back();
                        for (size_t slot = 0; slot < kMaterialSlots; ++slot) {
                            const int index = materialTextures(material)[slot];
                            if (index < 0) continue;
                            samplers[slot] = m_renderer->sampler(material.bindings[slot].sampler);
                            if (uploaded->textures[index].id != 0) continue;
                            const auto& texture = uploaded->data.textures[index];
                            const float coverage = job.coverage[size_t(index)];
                            uploaded->textures[index] =
                                job.modelTextures[index].valid() ? m_renderer->createTexture(job.modelTextures[index], usage)
                                : coverage >= 0.0f
                                    ? m_renderer->createTexture(
                                          textureMipChain(rgbaTexture(texture.image, texture.srgb), coverage), usage)
                                    : m_renderer->createTexture(
                                          texture.image.width, texture.image.height, texture.image.rgba, texture.srgb,
                                          usage);
                        }
                    }
                    // What each material draws with: the same path authored materials take.
                    for (uint32_t i = 0; i < uploaded->data.materials.size(); ++i)
                        uploaded->drawMaterials.push_back(modelDrawMaterial(*uploaded, i));
                    // Simpler versions of each mesh for distant draws (MeshOptimise.h).
                    for (size_t i = 0; i < uploaded->meshes.size(); ++i) {
                        const MeshData& mesh = uploaded->data.meshes[i].data;
                        if (!mesh.lods.empty())
                            m_renderer->setMeshLods(uploaded->meshes[i], mesh.lods, mesh.lodIndices);
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
                    // HDR environment maps light the scene; they keep their full size.
                    const auto usage = job.texture.valid() && job.texture.format == TextureFormat::RGBA16F
                                           ? TextureUsage::Fixed
                                           : textureUsage(job.settings);
                    f.srgb = !job.texture.valid() || job.texture.srgb;
                    // A .ktx2's levels, or an HDR image. A compressed copy of a PNG from the
                    // cache isn't: the PNG can still be decoded another way for a material.
                    f.ownLevels = job.texture.valid() &&
                                  (isKtx2File(f.path) || job.texture.format == TextureFormat::RGBA16F);
                    if (job.texture.valid() && job.reload)
                        m_renderer->updateTexture(f.texture, job.texture, usage);
                    else if (job.texture.valid())
                        f.texture = m_renderer->createTexture(job.texture, usage);
                    else if (job.reload)
                        m_renderer->updateTexture(
                            f.texture, job.image.width, job.image.height, job.image.rgba, true, usage);
                    else
                        f.texture =
                            m_renderer->createTexture(job.image.width, job.image.height, job.image.rgba, true, usage);
                } catch (const std::exception& e) {
                    job.error = e.what();
                }
            }
        }
        if (!job.error.empty()) {
            // A file caught mid-save fails to decode; the next save changes it again and
            // it's retried then. Until then the last good version stays in use.
            f.error = job.error;
            if (f.kind == Kind::Material) f.materialParseFailed = true;
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
    if (!changes.empty()) resolveMaterials(changes);
    return changes;
}

}   // namespace shockwave
