#include "GraphicsGuide.h"

#include <Core/Log.h>
#include <UI/Gui.h>

#include <SDL3/SDL.h>

#include <array>
#include <string>

namespace editor {
namespace {
SW_LOG_CATEGORY(LogDocumentation);

struct Chapter {
    const char* title;
    const char* file;
    const char* description;
};

constexpr std::array chapters{
    Chapter{
        "Start here: roadmap and batch order", "Graphics/README.md",
        "Scope, current capabilities, batch dependencies, and the 1v1 to 32v32 target."},
    Chapter{
        "Foundation: rendering batches G00-G13", "GraphicsImplementation.md",
        "G00-G13: rendering contracts, lightmaps/probes, anti-aliasing, AO, decals and performance."},
    Chapter{
        "1. Graphics settings and texture residency", "Graphics/01-SettingsAndTextures.md",
        "Player controls, genuine off paths, texture quality, memory budgets, bloom and shadows."},
    Chapter{
        "2. Materials and decals", "Graphics/02-MaterialsAndDecals.md",
        "Material instances, UVs, samplers, detail normals, layers and surface authoring."},
    Chapter{
        "3. Particles and transparency", "Graphics/03-ParticlesAndTransparency.md",
        "Soft particles, distortion, lighting, sorting and Effekseer compatibility."},
    Chapter{
        "4. Color and atmosphere", "Graphics/04-ColorAndAtmosphere.md",
        "White balance, grading, exposure, volumes and fog integration."},
    Chapter{
        "5. Character and specialized shading", "Graphics/05-CharacterShading.md",
        "Clearcoat, glass, skin, hair and eye shading with scalable fallbacks."},
    Chapter{
        "6. Verification and delivery", "Graphics/06-Verification.md",
        "Editor/CLI routes, test scenes, packaging, performance evidence and completion gates."},
    Chapter{
        "7. Mathematical contracts", "Graphics/07-MathematicalContracts.md",
        "Derivations, numerical edge cases, reference values and limits of the proofs."},
};

// Encode UTF-8 bytes, including spaces, '#' and '%', without invoking a shell.
std::string fileUrl(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    std::string result = "file://";
    if (utf8.empty() || utf8.front() != u8'/') result += '/';
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto value : utf8) {
        const auto c = static_cast<unsigned char>(value);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == ':' ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}
}   // namespace

std::filesystem::path graphicsGuidePath() {
    const auto source = std::filesystem::path(SHOCKWAVE_SOURCE_DIR) / "docs/Graphics/README.md";
    std::error_code ec;
    if (std::filesystem::is_regular_file(source, ec)) return source;
    if (const char* base = SDL_GetBasePath()) {
        const auto bundled = std::filesystem::path(base) / "docs/Graphics/README.md";
        if (std::filesystem::is_regular_file(bundled, ec)) return bundled;
    }
    return {};
}

void drawGraphicsGuide(bool& open) {
    namespace gui = shockwave::gui;
    ImGui::SetNextWindowSize(ImVec2(760, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics implementation guide", &open)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Implementation specifications. Planned features are not necessarily implemented.");
    ImGui::TextWrapped("Select a chapter, then open its Markdown in your associated application or copy its path.");
    ImGui::Separator();
    static int selected = 0;
    static std::string lastError;
    const auto index = graphicsGuidePath();
    if (index.empty()) {
        ImGui::TextWrapped("Documentation is missing. Rebuild the editor target to copy docs next to the executable.");
        ImGui::End();
        return;
    }
    if (ImGui::BeginChild(
            "chapters", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 11), ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        for (size_t i = 0; i < chapters.size(); ++i) {
            if (ImGui::Selectable(chapters[i].title, selected == static_cast<int>(i))) {
                selected = static_cast<int>(i);
                lastError.clear();
            }
            ImGui::SetItemTooltip("%s", chapters[i].description);
        }
    }
    ImGui::EndChild();
    const auto& chapter = chapters[static_cast<size_t>(selected)];
    ImGui::TextWrapped("%s", chapter.description);
    const auto path = index.parent_path().parent_path() / chapter.file;
    std::error_code ec;
    const bool exists = std::filesystem::is_regular_file(path, ec);
    ImGui::BeginDisabled(!exists);
    if (gui::button("Open chapter")) {
        lastError.clear();
        if (!SDL_OpenURL(fileUrl(path).c_str())) {
            lastError = std::string("Couldn't open the document: ") + SDL_GetError();
            SW_LOG(LogDocumentation, Error, "{} ({})", lastError, path.string());
        }
    }
    gui::toolbarNext(ImGui::CalcTextSize("Copy path").x + ImGui::GetStyle().FramePadding.x * 2);
    if (gui::button("Copy path")) ImGui::SetClipboardText(path.string().c_str());
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", path.string().c_str());
    if (!exists) ImGui::TextWrapped("This chapter is missing. Refresh the documentation by rebuilding the editor.");
    if (!lastError.empty()) ImGui::TextWrapped("%s", lastError.c_str());
    ImGui::End();
}
}   // namespace editor
