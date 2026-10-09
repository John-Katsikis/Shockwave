// The Assets panel: every mesh and texture the scene can use, as tiles you can drag onto
// the viewport, the hierarchy or the inspector. Images in the project's assets/ folder
// are imported automatically and re-imported when saved.
#include "Editor.h"

#include <shockwave/Gui.h>

#include <SDL3/SDL.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace editor {

namespace gui = shockwave::gui;
using shockwave::AssetFolder;
using shockwave::Entity;
using shockwave::MeshRenderer;

namespace {

enum TileKind { MeshTiles, TextureTiles, OtherTiles };

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

// The last part of a path, which is what fits under a tile.
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

} // namespace

void Editor::drawAssetBrowser() {
    if (!ImGui::Begin("Assets", &m_showAssets)) {
        ImGui::End();
        return;
    }
    const float dpi = ImGui::GetStyle().FontScaleDpi;

    const float folderWidth = gui::buttonWidth("Open folder");
    const float tileWidth = 120.0f * dpi;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    const bool wide = available >= 180.0f * dpi + tileWidth + folderWidth + gap * 2.0f;
    ImGui::SetNextItemWidth(wide ? available - tileWidth - folderWidth - gap * 2.0f : available);
    ImGui::InputTextWithHint("##filter", "Search assets", &m_assetFilter);
    if (wide) ImGui::SameLine();
    ImGui::SetNextItemWidth(std::min(tileWidth, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##size", &m_tileSize, 56.0f, 160.0f, "Size %.0f");
    ImGui::SetItemTooltip("Asset thumbnail size");
    gui::alignTrailing(folderWidth);
    if (gui::button("Open folder")) {
        std::error_code ec;
        std::filesystem::create_directories(m_assetFolder->root(), ec);
        const std::string url = "file://" + std::filesystem::absolute(m_assetFolder->root()).generic_string();
        if (!SDL_OpenURL(url.c_str())) log(std::string("Couldn't open the folder: ") + SDL_GetError());
    }
    ImGui::SetItemTooltip("Show %s in your file manager. Images saved there are imported straight away.",
                          m_assetFolder->root().generic_string().c_str());
    ImGui::Separator();

    ImGui::BeginChild("tiles");
    drawAssetTiles("Meshes", MeshTiles);
    drawAssetTiles("Textures", TextureTiles);
    drawAssetTiles("Other files", OtherTiles);
    ImGui::EndChild();
    ImGui::End();
}

void Editor::drawAssetTiles(const char* section, int kind) {
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    const std::string filter = lower(m_assetFilter);

    struct Tile {
        std::string name;
        uint64_t texture = 0;               // thumbnail
        const MeshAsset* mesh = nullptr;    // drawn as an outline instead
        const AssetFolder::File* file = nullptr;
    };
    std::vector<Tile> tiles;
    if (kind == MeshTiles) {
        for (const auto& [name, handle] : engine().assets().meshes()) tiles.push_back({name, 0, findMesh(handle), nullptr});
    } else if (kind == TextureTiles) {
        for (const auto& [name, handle] : engine().assets().textures()) {
            tiles.push_back({name, engine().renderer().guiTexture(handle), nullptr, m_assetFolder->find(name)});
        }
        // Images that failed to import get a tile too, to show why.
        for (const AssetFolder::File& f : m_assetFolder->files()) {
            if (f.kind == AssetFolder::Kind::Texture && !f.imported) tiles.push_back({f.name, 0, nullptr, &f});
        }
    } else {
        for (const AssetFolder::File& f : m_assetFolder->files()) {
            if (f.kind == AssetFolder::Kind::Other) tiles.push_back({f.name, 0, nullptr, &f});
        }
    }
    std::erase_if(tiles, [&](const Tile& t) { return !filter.empty() && lower(t.name).find(filter) == std::string::npos; });
    if (kind == OtherTiles && tiles.empty()) return;

    char header[96];
    std::snprintf(header, sizeof(header), "%s (%zu)###%s", section, tiles.size(), section);
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (tiles.empty()) {
        ImGui::TextDisabled(kind == TextureTiles ? "No textures match" : "No meshes match");
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float side = m_tileSize * dpi;
    const float labelHeight = ImGui::GetTextLineHeight() + 6.0f;
    const ImVec2 tileSize(side, side + labelHeight);
    const float spacing = style.ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (side + spacing)));
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 errorColor = gui::semanticColorU32(gui::currentTheme().error);

    for (size_t i = 0; i < tiles.size(); ++i) {
        const Tile& tile = tiles[i];
        const bool failed = tile.file && !tile.file->error.empty();
        const bool usable = kind != OtherTiles && (!tile.file || tile.file->imported);
        if (i % columns != 0) ImGui::SameLine();

        ImGui::PushID(section);
        ImGui::PushID(tile.name.c_str());
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", tileSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool chosen = m_selectedAsset == tile.name;
        if (ImGui::IsItemClicked()) m_selectedAsset = tile.name;

        // Background, picture, then the name underneath.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 max(min.x + tileSize.x, min.y + tileSize.y);
        draw->AddRectFilled(min, max,
                            ImGui::GetColorU32(chosen ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg),
                            style.FrameRounding);
        draw->AddRect(min, max, chosen ? ImGui::GetColorU32(ImGuiCol_NavCursor) : ImGui::GetColorU32(ImGuiCol_Border), 0.0f, 0, style.FrameBorderSize);
        const float pad = 6.0f * dpi;
        const ImVec2 picMin(min.x + pad, min.y + pad);
        const ImVec2 picMax(min.x + side - pad, min.y + side - pad);
        const ImVec2 center((picMin.x + picMax.x) * 0.5f, (picMin.y + picMax.y) * 0.5f);
        const float half = (picMax.x - picMin.x) * 0.5f;

        if (tile.texture) {
            // Keep the image's shape inside the square.
            float w = 1.0f, h = 1.0f;
            if (tile.file && tile.file->width && tile.file->height) {
                const float aspect = float(tile.file->width) / float(tile.file->height);
                (aspect >= 1.0f ? h : w) = aspect >= 1.0f ? 1.0f / aspect : aspect;
            }
            draw->AddImageRounded(static_cast<ImTextureID>(tile.texture), ImVec2(center.x - half * w, center.y - half * h),
                                  ImVec2(center.x + half * w, center.y + half * h), ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32_WHITE, style.FrameRounding);
        } else if (tile.mesh) {
            // A line drawing of the shape.
            const ImU32 c = ImGui::ColorConvertFloat4ToU32(gui::accent());
            const float r = half * 0.62f;
            switch (tile.mesh->shape) {
            case Shape::Sphere:
                draw->AddCircle(center, r, c, 0, 2.0f);
                draw->AddEllipse(center, ImVec2(r, r * 0.35f), c, 0.0f, 0, 1.2f);
                break;
            case Shape::Plane: {
                const ImVec2 q[4] = {{center.x - r, center.y + r * 0.35f}, {center.x - r * 0.4f, center.y - r * 0.35f},
                                     {center.x + r, center.y - r * 0.35f}, {center.x + r * 0.4f, center.y + r * 0.35f}};
                draw->AddPolyline(q, 4, c, ImDrawFlags_Closed, 2.0f);
                break;
            }
            case Shape::Box: {
                const float d = r * 0.45f;
                draw->AddRect(ImVec2(center.x - r, center.y - r + d), ImVec2(center.x + r - d, center.y + r), c, 0.0f, 0, 2.0f);
                draw->AddLine(ImVec2(center.x - r, center.y - r + d), ImVec2(center.x - r + d, center.y - r), c, 2.0f);
                draw->AddLine(ImVec2(center.x + r - d, center.y - r + d), ImVec2(center.x + r, center.y - r), c, 2.0f);
                draw->AddLine(ImVec2(center.x + r - d, center.y + r), ImVec2(center.x + r, center.y + r - d), c, 2.0f);
                draw->AddLine(ImVec2(center.x - r + d, center.y - r), ImVec2(center.x + r, center.y - r), c, 2.0f);
                draw->AddLine(ImVec2(center.x + r, center.y - r), ImVec2(center.x + r, center.y + r - d), c, 2.0f);
                break;
            }
            }
        } else {
            // No picture: the file's extension in big letters.
            std::string ext = std::filesystem::path(tile.name).extension().string();
            if (!ext.empty()) ext.erase(0, 1);
            if (ext.empty()) ext = kind == MeshTiles ? "mesh" : "file";
            const ImVec2 textSize = ImGui::CalcTextSize(ext.c_str());
            draw->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                          failed ? errorColor : dimColor, ext.c_str());
        }
        if (failed) draw->AddText(ImVec2(min.x + pad, min.y + pad), errorColor, "!");

        const std::string label = ellipsize(leafName(tile.name), side - pad * 2.0f);
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        draw->AddText(ImVec2(min.x + (side - labelWidth) * 0.5f, min.y + side - 2.0f), usable ? textColor : dimColor,
                      label.c_str());

        // Details on hover.
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(tile.name.c_str());
            if (tile.file) {
                if (tile.file->width) ImGui::TextDisabled("%u x %u", tile.file->width, tile.file->height);
                ImGui::TextDisabled("%s, %s", humanSize(tile.file->bytes).c_str(),
                                    tile.file->path.generic_string().c_str());
                if (failed) ImGui::TextColored(gui::semanticColor(gui::currentTheme().error), "%s", tile.file->error.c_str());
                if (kind == OtherTiles) ImGui::TextDisabled("No importer for this kind of file yet");
            } else {
                ImGui::TextDisabled("Built in");
            }
            if (usable) {
                ImGui::TextDisabled(kind == MeshTiles ? "Drag onto the viewport to place it, or double-click"
                                                      : "Drag onto an object, or double-click to use on the selection");
            }
            ImGui::EndTooltip();
        }

        if (usable) {
            const char* payloadType = kind == MeshTiles ? kMeshPayload : kTexturePayload;
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(payloadType, tile.name.c_str(), tile.name.size() + 1);
                if (tile.texture) {
                    ImGui::Image(static_cast<ImTextureID>(tile.texture), ImVec2(48.0f * dpi, 48.0f * dpi));
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(tile.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (kind == MeshTiles) {
                    const glm::vec3 ahead = m_camera.position + m_camera.forward() * 6.0f;
                    spawnMesh(tile.name, {ahead.x, tile.mesh && tile.mesh->shape == Shape::Plane ? 0.01f : 0.5f, ahead.z});
                } else if (selected().valid()) {
                    applyTexture(selected(), tile.name);
                } else {
                    log("Select an object first, or drag " + tile.name + " onto one");
                }
            }
            if (ImGui::BeginPopupContextItem("tile")) {
                if (kind == MeshTiles && ImGui::MenuItem("Place in front of the camera")) {
                    const glm::vec3 ahead = m_camera.position + m_camera.forward() * 6.0f;
                    spawnMesh(tile.name, {ahead.x, 0.5f, ahead.z});
                }
                if (kind == TextureTiles && ImGui::MenuItem("Use on the selection", nullptr, false, selected().valid())) {
                    applyTexture(selected(), tile.name);
                }
                if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(tile.name.c_str());
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
        ImGui::PopID();
    }
}

} // namespace editor
