#include <Assets/AssetFolder.h>

#include <Assets/Image.h>
#include <Core/Jobs.h>
#include <Renderer/Renderer.h>

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

std::vector<AssetFolder::Change> AssetFolder::open(fs::path root) {
    m_jobs.wait(m_audioJobs);
    m_audioImports.clear();
    for (const auto& old : m_files)
        if (old.kind == Kind::Audio) m_assets.addAudioClip(old.name, nullptr);
    m_root = std::move(root);
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
            File f;
            f.path = it->path();
            f.name = fs::relative(f.path, m_root, ec).generic_string();
            f.kind = isImageFile(f.path)   ? Kind::Texture
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
        size_t file;
        bool reload;
        AudioClip audio;
        Image image;
        ModelData model;
        std::vector<fs::path> dependencies;
        std::string error;
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
            if (!needsImport) continue;
        }
        if (f.kind == Kind::Other) {
            if (!old) changes.push_back({Change::What::Added, f.name, {}});
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
        imports.push_back({i, f.imported, {}, {}, {}, {}, {}});
    }
    for (const File& old : m_files) {
        const bool gone = std::none_of(found.begin(), found.end(), [&](const File& f) { return f.name == old.name; });
        // Its texture stays registered, so scenes using it keep their last good copy.
        if (gone) {
            if (old.kind == Kind::Audio) m_assets.addAudioClip(old.name, nullptr);
            changes.push_back({Change::What::Removed, old.name, {}});
        }
    }

    // CPU imports run in parallel with one Assimp instance per job. Publication and
    // GPU resource retirement run between frames on the main thread.
    m_jobs.parallelFor(imports.size(), 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            auto& job = imports[i];
            const auto& f = found[job.file];
            if (f.kind == Kind::Audio)
                loadAudioClip(f.path, job.audio, job.error);
            else if (f.kind == Kind::Model)
                loadModel(f.path, job.model, job.error, &job.dependencies);
            else
                loadImage(f.path, job.image, job.error);
        }
    });

    for (Import& job : imports) {
        File& f = found[job.file];
        f.dependencies.clear();
        for (const auto& path : job.dependencies) f.dependencies.push_back(stamp(path));
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
                        uploaded->meshes.push_back(m_renderer->createMesh(mesh.data));
                    // Only base-color maps are currently sampled. Other decoded maps
                    // and PBR factors remain in ModelData for the lighting milestone.
                    uploaded->textures.resize(uploaded->data.textures.size());
                    for (const auto& material : uploaded->data.materials) {
                        const int index = material.baseColorTexture;
                        if (index < 0 || uploaded->textures[index].id != 0) continue;
                        const auto& image = uploaded->data.textures[index].image;
                        uploaded->textures[index] = m_renderer->createTexture(image.width, image.height, image.rgba);
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
                    if (job.reload)
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
        if (f.kind == Kind::Texture && !job.reload) m_assets.addTexture(f.name, f.texture);
        changes.push_back({job.reload ? Change::What::Reloaded : Change::What::Added, f.name, {}});
        f.imported = true;
    }

    m_files = std::move(found);
    return changes;
}

}   // namespace shockwave
