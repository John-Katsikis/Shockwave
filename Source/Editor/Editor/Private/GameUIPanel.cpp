// The game UI in the editor: UIDocuments are previewed in the viewport while editing and
// take mouse and keyboard input during Play. The Inspector shows a document's problems,
// the Stats window what the UI costs. RML and RCSS errors reach the Console through the
// LogUI category. See docs/GameUI.md.
#include "Editor.h"

#include <GameUI/GameUI.h>
#include <UI/Gui.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;

namespace {

// What Create > UI document writes when the file isn't there yet.
constexpr const char* kStarterDocument = R"(<rml>
<head>
    <title>%TITLE%</title>
    <link type="text/rcss" href="%STYLE%"/>
</head>
<!-- {{name}} shows a value game code sets with game.ui().set("name", value). -->
<body data-model="game">
    <div class="panel">
        <h1 id="title">%TITLE%</h1>
        <p>Score: {{score}}</p>
        <!-- onclick names a command: game.ui().on("start", ...) reacts to it. -->
        <button onclick="start">Start</button>
    </div>
</body>
</rml>
)";

constexpr const char* kStarterStyle = R"(/* dp units scale with the display's DPI. */
body {
    font-family: "Noto Sans";
    font-size: 18dp;
    color: #f2f4f8;
    width: 100%;
    height: 100%;
    pointer-events: none;   /* clicks reach the game, except on .panel */
}
.panel {
    position: absolute;
    left: 24dp;
    top: 24dp;
    padding: 12dp 16dp;
    background-color: #12161ccc;
    pointer-events: auto;
}
h1 { display: block; font-size: 22dp; font-weight: bold; margin-bottom: 6dp; }
p { display: block; margin-bottom: 10dp; }
button { display: inline-block; padding: 6dp 14dp; background-color: #3d7bd9; cursor: pointer; }
button:hover { background-color: #5b93e6; }
)";

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

}   // namespace

void Editor::updateGameUI(double dt) {
    if (!m_gameUI || !m_hasProject) return;
    // Input only while playing, and only when the viewport has the mouse or focus, so
    // typing in a panel never reaches the game.
    m_gameUI->setInputEnabled(playState() == PlayState::Playing && m_viewportActive);
    m_gameUI->update(m_world, dt);
}

void Editor::feedGameUIPointer(const ImVec2& pos, const ImVec2& size, bool hovered) {
    m_viewportActive = hovered || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // The panel is drawn in points; the UI is laid out in the viewport's pixels.
    const glm::uvec2 pixels = engine().renderer().uiOutputSize();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (!hovered || size.x <= 0.0f || size.y <= 0.0f) {
        m_gameUI->setPointer(std::nullopt);
        return;
    }
    m_gameUI->setPointer(glm::vec2(
        (mouse.x - pos.x) * static_cast<float>(pixels.x) / size.x,
        (mouse.y - pos.y) * static_cast<float>(pixels.y) / size.y));
}

std::string Editor::newUIDocumentPath() const {
    std::error_code ec;
    for (int i = 1;; ++i) {
        const std::string name = "UI/" + std::string(i == 1 ? "hud" : "hud" + std::to_string(i)) + ".rml";
        if (!fs::exists(m_project.assetFolder / name, ec)) return name;
    }
}

Entity Editor::createUIDocument(const std::string& document) {
    const fs::path file = m_project.assetFolder / document;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        fs::create_directories(file.parent_path(), ec);
        const fs::path style = fs::path(file).replace_extension(".rcss");
        const std::string title = file.stem().string();
        std::ofstream(file) << replaceAll(
            replaceAll(kStarterDocument, "%TITLE%", title), "%STYLE%", style.filename().string());
        if (!fs::exists(style, ec)) std::ofstream(style) << kStarterStyle;
        if (!fs::exists(file, ec)) {
            error("Couldn't write " + m_project.relative(file));
            return {};
        }
        log("Wrote a starter document, " + m_project.relative(file) +
            ", and its stylesheet. Edit them in any text "
            "editor; the viewport updates when you save.");
    }
    pushUndo(m_world);
    const Entity e = m_world.create();
    std::string name = fs::path(document).stem().string();
    if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    m_world.add<shockwave::Name>(e, {uniqueName(name.empty() ? "UI" : name)});
    m_world.add<shockwave::UIDocument>(e, {document, true, 0, false});
    select(e);
    log("Created " + displayName(e) + " showing " + document);
    return e;
}

void Editor::drawUIDocumentNotes(Entity e) {
    const auto* component = m_world.get<shockwave::UIDocument>(e);
    if (!component) return;
    const auto& problems = m_gameUI->problems(e);
    for (const auto& problem : problems) {
        ImGui::PushStyleColor(ImGuiCol_Text, gui::semanticColor(gui::currentTheme().error));
        ImGui::TextWrapped("%s", problem.message.c_str());
        ImGui::PopStyleColor();
    }
    const fs::path file = m_project.assetFolder / component->document;
    std::error_code ec;
    const bool exists = !component->document.empty() && fs::is_regular_file(file, ec);
    if (gui::button("Reload")) {
        m_gameUI->reloadAll();
        log("Reloaded the game UI");
    }
    ImGui::SetItemTooltip("Documents reload by themselves when their files change");
    gui::toolbarNext(gui::buttonWidth("Open file"));
    ImGui::BeginDisabled(!exists);
    if (gui::button("Open file")) SDL_OpenURL(("file://" + fs::absolute(file).generic_string()).c_str());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Open the .rml in your text editor");
    if (!component->document.empty() && !exists && fs::path(component->document).extension() == ".rml") {
        gui::toolbarNext(gui::buttonWidth("Create file"));
        if (gui::button("Create file")) {
            fs::create_directories(file.parent_path(), ec);
            std::ofstream(file) << replaceAll(
                replaceAll(kStarterDocument, "%TITLE%", file.stem().string()), "%STYLE%",
                fs::path(file).replace_extension(".rcss").filename().string());
            const fs::path style = fs::path(file).replace_extension(".rcss");
            if (!fs::exists(style, ec)) std::ofstream(style) << kStarterStyle;
            log("Wrote " + m_project.relative(file));
        }
    }
    if (problems.empty() && exists)
        ImGui::TextDisabled(
            playing() ? "Live: takes the mouse and keys while the viewport is focused" : "Previewed in the viewport");
}

void Editor::drawGameUIStats() {
    if (!m_gameUI) return;
    const shockwave::UIStats s = m_gameUI->stats();
    ImGui::SeparatorText("Game UI");
    if (s.documents == 0) {
        ImGui::TextDisabled("No UI documents. Create > UI document adds one.");
        return;
    }
    if (gui::beginPropertyGrid("gameui")) {
        gui::propertyLabel("Documents");
        ImGui::Text("%u shown of %u", s.visible, s.documents);
        gui::propertyLabel("Draw list");
        ImGui::Text("%u draws, %u vertices", s.drawCommands, s.vertices);
        gui::propertyLabel("Textures");
        ImGui::Text("%u (glyph atlases, images)", s.textures);
        gui::propertyLabel("CPU");
        ImGui::Text("%.3f ms update, %.3f ms layout", static_cast<double>(s.updateMs), static_cast<double>(s.layoutMs));
        const glm::uvec2 size = m_gameUI->size();
        gui::propertyLabel("Laid out at");
        ImGui::Text("%u x %u px (native)", size.x, size.y);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("GPU: the \"ui\" pass under GPU time per pass");
}

}   // namespace editor
