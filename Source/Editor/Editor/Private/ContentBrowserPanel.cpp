// The Content panel: the project's assets (by folder) and scenes, and the engine's
// built-in assets, as tiles you can drag onto the viewport, the hierarchy or the
// inspector. Files saved into Content/ by any program are imported straight away.
#include "Editor.h"

#include <UI/Gui.h>

#include <imgui_stdlib.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::AssetFolder;
using shockwave::Entity;
using shockwave::MeshRenderer;

namespace {

constexpr const char* kBuiltinMeshes[] = {"cube", "sphere", "plane", "floor"};
constexpr const char* kBuiltinTextures[] = {"white", "checker", "grid", "panel"};

enum class TileKind { Folder, Mesh, Model, Texture, Audio, Scene, UIDocument, Prefab, Circuit, Other };

struct Tile {
    TileKind kind;
    std::string name;   // asset name, folder path or scene path: what actions use
    std::string label;   // what shows under the tile
    uint64_t texture = 0;
    const AssetFolder::File* file = nullptr;
    fs::path path;   // on disk, when it's a file or folder
};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string humanSize(uintmax_t bytes) {
    char text[32];
    if (bytes < 1024) {
        std::snprintf(text, sizeof(text), "%ju bytes", bytes);
    } else if (bytes < 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    return text;
}

std::string parentOf(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? std::string() : name.substr(0, slash);
}

std::string leafName(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

// Fits text into width, ending in "..." when it doesn't.
std::string ellipsize(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string cut = text;
    while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width) cut.pop_back();
    return cut + "...";
}

void openInFileManager(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::string url = "file://" + fs::absolute(folder).generic_string();
    SDL_OpenURL(url.c_str());
}

// Line drawings for tiles without a picture.
void drawIcon(ImDrawList* draw, TileKind kind, const std::string& name, ImVec2 c, float half, ImU32 color) {
    const float r = half * 0.62f;
    if (kind == TileKind::Audio) {
        draw->AddCircle(c, half * 0.5f, color, 16, 1.0f);
        draw->AddLine(ImVec2(c.x - half * 0.25f, c.y), ImVec2(c.x + half * 0.25f, c.y), color, 1.0f);
        return;
    }
    if (kind == TileKind::Folder) {
        const ImVec2 lo(c.x - r, c.y - r * 0.6f), hi(c.x + r, c.y + r * 0.75f);
        draw->AddRectFilled(ImVec2(lo.x, lo.y - r * 0.22f), ImVec2(c.x - r * 0.15f, lo.y + 2.0f), color);
        draw->AddRectFilled(lo, hi, color);
        return;
    }
    if (kind == TileKind::Scene) {
        // A little landscape: a frame with a horizon and a sun.
        draw->AddRect(ImVec2(c.x - r, c.y - r * 0.75f), ImVec2(c.x + r, c.y + r * 0.75f), color, 0.0f, 0, 2.0f);
        const ImVec2 hills[4] = {
            {c.x - r, c.y + r * 0.45f},
            {c.x - r * 0.3f, c.y - r * 0.1f},
            {c.x + r * 0.2f, c.y + r * 0.3f},
            {c.x + r, c.y - r * 0.2f}};
        draw->AddPolyline(hills, 4, color, 0, 2.0f);
        draw->AddCircle(ImVec2(c.x + r * 0.45f, c.y - r * 0.38f), r * 0.14f, color, 0, 2.0f);
        return;
    }
    if (kind == TileKind::Prefab) {
        // Three stacked boxes: a group of things kept together.
        const float s = r * 0.5f;
        draw->AddRect(ImVec2(c.x - r, c.y), ImVec2(c.x - r + 2 * s, c.y + r), color, 0.0f, 0, 2.0f);
        draw->AddRect(ImVec2(c.x + r - 2 * s, c.y), ImVec2(c.x + r, c.y + r), color, 0.0f, 0, 2.0f);
        draw->AddRect(ImVec2(c.x - s, c.y - r), ImVec2(c.x + s, c.y), color, 0.0f, 0, 2.0f);
        return;
    }
    if (kind == TileKind::Circuit) {
        // Two nodes and a wire between them.
        const float s = r * 0.42f;
        draw->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r + 2 * s, c.y - r + 1.4f * s), color, 0.0f, 0, 2.0f);
        draw->AddRect(ImVec2(c.x + r - 2 * s, c.y + r - 1.4f * s), ImVec2(c.x + r, c.y + r), color, 0.0f, 0, 2.0f);
        draw->AddBezierCubic(
            ImVec2(c.x - r + 2 * s, c.y - r + 0.7f * s), ImVec2(c.x + r * 0.2f, c.y - r + 0.7f * s),
            ImVec2(c.x - r * 0.2f, c.y + r - 0.7f * s), ImVec2(c.x + r - 2 * s, c.y + r - 0.7f * s), color, 2.0f);
        return;
    }
    if (kind == TileKind::Mesh && name == "sphere") {
        draw->AddCircle(c, r, color, 0, 2.0f);
        draw->AddEllipse(c, ImVec2(r, r * 0.35f), color, 0.0f, 0, 1.2f);
        return;
    }
    if (kind == TileKind::Mesh && (name == "plane" || name == "floor")) {
        const ImVec2 q[4] = {
            {c.x - r, c.y + r * 0.35f},
            {c.x - r * 0.4f, c.y - r * 0.35f},
            {c.x + r, c.y - r * 0.35f},
            {c.x + r * 0.4f, c.y + r * 0.35f}};
        draw->AddPolyline(q, 4, color, ImDrawFlags_Closed, 2.0f);
        return;
    }
    if (kind == TileKind::Mesh) {
        const float d = r * 0.45f;
        draw->AddRect(ImVec2(c.x - r, c.y - r + d), ImVec2(c.x + r - d, c.y + r), color, 0.0f, 0, 2.0f);
        draw->AddLine(ImVec2(c.x - r, c.y - r + d), ImVec2(c.x - r + d, c.y - r), color, 2.0f);
        draw->AddLine(ImVec2(c.x + r - d, c.y - r + d), ImVec2(c.x + r, c.y - r), color, 2.0f);
        draw->AddLine(ImVec2(c.x + r - d, c.y + r), ImVec2(c.x + r, c.y + r - d), color, 2.0f);
        draw->AddLine(ImVec2(c.x - r + d, c.y - r), ImVec2(c.x + r, c.y - r), color, 2.0f);
        draw->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x + r, c.y + r - d), color, 2.0f);
        return;
    }
    // Anything else: its extension in big letters.
    std::string ext = fs::path(name).extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    if (ext.empty()) ext = "file";
    const ImVec2 size = ImGui::CalcTextSize(ext.c_str());
    draw->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), color, ext.c_str());
}

}   // namespace

void Editor::drawContentBrowser() {
    if (m_options.focusContent) {
        ImGui::SetNextWindowFocus();
        if (ImGui::GetFrameCount() > 2) m_options.focusContent = false;
    }
    if (!ImGui::Begin("Content", &m_showContent)) {
        ImGui::End();
        return;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    if (ImGui::BeginTable("content", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthFixed, 190.0f * dpi);
        ImGui::TableSetupColumn("items", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("tree");
        drawContentTree();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("items");
        drawContentItems();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::drawContentTree() {
    auto rootNode = [&](const char* label, ContentRoot root) {
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
        if (m_contentRoot == root && m_contentFolder.empty()) flags |= ImGuiTreeNodeFlags_Selected;
        if (root != ContentRoot::Assets || m_assetFolders.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        const bool open = ImGui::TreeNodeEx(label, flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            m_contentRoot = root;
            m_contentFolder.clear();
        }
        return open;
    };

    const std::string assetsName = m_project.relative(m_project.assetFolder);
    const std::string scenesName = m_project.relative(m_project.sceneFolder);
    if (rootNode(assetsName.c_str(), ContentRoot::Assets)) {
        // Folders as a tree: each one under its parent.
        std::vector<std::string> stack;
        for (const std::string& folder : m_assetFolders) {
            const std::string parent = parentOf(folder);
            while (!stack.empty() && stack.back() != parent) {
                ImGui::TreePop();
                stack.pop_back();
            }
            if (!stack.empty() || parent.empty()) {
                const bool hasChildren =
                    std::any_of(m_assetFolders.begin(), m_assetFolders.end(), [&](const std::string& f) {
                        return parentOf(f) == folder;
                    });
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
                if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;
                if (m_contentRoot == ContentRoot::Assets && m_contentFolder == folder)
                    flags |= ImGuiTreeNodeFlags_Selected;
                // Keep the way down to the current folder open.
                if (m_contentRoot == ContentRoot::Assets && m_contentFolder.starts_with(folder + "/")) {
                    ImGui::SetNextItemOpen(true);
                }
                const bool open = ImGui::TreeNodeEx(folder.c_str(), flags, "%s", leafName(folder).c_str());
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    m_contentRoot = ContentRoot::Assets;
                    m_contentFolder = folder;
                }
                if (open) stack.push_back(folder);
            }
        }
        while (!stack.empty()) {
            ImGui::TreePop();
            stack.pop_back();
        }
        ImGui::TreePop();
    }
    if (rootNode(scenesName.c_str(), ContentRoot::Scenes)) ImGui::TreePop();
    ImGui::Spacing();
    if (rootNode("Engine", ContentRoot::Engine)) ImGui::TreePop();
    ImGui::SetItemTooltip("Built-in meshes and textures, available in every project");
}

void Editor::drawContentItems() {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const ImGuiStyle& style = ImGui::GetStyle();
    const fs::path folderPath = m_contentRoot == ContentRoot::Assets   ? m_project.assetFolder / m_contentFolder
                                : m_contentRoot == ContentRoot::Scenes ? m_project.sceneFolder
                                                                       : fs::path();

    // Header: search leads and expands, folder actions and tile size trail.
    const bool canCreate = m_contentRoot != ContentRoot::Engine;
    const bool canImport = m_contentRoot == ContentRoot::Assets;
    const float sliderWidth = 90.0f * dpi;
    float actionsWidth = sliderWidth;
    if (canCreate) {
        actionsWidth += gui::buttonWidth("New folder") + gui::buttonWidth("Open folder") + 2.0f * style.ItemSpacing.x;
        if (canImport) actionsWidth += gui::buttonWidth("Import...") + style.ItemSpacing.x;
    }
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(
        available < actionsWidth + 120 * dpi ? available
                                             : std::max(1.0f, available - actionsWidth - style.ItemSpacing.x));
    ImGui::InputTextWithHint("##search", "Search content", &m_contentFilter);
    gui::alignTrailing(actionsWidth);
    if (canCreate) {
        if (canImport) {
            if (gui::button("Import...")) showImportDialog(folderPath);
            ImGui::SetItemTooltip("Copy files into this folder");
            gui::toolbarNext(gui::buttonWidth("New folder"));
        }
        if (gui::button("New folder")) {
            std::error_code ec;
            fs::path target = folderPath / "New Folder";
            for (int n = 2; fs::exists(target, ec); ++n) target = folderPath / ("New Folder " + std::to_string(n));
            fs::create_directories(target, ec);
            if (ec) {
                error("Couldn't create " + target.string() + ": " + ec.message());
            } else {
                log("Created " + m_project.relative(target));
                scanScenes();
            }
        }
        gui::toolbarNext(gui::buttonWidth("Open folder"));
        if (gui::button("Open folder")) openInFileManager(folderPath);
        ImGui::SetItemTooltip("Show this folder in the system file manager");
        gui::toolbarNext(sliderWidth);
    }
    ImGui::SetNextItemWidth(std::min(sliderWidth, std::max(1.0f, ImGui::GetContentRegionAvail().x)));
    ImGui::SliderFloat("##size", &m_tileSize, 56.0f, 160.0f, "Size");
    ImGui::SetItemTooltip("Tile size");

    // Breadcrumb: click a step to go back up.
    const std::string rootName = m_contentRoot == ContentRoot::Assets   ? m_project.relative(m_project.assetFolder)
                                 : m_contentRoot == ContentRoot::Scenes ? m_project.relative(m_project.sceneFolder)
                                                                        : "Engine";
    if (ImGui::SmallButton(rootName.c_str())) m_contentFolder.clear();
    if (!m_contentFolder.empty()) {
        size_t start = 0;
        while (start <= m_contentFolder.size()) {
            const size_t slash = m_contentFolder.find('/', start);
            const size_t end = slash == std::string::npos ? m_contentFolder.size() : slash;
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PushID(static_cast<int>(start));
            if (ImGui::SmallButton(m_contentFolder.substr(start, end - start).c_str())) {
                m_contentFolder = m_contentFolder.substr(0, end);
            }
            ImGui::PopID();
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
    }
    ImGui::Separator();

    // Gather what's in this folder. A search looks through the whole root instead.
    const std::string filter = lower(m_contentFilter);
    auto matches = [&](const std::string& text) {
        return filter.empty() || lower(text).find(filter) != std::string::npos;
    };
    std::vector<Tile> tiles;
    if (m_contentRoot == ContentRoot::Assets) {
        if (filter.empty()) {
            for (const std::string& folder : m_assetFolders) {
                if (parentOf(folder) == m_contentFolder) {
                    tiles.push_back(
                        {TileKind::Folder, folder, leafName(folder), 0, nullptr, m_project.assetFolder / folder});
                }
            }
        }
        for (const AssetFolder::File& f : m_assetFolder->files()) {
            if (filter.empty() ? parentOf(f.name) != m_contentFolder : !matches(f.name)) continue;
            const TileKind kind = f.kind == AssetFolder::Kind::Texture ? TileKind::Texture
                                  : f.kind == AssetFolder::Kind::Audio ? TileKind::Audio
                                  : f.kind == AssetFolder::Kind::Model ? TileKind::Model
                                  : f.path.extension() == ".rml"       ? TileKind::UIDocument
                                  : f.path.extension() == ".swprefab"  ? TileKind::Prefab
                                  : f.path.extension() == ".swcircuit" ? TileKind::Circuit
                                                                       : TileKind::Other;
            const uint64_t thumb =
                f.imported && kind == TileKind::Texture ? engine().renderer().guiTexture(f.texture) : 0;
            tiles.push_back({kind, f.name, leafName(f.name), thumb, &f, f.path});
        }
    } else if (m_contentRoot == ContentRoot::Scenes) {
        for (const fs::path& scene : m_scenes) {
            const std::string rel = scene.lexically_relative(m_project.sceneFolder).generic_string();
            if (!matches(rel)) continue;
            tiles.push_back({TileKind::Scene, scene.string(), rel, 0, nullptr, scene});
        }
    } else {
        for (const char* mesh : kBuiltinMeshes) {
            if (matches(mesh)) tiles.push_back({TileKind::Mesh, mesh, mesh, 0, nullptr, {}});
        }
        for (const char* texture : kBuiltinTextures) {
            if (!matches(texture)) continue;
            const auto handle = engine().assets().findTexture(texture);
            tiles.push_back(
                {TileKind::Texture,
                 texture,
                 texture,
                 handle ? engine().renderer().guiTexture(*handle) : 0,
                 nullptr,
                 {}});
        }
    }

    ImGui::BeginChild("tiles");
    if (tiles.empty()) {
        if (!filter.empty()) {
            ImGui::TextDisabled("Nothing matches \"%s\"", m_contentFilter.c_str());
        } else if (m_contentRoot == ContentRoot::Scenes) {
            ImGui::TextDisabled("No scenes yet. Save one with Ctrl+S.");
        } else {
            ImGui::TextDisabled("This folder is empty. Import files, or save them into");
            ImGui::TextDisabled("%s", folderPath.string().c_str());
        }
    }

    const float side = m_tileSize * dpi;
    const float labelHeight = ImGui::GetTextLineHeight() + 6.0f;
    const ImVec2 tileSize(side, side + labelHeight);
    const float spacing = style.ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (side + spacing)));
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 accentColor = gui::semanticColorU32(gui::currentTheme().accent);
    const ImU32 errorColor = gui::semanticColorU32(gui::currentTheme().error);
    std::string navigateTo;
    bool navigate = false;

    for (size_t i = 0; i < tiles.size(); ++i) {
        const Tile& tile = tiles[i];
        const bool failed = tile.file && !tile.file->error.empty();
        const bool usable = tile.kind == TileKind::UIDocument || tile.kind == TileKind::Prefab ||
                            tile.kind == TileKind::Circuit ||
                            (tile.kind != TileKind::Other && (!tile.file || tile.file->imported));
        if (i % columns != 0) ImGui::SameLine();

        ImGui::PushID(tile.name.c_str());
        ImGui::BeginGroup();
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", tileSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool chosen = m_selectedContent == tile.name;
        if (ImGui::IsItemClicked()) m_selectedContent = tile.name;

        // Background, picture, then the name underneath.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 max(min.x + tileSize.x, min.y + tileSize.y);
        draw->AddRectFilled(
            min, max,
            ImGui::GetColorU32(
                chosen    ? ImGuiCol_HeaderActive
                : hovered ? ImGuiCol_HeaderHovered
                          : ImGuiCol_FrameBg),
            style.FrameRounding);
        if (chosen) draw->AddRect(min, max, accentColor, style.FrameRounding);
        const float pad = 6.0f * dpi;
        const ImVec2 picMin(min.x + pad, min.y + pad);
        const ImVec2 picMax(min.x + side - pad, min.y + side - pad);
        const ImVec2 centre((picMin.x + picMax.x) * 0.5f, (picMin.y + picMax.y) * 0.5f);
        const float half = (picMax.x - picMin.x) * 0.5f;
        if (tile.texture) {
            // Keep the image's shape inside the square.
            float w = 1.0f, h = 1.0f;
            if (tile.file && tile.file->width && tile.file->height) {
                const float aspect = float(tile.file->width) / float(tile.file->height);
                (aspect >= 1.0f ? h : w) = aspect >= 1.0f ? 1.0f / aspect : aspect;
            }
            draw->AddImageRounded(
                static_cast<ImTextureID>(tile.texture), ImVec2(centre.x - half * w, centre.y - half * h),
                ImVec2(centre.x + half * w, centre.y + half * h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                style.FrameRounding);
        } else {
            const ImU32 iconColor = failed ? errorColor : tile.kind == TileKind::Folder ? dimColor : accentColor;
            drawIcon(draw, tile.kind, tile.name, centre, half, iconColor);
        }
        if (failed) draw->AddText(ImVec2(min.x + pad, min.y + pad), errorColor, "!");
        const std::string label = ellipsize(tile.label, side - pad * 2.0f);
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        draw->AddText(
            ImVec2(min.x + (side - labelWidth) * 0.5f, min.y + side - 2.0f),
            usable || tile.kind == TileKind::Folder ? textColor : dimColor, label.c_str());

        // Details on hover.
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(tile.label.c_str());
            if (tile.file) {
                if (tile.file->width) ImGui::TextDisabled("%u x %u", tile.file->width, tile.file->height);
                ImGui::TextDisabled("%s", humanSize(tile.file->bytes).c_str());
                if (failed)
                    ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "%s", tile.file->error.c_str());
                if (tile.kind == TileKind::Other && tile.path.extension() == ".rcss")
                    ImGui::TextDisabled("Game UI stylesheet; documents using it reload when it's saved");
                else if (tile.kind == TileKind::Other)
                    ImGui::TextDisabled("No importer for this kind of file yet");
                if (tile.kind == TileKind::Model) {
                    if (const auto model = engine().assets().model(tile.file->model)) {
                        ImGui::TextDisabled(
                            "%zu meshes, %zu nodes, %zu materials, %llu triangles", model->data.meshes.size(),
                            model->data.nodes.size(), model->data.materials.size(),
                            static_cast<unsigned long long>(model->data.triangles));
                    }
                }
                for (const std::string& warning : tile.file->warnings) {
                    ImGui::TextColored(gui::semanticColor(gui::currentTheme().warning), "%s", warning.c_str());
                }
            }
            switch (tile.kind) {
            case TileKind::Folder: ImGui::TextDisabled("Double-click to open"); break;
            case TileKind::Scene: ImGui::TextDisabled("Double-click to open"); break;
            case TileKind::Mesh: ImGui::TextDisabled("Drag into the viewport, or double-click to place it"); break;
            case TileKind::Model:
                if (usable) ImGui::TextDisabled("Drag into the viewport, or double-click to place it");
                if (failed) ImGui::TextDisabled("Right-click to retry the import");
                break;
            case TileKind::Texture:
                if (usable) ImGui::TextDisabled("Drag onto an object, or double-click to use it on the selection");
                break;
            case TileKind::Audio: ImGui::TextDisabled("Drag into the viewport to place a source"); break;
            case TileKind::UIDocument:
                ImGui::TextDisabled("Game UI document. Drag into the viewport, or double-click to show it");
                break;
            case TileKind::Prefab:
                ImGui::TextDisabled("Prefab. Drag into the viewport to place a copy, or double-click to edit it");
                break;
            case TileKind::Circuit:
                ImGui::TextDisabled("Circuit. Drag onto an object to run it there, or double-click to edit it");
                break;
            case TileKind::Other: break;
            }
            ImGui::EndTooltip();
        }

        // Drag sources carry the asset name.
        if (usable && tile.kind != TileKind::Folder && tile.kind != TileKind::Scene && ImGui::BeginDragDropSource()) {
            const char* payload = tile.kind == TileKind::Mesh         ? kMeshPayload
                                  : tile.kind == TileKind::Audio      ? kAudioPayload
                                  : tile.kind == TileKind::Model      ? kModelPayload
                                  : tile.kind == TileKind::UIDocument ? kUIDocumentPayload
                                  : tile.kind == TileKind::Prefab     ? kPrefabPayload
                                  : tile.kind == TileKind::Circuit    ? kCircuitPayload
                                                                      : kTexturePayload;
            ImGui::SetDragDropPayload(payload, tile.name.c_str(), tile.name.size() + 1);
            if (tile.texture) {
                ImGui::Image(static_cast<ImTextureID>(tile.texture), ImVec2(48.0f * dpi, 48.0f * dpi));
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(tile.name.c_str());
            ImGui::EndDragDropSource();
        }

        auto useTexture = [&]() {
            bool any = false;
            for (Entity e : selection()) any |= applyTexture(e, tile.name);
            if (!any && selection().empty()) warn("Select an object first, or drag " + tile.name + " onto one");
        };
        auto placeMesh = [&]() {
            const shockwave::Bounds* b = meshBounds(engine().assets().findMesh(tile.name));
            spawnMesh(tile.name, placementPoint() + glm::vec3(0.0f, b ? std::max(-b->min.y, 0.0f) : 0.0f, 0.0f));
        };
        auto placeModel = [&]() {
            const auto model = engine().assets().model(engine().assets().findModel(tile.name));
            spawnModel(tile.name, placementPoint() + glm::vec3(0.0f, model ? -model->data.bounds.min.y : 0.0f, 0.0f));
        };
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            switch (tile.kind) {
            case TileKind::Folder:
                navigateTo = tile.name;
                navigate = true;
                break;
            case TileKind::Scene: requestAction(PendingAction::OpenScene, tile.name); break;
            case TileKind::Mesh: placeMesh(); break;
            case TileKind::Model:
                if (usable) placeModel();
                break;
            case TileKind::Texture:
                if (usable) useTexture();
                break;
            case TileKind::Audio:
                if (usable) previewAudio(tile.name);
                break;
            case TileKind::UIDocument: createUIDocument(tile.name); break;
            case TileKind::Prefab: editPrefab(tile.name); break;
            case TileKind::Circuit: openCircuit(tile.name); break;
            case TileKind::Other: break;
            }
        }
        if (ImGui::BeginPopupContextItem("tile")) {
            switch (tile.kind) {
            case TileKind::Folder:
                if (ImGui::MenuItem("Open")) {
                    navigateTo = tile.name;
                    navigate = true;
                }
                break;
            case TileKind::Scene: {
                if (ImGui::MenuItem("Open")) requestAction(PendingAction::OpenScene, tile.name);
                const std::string rel = m_project.relative(tile.path);
                const bool isStartup = tile.path == m_project.startupScene;
                if (ImGui::MenuItem("Startup scene", nullptr, isStartup, !isStartup)) {
                    m_project.startupScene = tile.path;
                    m_projectEdit.startupScene = tile.path;
                    std::string err;
                    if (shockwave::saveProject(m_project, err)) {
                        log("Startup scene set to " + rel);
                    } else {
                        error("Couldn't save the project: " + err);
                    }
                }
                break;
            }
            case TileKind::Mesh:
                if (ImGui::MenuItem("Place in front of the camera")) placeMesh();
                break;
            case TileKind::Model:
                if (ImGui::MenuItem("Place in front of the camera", nullptr, false, usable)) placeModel();
                break;
            case TileKind::Texture:
                if (ImGui::MenuItem("Use on the selection", nullptr, false, usable && !selection().empty()))
                    useTexture();
                break;
            case TileKind::Audio:
                if (ImGui::MenuItem("Play", nullptr, false, usable)) previewAudio(tile.name);
                if (ImGui::MenuItem("Stop preview")) stopAudioPreview();
                if (ImGui::MenuItem("Place audio source", nullptr, false, usable))
                    spawnAudio(tile.name, placementPoint());
                break;
            case TileKind::UIDocument:
                if (ImGui::MenuItem("Show in the scene")) createUIDocument(tile.name);
                if (ImGui::MenuItem("Open in text editor"))
                    SDL_OpenURL(("file://" + fs::absolute(tile.path).generic_string()).c_str());
                break;
            case TileKind::Prefab:
                if (ImGui::MenuItem("Place in front of the camera")) placePrefab(tile.name, placementPoint());
                if (ImGui::MenuItem("Edit")) editPrefab(tile.name);
                if (ImGui::MenuItem("Open in text editor"))
                    SDL_OpenURL(("file://" + fs::absolute(tile.path).generic_string()).c_str());
                break;
            case TileKind::Circuit:
                if (ImGui::MenuItem("Edit")) openCircuit(tile.name);
                if (ImGui::MenuItem("Run on the selection", nullptr, false, !selection().empty()))
                    attachCircuit(tile.name);
                if (ImGui::MenuItem("Open in text editor"))
                    SDL_OpenURL(("file://" + fs::absolute(tile.path).generic_string()).c_str());
                break;
            case TileKind::Other: break;
            }
            if (failed && ImGui::MenuItem("Retry imports")) m_retryAssetImports = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(tile.name.c_str());
            if (!tile.path.empty() && ImGui::MenuItem("Show in file manager")) {
                openInFileManager(tile.kind == TileKind::Folder ? tile.path : tile.path.parent_path());
            }
            ImGui::EndPopup();
        }
        if (tile.kind == TileKind::Audio) {
            const bool active =
                m_audioPreview && m_previewClip == tile.name && m_audioPreview->isPlaying(m_previewVoice);
            ImGui::BeginDisabled(!usable);
            if (gui::button(active ? "Stop" : "Play", ImVec2(side, 0))) {
                if (active)
                    stopAudioPreview();
                else
                    previewAudio(tile.name);
            }
            ImGui::EndDisabled();
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }

    // Right-click on empty space.
    if (m_contentRoot != ContentRoot::Engine &&
        ImGui::BeginPopupContextWindow(
            "background", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (m_contentRoot == ContentRoot::Assets && ImGui::MenuItem("Import...")) showImportDialog(folderPath);
        if (m_contentRoot == ContentRoot::Assets && ImGui::MenuItem("New circuit")) newCircuit(m_contentFolder);
        if (ImGui::MenuItem("Show in file manager")) openInFileManager(folderPath);
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    if (navigate) {
        m_contentFolder = navigateTo;
        m_contentFilter.clear();
    }
}

}   // namespace editor
