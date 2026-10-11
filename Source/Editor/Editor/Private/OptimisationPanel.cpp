// The Optimisation window (docs/Optimisation.md): the project's texture compression and
// level-of-detail settings, overrides for the asset selected in Content, and what it all
// saves on the GPU.
#include "Editor.h"

#include <Assets/ImportSettings.h>
#include <Projects/Project.h>
#include <UI/Gui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::TextureCompression;

namespace {
constexpr TextureCompression kModes[] = {TextureCompression::High, TextureCompression::Small, TextureCompression::Off};
const char* modeLabel(TextureCompression mode) {
    switch (mode) {
    case TextureCompression::High: return "High (BC7, sharp)";
    case TextureCompression::Small: return "Small (BC7, smaller files)";
    case TextureCompression::Off: return "Off (RGBA8)";
    }
    return "?";
}
const char* modeTooltip(TextureCompression mode) {
    switch (mode) {
    case TextureCompression::High: return "A quarter of the GPU memory, close to the original. The default.";
    case TextureCompression::Small:
        return "The same GPU memory as High, much smaller cache files, slightly softer detail.";
    case TextureCompression::Off: return "Uncompressed: four times the GPU memory. For pixel art or exact colours.";
    }
    return "";
}
// Returns true when the choice changed.
bool modeCombo(const char* id, TextureCompression& mode) {
    bool changed = false;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo(id, modeLabel(mode))) {
        for (TextureCompression option : kModes) {
            if (ImGui::Selectable(modeLabel(option), option == mode) && option != mode) {
                mode = option;
                changed = true;
            }
            ImGui::SetItemTooltip("%s", modeTooltip(option));
        }
        ImGui::EndCombo();
    }
    return changed;
}
std::string megabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}
std::string levelText(const shockwave::MeshOptimiseReport& meshes) {
    std::string text;
    for (uint32_t i = 0; i < meshes.levels; ++i) text += (i ? " > " : "") + std::to_string(meshes.levelTriangles[i]);
    return text;
}
}   // namespace

void Editor::applyOptimisationSettings() {
    shockwave::ImportSettings defaults;
    shockwave::parseTextureCompression(m_project.textureCompression, defaults.compression);
    defaults.lods = static_cast<uint32_t>(std::clamp(m_project.meshLods, 0, 7));
    if (!(defaults == m_assetFolder->importDefaults())) m_assetFolder->setImportDefaults(defaults);
    m_assetFolder->setCacheFolder(m_project.statePath() / "Cache" / "Textures");
    engine().renderer().setLodSettings({static_cast<float>(m_project.lodErrorPixels), m_showLodColors});
}

void Editor::saveOptimisationSettings() {
    // Only these keys: anything else being edited in Project settings stays unsaved there.
    m_projectEdit.textureCompression = m_project.textureCompression;
    m_projectEdit.meshLods = m_project.meshLods;
    m_projectEdit.lodErrorPixels = m_project.lodErrorPixels;
    std::string err;
    if (shockwave::saveProject(m_project, err))
        log("Saved optimisation settings to " + m_project.file.filename().string());
    else
        error("Couldn't save the project: " + err);
    applyOptimisationSettings();
}

void Editor::drawOptimisationWindow() {
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Optimisation", &m_showOptimisation)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject) {
        ImGui::TextDisabled("Open a project to see its textures and meshes.");
        ImGui::End();
        return;
    }

    // ---- Project defaults: the .swproject's texture_compression, mesh_lods, lod_error_pixels ----
    ImGui::SeparatorText("Project");
    if (gui::beginPropertyGrid("optimisation_project")) {
        gui::propertyLabel("Texture compression");
        TextureCompression mode = TextureCompression::High;
        shockwave::parseTextureCompression(m_project.textureCompression, mode);
        if (modeCombo("##compression", mode)) {
            m_project.textureCompression = shockwave::textureCompressionName(mode);
            saveOptimisationSettings();
        }
        gui::propertyLabel("Levels of detail");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##lods", &m_project.meshLods, 0, 7, m_project.meshLods == 0 ? "off" : "%d per mesh");
        ImGui::SetItemTooltip("Simpler versions of each model mesh, used when it's far away. Re-imports models.");
        if (ImGui::IsItemDeactivatedAfterEdit()) saveOptimisationSettings();
        gui::propertyLabel("LOD pixel error");
        ImGui::SetNextItemWidth(-1.0f);
        if (!m_lodErrorActive) m_lodErrorEdit = static_cast<float>(m_project.lodErrorPixels);
        if (ImGui::DragFloat("##lod_error", &m_lodErrorEdit, 0.05f, 0.0f, 100.0f, "%.2f px",
                             ImGuiSliderFlags_AlwaysClamp)) {
            m_project.lodErrorPixels = m_lodErrorEdit;
            applyOptimisationSettings();   // live: just which level each draw picks
        }
        ImGui::SetItemTooltip(
            "How far, in pixels, a simpler level may stray from the full mesh on screen.\n"
            "Lower is sharper, higher draws fewer triangles. 0 always draws full detail.");
        m_lodErrorActive = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) saveOptimisationSettings();
        ImGui::EndTable();
    }

    // ---- The asset selected in Content: its .swimport overrides ----
    ImGui::SeparatorText("Selected asset");
    const shockwave::AssetFolder::File* file =
        m_contentRoot == ContentRoot::Assets ? m_assetFolder->find(m_selectedContent) : nullptr;
    using Kind = shockwave::AssetFolder::Kind;
    if (!file || (file->kind != Kind::Texture && file->kind != Kind::Model)) {
        ImGui::TextDisabled("Select a texture or model in Content to change how it's imported.");
    } else {
        ImGui::TextUnformatted(file->name.c_str());
        const shockwave::ImportSettings& defaults = m_assetFolder->importDefaults();
        shockwave::ImportSettings settings = file->settings;
        bool changed = false;
        if (gui::beginPropertyGrid("optimisation_asset")) {
            gui::propertyLabel("Texture compression");
            changed |= modeCombo("##asset_compression", settings.compression);
            if (file->kind == Kind::Model) {
                gui::propertyLabel("Levels of detail");
                // While dragging, the slider keeps its own value; it's saved when let go.
                if (!m_importLodsActive) m_importLodsEdit = static_cast<int>(settings.lods);
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::SliderInt(
                    "##asset_lods", &m_importLodsEdit, 0, 7, m_importLodsEdit == 0 ? "off" : "%d per mesh");
                m_importLodsActive = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    settings.lods = static_cast<uint32_t>(m_importLodsEdit);
                    changed |= !(settings == file->settings);
                }
            }
            ImGui::EndTable();
        }
        const bool overridden = !(file->settings == defaults);
        if (overridden)
            ImGui::TextDisabled("Overrides the project in %s", shockwave::importSettingsPath(file->path).filename().string().c_str());
        else
            ImGui::TextDisabled("Uses the project's settings");
        ImGui::BeginDisabled(!overridden);
        if (gui::button("Use project settings")) {
            settings = defaults;
            changed = true;
        }
        ImGui::EndDisabled();
        if (changed) {
            std::string err;
            if (shockwave::saveImportSettings(file->path, settings, defaults, err))
                log("Import settings for " + file->name + " saved; re-importing");
            else
                error(err);
            logAssetChanges(m_assetFolder->refresh());
            file = m_assetFolder->find(m_selectedContent);
        }

        if (file && file->imported && file->error.empty() && gui::beginPropertyGrid("optimisation_asset_stats")) {
            if (file->textures > 0) {
                gui::propertyLabel("GPU memory");
                ImGui::Text("%s (%s uncompressed)", megabytes(file->gpuBytes).c_str(),
                            megabytes(file->uncompressedBytes).c_str());
                gui::propertyLabel("Compressed");
                if (file->compressing > 0)
                    ImGui::TextColored(gui::accent(), "%u of %u, %u compressing...", file->compressedTextures,
                                       file->textures, file->compressing);
                else
                    ImGui::Text("%u of %u textures", file->compressedTextures, file->textures);
                if (file->compressedFileBytes > 0) {
                    gui::propertyLabel(".ktx2 size");
                    ImGui::TextUnformatted(megabytes(file->compressedFileBytes).c_str());
                }
            }
            if (file->kind == Kind::Model && file->meshes.meshes > 0) {
                gui::propertyLabel("Triangles per level");
                ImGui::TextWrapped("%s", levelText(file->meshes).c_str());
                ImGui::SetItemTooltip("Full detail first. Small meshes (under 256 triangles) get no levels.");
                gui::propertyLabel("Vertices shaded");
                ImGui::Text("%.2f per triangle (%.2f before)", static_cast<double>(file->meshes.shadedPerTriangleAfter),
                            static_cast<double>(file->meshes.shadedPerTriangleBefore));
                ImGui::SetItemTooltip("Lower is better: triangles are reordered so the GPU reuses shaded vertices.");
            }
            ImGui::EndTable();
        }
    }

    // ---- Every asset ----
    ImGui::SeparatorText("All assets");
    uint64_t gpu = 0, uncompressed = 0;
    uint32_t textures = 0, compressed = 0;
    for (const auto& f : m_assetFolder->files()) {
        gpu += f.gpuBytes;
        uncompressed += f.uncompressedBytes;
        textures += f.textures;
        compressed += f.compressedTextures;
    }
    ImGui::Text("%u of %u textures compressed", compressed, textures);
    ImGui::Text("%s on the GPU instead of %s", megabytes(gpu).c_str(), megabytes(uncompressed).c_str());
    if (const size_t waiting = m_assetFolder->compressing()) {
        ImGui::TextColored(gui::accent(), "Compressing %zu textures in the background...", waiting);
        ImGui::SameLine();
        if (gui::button("Finish now")) logAssetChanges(m_assetFolder->finishCompressing());
        ImGui::SetItemTooltip("Uses every thread until it's done; the editor pauses meanwhile.");
    }
    if (!engine().renderer().supportsTextureFormat(shockwave::TextureFormat::BC7))
        ImGui::TextColored(gui::semanticColor(gui::currentTheme().warning),
                           "This GPU can't sample BC7: textures are drawn uncompressed.");
    ImGui::TextDisabled("Cache: %s", m_project.relative(m_assetFolder->cacheFolder()).c_str());
    ImGui::SetItemTooltip("Compressed copies kept between runs. Safe to delete; they're rebuilt.");

    // ---- What the renderer did last frame ----
    ImGui::SeparatorText("View");
    if (ImGui::Checkbox("Show levels of detail", &m_showLodColors)) applyOptimisationSettings();
    ImGui::SetItemTooltip("Tints each mesh by the level it's drawn with: green is full detail,\n"
                          "then yellow, orange, red, magenta, blue, cyan, purple.");
    drawOptimisationStats();
    ImGui::End();
}

void Editor::drawOptimisationStats() {
    const shockwave::LodStats stats = engine().renderer().lodStats();
    if (stats.fullTriangles == 0) return;
    ImGui::Text("%llu triangles drawn, %llu at full detail", static_cast<unsigned long long>(stats.triangles),
                static_cast<unsigned long long>(stats.fullTriangles));
    ImGui::SetItemTooltip("%u draws used a simpler level of detail", stats.reducedDraws);
}

}   // namespace editor
