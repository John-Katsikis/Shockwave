// The frame around the panels: menu bar, toolbar, status bar, dock layout, and the
// smaller windows (preferences, project settings, stats, controls, about, prompts).
#include "Editor.h"
#include "GpuPanel.h"

#include <Physics/Physics.h>
#include <UI/Gui.h>

#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <set>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::Entity;
using shockwave::kNullEntity;

namespace {

// Theme files next to the working folder and the executable.
std::vector<std::string> findThemes() {
    std::set<std::string> found;
    for (const fs::path& dir : {fs::path("themes"), fs::path(SDL_GetBasePath()) / "themes"}) {
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".swtheme") found.insert("themes/" + entry.path().filename().string());
        }
    }
    return {found.begin(), found.end()};
}

std::string fileTime(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    if (ec) return "an unknown time";
    const std::time_t t =
        std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(time));
    char text[64];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", std::localtime(&t));
    return text;
}

}   // namespace

// ---- Menu bar -----------------------------------------------------------------------------

void Editor::drawCreateMenuItems(Entity parent) {
    if (ImGui::MenuItem("Empty entity")) createEntity(CreateKind::Empty, parent);
    ImGui::SeparatorText("3D objects");
    if (ImGui::MenuItem("Cube")) createEntity(CreateKind::Cube, parent);
    if (ImGui::MenuItem("Sphere")) createEntity(CreateKind::Sphere, parent);
    if (ImGui::MenuItem("Plane")) createEntity(CreateKind::Plane, parent);
    ImGui::SeparatorText("Scene");
    if (ImGui::MenuItem("Camera")) createEntity(CreateKind::Camera, parent);
    if (ImGui::BeginMenu("Light")) {
        if (ImGui::MenuItem("Directional (sun)")) createEntity(CreateKind::DirectionalLight, parent);
        if (ImGui::MenuItem("Point")) createEntity(CreateKind::PointLight, parent);
        if (ImGui::MenuItem("Spot")) createEntity(CreateKind::SpotLight, parent);
        ImGui::EndMenu();
    }
    ImGui::SetItemTooltip("Placed where the viewport is looking from");
    if (ImGui::MenuItem("Environment", nullptr, false, !parent.valid()))
        createEntity(CreateKind::Environment, kNullEntity);
    ImGui::SeparatorText("UI");
    if (ImGui::MenuItem("UI document")) createUIDocument(newUIDocumentPath());
    ImGui::SetItemTooltip("A HUD or menu from an .rml file in Content/UI; writes a starter one");
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New project...")) requestAction(PendingAction::CloseProject);
        if (ImGui::MenuItem("Open project...")) showOpenProjectDialog();
        if (ImGui::BeginMenu("Recent projects", !m_prefs.recentProjects.empty())) {
            for (const std::string& path : m_prefs.recentProjects) {
                if (ImGui::MenuItem(path.c_str(), nullptr, m_hasProject && path == m_project.file.string())) {
                    requestAction(PendingAction::OpenProject, path);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Close project", nullptr, false, m_hasProject)) requestAction(PendingAction::CloseProject);
        ImGui::Separator();
        if (ImGui::BeginMenu("New scene", m_hasProject)) {
            if (ImGui::MenuItem("Basic", "Ctrl+N")) requestAction(PendingAction::NewBasicScene);
            ImGui::SetItemTooltip("Sky, camera and ground");
            if (ImGui::MenuItem("Empty")) requestAction(PendingAction::NewEmptyScene);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Open scene...", "Ctrl+O", false, m_hasProject))
            requestAction(PendingAction::OpenSceneDialog);
        // Recent scenes from this project only.
        std::vector<std::string> recent;
        if (m_hasProject) {
            for (const std::string& path : m_prefs.recentScenes) {
                if (!fs::path(m_project.relative(path)).is_absolute()) recent.push_back(path);
            }
        }
        if (ImGui::BeginMenu("Recent scenes", !recent.empty())) {
            for (const std::string& path : recent) {
                if (ImGui::MenuItem(m_project.relative(path).c_str())) requestAction(PendingAction::OpenScene, path);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save scene", "Ctrl+S", false, m_hasProject && !playing())) saveScene();
        if (ImGui::MenuItem("Save scene as...", "Ctrl+Shift+S", false, m_hasProject && !playing()))
            showSaveSceneDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Project settings...", nullptr, false, m_hasProject)) {
            m_projectEdit = m_project;
            m_showProjectSettings = true;
        }
        if (ImGui::MenuItem("Preferences...", "Ctrl+,")) m_showPreferences = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) requestAction(PendingAction::Quit);
        ImGui::EndMenu();
    }

    if (m_hasProject) {
        if (ImGui::BeginMenu("Edit")) {
            const bool any = !selection().empty();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undo.empty() && !playing())) undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redo.empty() && !playing())) redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, any)) duplicateSelected();
            if (ImGui::MenuItem("Delete", "Del", false, any)) deleteSelected();
            if (ImGui::MenuItem("Rename", "F2", false, selection().size() == 1)) beginRename(selected());
            if (ImGui::MenuItem("Group", "Ctrl+G", false, any)) groupSelected();
            ImGui::Separator();
            if (ImGui::MenuItem("Select all", "Ctrl+A")) selectAll();
            if (ImGui::MenuItem("Deselect", "Esc", false, any)) select(kNullEntity);
            ImGui::Separator();
            if (ImGui::MenuItem("Focus", "F", false, any)) focusSelected();
            if (ImGui::MenuItem("Move to view", "Ctrl+Shift+F", false, selection().size() == 1)) alignSelectedToView();
            ImGui::SetItemTooltip("Put the selection where the viewport camera is: handy for setting up a Camera");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Create")) {
            drawCreateMenuItems(kNullEntity);
            ImGui::Separator();
            if (ImGui::BeginMenu("Child of selection", selection().size() == 1)) {
                drawCreateMenuItems(selected());
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Play")) {
            if (ImGui::MenuItem(
                    playState() == PlayState::Playing ? "Stop" : "Play", "Ctrl+P", false, m_gameModuleMatches)) {
                playState() == PlayState::Playing ? stop() : play();
            }
            if (ImGui::MenuItem("Pause", "Ctrl+Shift+P", playState() == PlayState::Paused, playing())) pause();
            if (ImGui::MenuItem("Stop", nullptr, false, playing())) stop();
            ImGui::Separator();
            if (ImGui::MenuItem("Play from scene camera", nullptr, &m_prefs.playFromSceneCamera)) m_prefsDirty = true;
            ImGui::MenuItem("Look through scene camera", "F8", &m_viewThroughCamera);
            ImGui::EndMenu();
        }
        drawBuildMenu();
        if (ImGui::BeginMenu("Window")) {
            ImGui::MenuItem("Hierarchy", nullptr, &m_showHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
            ImGui::MenuItem("Content", nullptr, &m_showContent);
            ImGui::MenuItem("Console", nullptr, &m_showConsole);
            ImGui::MenuItem("Stats", nullptr, &m_showStats);
            ImGui::MenuItem("Input", nullptr, &m_showInput);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) m_resetLayout = true;
            ImGui::EndMenu();
        }
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Controls", "F1", &m_showControls);
        ImGui::MenuItem("About Shockwave", nullptr, &m_showAbout);
        ImGui::Separator();
        if (ImGui::BeginMenu("Debug")) {
            ImGui::MenuItem("Dear ImGui demo", nullptr, &m_showDemo);
            ImGui::MenuItem("Dear ImGui metrics", nullptr, &m_showMetrics);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    // The project on the right.
    if (m_hasProject) {
        const float width = ImGui::CalcTextSize(m_project.name.c_str()).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - width - ImGui::GetStyle().WindowPadding.x * 2.0f);
        ImGui::TextDisabled("%s", m_project.name.c_str());
        ImGui::SetItemTooltip("%s", m_project.file.string().c_str());
    }
    ImGui::EndMainMenuBar();
}

// ---- Command bar --------------------------------------------------------------------------
// Application-wide actions (docs/UI.md): scene context on the left, playback in the
// middle, the mode on the right.

void Editor::drawCommandBar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginViewportSideBar("##WorkspaceCommands", ImGui::GetMainViewport(), ImGuiDir_Up, height, flags)) {
        const char* playLabel = playState() == PlayState::Paused ? "Resume" : "Play";
        const float transportWidth = gui::controlGroupWidth({playLabel, "Pause", "Stop"});
        const float centerX = (ImGui::GetWindowWidth() - transportWidth) * 0.5f;
        const float rowY = ImGui::GetCursorPosY();

        // Scene: name, save, undo and redo, as long as they fit left of the transport.
        const std::string scene = sceneDisplayName() + (m_dirty ? " *" : "");
        const float sceneWidth = ImGui::CalcTextSize("SCENE").x + ImGui::CalcTextSize(scene.c_str()).x +
                                 gui::controlGroupWidth({"Save", "Undo", "Redo"}) + style.ItemSpacing.x * 6.0f;
        if (sceneWidth + style.WindowPadding.x < centerX) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("SCENE");
            ImGui::SameLine();
            ImGui::TextUnformatted(scene.c_str());
            ImGui::SetItemTooltip(
                "%s", m_scenePath.empty() ? "Not saved yet" : m_project.relative(m_scenePath).c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(playing() || (!m_dirty && !m_scenePath.empty()));
            if (gui::button("Save")) saveScene();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Save scene (Ctrl+S)");
            ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
            ImGui::BeginDisabled(m_undo.empty() || playing());
            if (gui::button("Undo")) undo();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Undo (Ctrl+Z)");
            ImGui::SameLine();
            ImGui::BeginDisabled(m_redo.empty() || playing());
            if (gui::button("Redo")) redo();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Redo (Ctrl+Y)");
        }

        ImGui::SetCursorPos(ImVec2(std::max(centerX, style.WindowPadding.x), rowY));
        ImGui::BeginDisabled(!m_gameModuleMatches);
        if (gui::toolButton(
                playLabel, playState() == PlayState::Playing,
                "Run the project's gameplay and physics; resumes when paused (Ctrl+P)")) {
            playState() == PlayState::Playing ? stop() : play();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!playing());
        if (gui::toolButton("Pause", playState() == PlayState::Paused, "Pause / resume (Ctrl+Shift+P)")) pause();
        ImGui::SameLine();
        if (gui::toolButton("Stop", false, "Stop and restore the scene as it was before Play (Ctrl+P)")) stop();
        ImGui::EndDisabled();

        const char* state = !m_gameModuleMatches                ? "PLAY UNAVAILABLE"
                            : playState() == PlayState::Playing ? "SIMULATION RUNNING"
                            : playState() == PlayState::Paused  ? "SIMULATION PAUSED"
                                                                : "EDIT MODE";
        const float stateWidth = ImGui::CalcTextSize(state).x;
        const float right = ImGui::GetWindowWidth() - style.WindowPadding.x;
        if (right - stateWidth > centerX + transportWidth + style.ItemSpacing.x * 2.0f) {
            ImGui::SetCursorPos(ImVec2(right - stateWidth, rowY));
            ImGui::AlignTextToFramePadding();
            const gui::Theme& theme = gui::currentTheme();
            const ImVec4 color =
                !m_gameModuleMatches ? gui::semanticColor(theme.warning)
                : playing() ? gui::semanticColor(playState() == PlayState::Paused ? theme.warning : theme.accent)
                            : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            ImGui::TextColored(color, "%s", state);
            if (!m_gameModuleMatches) {
                ImGui::SetItemTooltip(
                    "The project's game module \"%s\" isn't built. Build > Build and reload game code builds it",
                    m_project.gameModule.c_str());
            }
        }
    }
    ImGui::End();
}

// ---- Status bar ---------------------------------------------------------------------------

void Editor::drawStatusBar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
        if (ImGui::BeginMenuBar()) {
            // Play state, when there is one.
            if (playing()) {
                const bool paused = playState() == PlayState::Paused;
                ImGui::TextColored(
                    paused ? gui::semanticColor(gui::currentTheme().warning)
                           : gui::semanticColor(gui::currentTheme().accent),
                    paused ? "PAUSED" : "PLAYING");
                ImGui::SameLine(0.0f, 16.0f);
            }
            // The scene and whether it's saved.
            ImGui::TextUnformatted(
                m_project.relative(m_scenePath.empty() ? fs::path("Untitled") : m_scenePath).c_str());
            ImGui::SameLine();
            if (m_dirty) {
                ImGui::TextColored(gui::semanticColor(gui::currentTheme().warning), "unsaved changes");
            } else {
                ImGui::TextDisabled("saved");
            }
            // The latest console message; click for the console.
            if (!m_log.empty()) {
                const LogEntry& last = m_log.back();
                ImGui::SameLine(0.0f, 24.0f);
                const ImVec4 color = last.level == LogLevel::Error     ? gui::semanticColor(gui::currentTheme().error)
                                     : last.level == LogLevel::Warning ? gui::semanticColor(gui::currentTheme().warning)
                                                                       : style.Colors[ImGuiCol_TextDisabled];
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::Selectable(
                        last.text.c_str(), false, ImGuiSelectableFlags_None,
                        ImVec2(std::max(ImGui::GetContentRegionAvail().x - 320.0f * style.FontScaleDpi, 50.0f), 0))) {
                    m_showConsole = true;
                    m_focusConsole = true;
                }
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Open the console");
            }
            // Selection, scene size and frame time on the right.
            char right[128];
            const double fps = engine().fps();
            const size_t count = selection().size();
            std::snprintf(
                right, sizeof(right), "%s%zu entities   %.0f fps  %.1f ms",
                count ? (std::to_string(count) + " selected   ").c_str() : "", m_world.entityCount(), fps,
                fps > 0.0 ? 1000.0 / fps : 0.0);
            const float width = ImGui::CalcTextSize(right).x;
            ImGui::SameLine(std::max(
                ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowWidth() - width - style.WindowPadding.x * 2.0f));
            ImGui::TextDisabled("%s", right);
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// ---- Dock layout --------------------------------------------------------------------------

void Editor::drawDockspace() {
    const ImGuiID dockspace = ImGui::GetID("ShockwaveDockspace");
    if (m_resetLayout || !ImGui::DockBuilderGetNode(dockspace)) {
        buildDefaultLayout(dockspace);
        m_showHierarchy = m_showInspector = m_showContent = m_showConsole = true;
        m_resetLayout = false;
    }
    // Panels close from their own tab; one close button per panel is enough.
    ImGui::DockSpaceOverViewport(
        dockspace, ImGui::GetMainViewport(), static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoCloseButton));
}

void Editor::buildDefaultLayout(ImGuiID dockspace) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_NoCloseButton);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

    ImGuiID center = dockspace;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Content", bottom);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspace);
}

// ---- Windows ------------------------------------------------------------------------------

// The last few hundred frames: frame time in the accent color, GPU time in the secondary
// color, and the frame cap as a faint line. Spikes are stutters.
void Editor::drawFrameTimeGraph() {
    const std::vector<shockwave::FrameSample> all = engine().frameStats().samples();
    const size_t count = std::min<size_t>(all.size(), 300);
    const float height = 64.0f * ImGui::GetStyle().FontScaleDpi;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
    ImGui::InvisibleButton("##frame_graph", ImVec2(width, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const gui::Theme& theme = gui::currentTheme();
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), gui::semanticColorU32(theme.panel));
    if (count < 2) return;
    const size_t first = all.size() - count;
    float top = 1.0f;
    for (size_t i = first; i < all.size(); ++i) top = std::max({top, all[i].frameMs, all[i].gpuMs});
    top *= 1.15f;
    auto point = [&](size_t i, float ms) {
        return ImVec2(
            origin.x + width * static_cast<float>(i - first) / static_cast<float>(count - 1),
            origin.y + height - height * std::min(ms / top, 1.0f));
    };
    const double target = engine().frameLimit() > 0 ? 1000.0 / engine().frameLimit() : 0.0;
    if (target > 0 && target < top) {
        const float y = point(first, static_cast<float>(target)).y;
        draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), gui::semanticColorU32(theme.textDisabled));
    }
    const ImU32 frameColor = ImGui::GetColorU32(gui::accent());
    const ImU32 gpuColor = gui::semanticColorU32(theme.secondary);
    for (size_t i = first + 1; i < all.size(); ++i) {
        if (all[i].gpuMs > 0 && all[i - 1].gpuMs > 0)
            draw->AddLine(point(i - 1, all[i - 1].gpuMs), point(i, all[i].gpuMs), gpuColor);
        draw->AddLine(point(i - 1, all[i - 1].frameMs), point(i, all[i].frameMs), frameColor, 1.5f);
    }
    char label[48];
    std::snprintf(label, sizeof label, "%.1f ms", static_cast<double>(top));
    draw->AddText(ImVec2(origin.x + 4, origin.y + 2), gui::semanticColorU32(theme.textDisabled), label);
    if (ImGui::IsItemHovered()) {
        const float t = std::clamp((ImGui::GetMousePos().x - origin.x) / width, 0.0f, 1.0f);
        const size_t i = first + static_cast<size_t>(t * static_cast<float>(count - 1) + 0.5f);
        ImGui::SetTooltip(
            "frame %.2f ms\nCPU %.2f ms\nGPU %.2f ms", static_cast<double>(all[i].frameMs),
            static_cast<double>(all[i].cpuMs), static_cast<double>(all[i].gpuMs));
    }
}

void Editor::drawStatsWindow() {
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Stats", &m_showStats)) {
        ImGui::End();
        return;
    }
    shockwave::Renderer& renderer = engine().renderer();
    const double fps = engine().fps();
    ImGui::TextDisabled("%s", renderer.gpuName());
    drawGpuStats(renderer);
    ImGui::Text("%.0f fps  (%.2f ms)", fps, fps > 0.0 ? 1000.0 / fps : 0.0);
    drawFrameTimeGraph();

    // Frame pacing: averages hide stutter, so the slow tail is shown too.
    const shockwave::FrameSummary pacing = engine().frameStats().summary();
    if (gui::beginPropertyGrid("pacing")) {
        gui::propertyLabel("1% / 0.1% low");
        ImGui::Text("%.0f / %.0f fps", pacing.low1Fps, pacing.low01Fps);
        gui::propertyLabel("Worst frame");
        ImGui::Text("%.2f ms (jitter %.2f ms)", pacing.maxMs, pacing.jitterMs);
        gui::propertyLabel("CPU / GPU");
        if (pacing.gpuMs > 0)
            ImGui::Text("%.2f / %.2f ms", pacing.cpuMs, pacing.gpuMs);
        else
            ImGui::Text("%.2f ms / no GPU timers", pacing.cpuMs);
        gui::propertyLabel("Presentation");
        ImGui::TextUnformatted(renderer.presentModeName());
        ImGui::SetItemTooltip("Timing: %s", renderer.presentTiming());
        if (pacing.presentMs > 0) {
            gui::propertyLabel("On screen every");
            ImGui::Text("%.2f ms", pacing.presentMs);
        }
        ImGui::EndTable();
    }
    if (!renderer.passTimings().empty() && ImGui::TreeNode("GPU time per pass")) {
        for (const auto& pass : renderer.passTimings())
            ImGui::Text("%7.3f ms  %s", static_cast<double>(pass.gpuMs), pass.name.c_str());
        ImGui::TreePop();
    }
    ImGui::Separator();
    ImGui::Text(
        "Scene %u x %u at %.2fx, %u draws", renderer.renderWidth(), renderer.renderHeight(), renderer.renderScale(),
        renderer.drawCount());
    ImGui::Text("%zu entities", m_world.entityCount());
    ImGui::Text("%u threads in the job system", engine().jobs().threadCount());
    drawGameUIStats();

    ImGui::SeparatorText("Plugins");
    ImGui::Text("Renderer: %s, %s", renderer.backendName().c_str(), renderer.backendDescription());
    if (!renderer.backendFile().empty()) ImGui::SetItemTooltip("%s", renderer.backendFile().string().c_str());
    if (m_gameModule) {
        ImGui::Text("Game module: %s", std::string(m_gameModule->id()).c_str());
        ImGui::SetItemTooltip(
            "%s", m_gameModule->reloadable() ? m_gameModule->file().string().c_str() : "built into the editor");
    } else {
        ImGui::TextColored(
            gui::semanticColor(gui::currentTheme().warning), "Game module: %s isn't built",
            m_hasProject ? m_project.gameModule.c_str() : "none");
    }

    ImGui::SeparatorText("Lighting");
    if (renderer.lightCount() == 0) {
        ImGui::TextDisabled("No lights: a default sun is used. Create > Light adds one.");
    } else {
        ImGui::Text("%u of up to %u lights", renderer.lightCount(), shockwave::Renderer::kMaxLights);
    }
    if (renderer.droppedLights() > 0) {
        ImGui::TextColored(
            gui::semanticColor(gui::currentTheme().warning), "%u lights over the limit are ignored",
            renderer.droppedLights());
    }
    const auto shadowStats = renderer.shadowStats();
    if (renderer.shadowQuality() == shockwave::ShadowQuality::Off) {
        ImGui::TextDisabled("Shadows are off (View > Shadows)");
    } else {
        ImGui::Text(
            "Shadows %s: %u sun cascades, %u shadowed lights, %u caster draws",
            kShadowQualityNames[static_cast<int>(renderer.shadowQuality())], shadowStats.cascades,
            shadowStats.shadowedLights, shadowStats.casterDraws);
        if (shadowStats.skippedLights > 0) {
            ImGui::TextColored(
                gui::semanticColor(gui::currentTheme().warning),
                "%u shadowed lights didn't fit the shadow atlas; the farthest go unshadowed",
                shadowStats.skippedLights);
        }
    }
    if (ImGui::TreeNode("Render graph")) {
        // The passes the renderer ran last frame and the barriers it placed between them.
        ImGui::TextUnformatted(renderer.frameGraph().c_str());
        ImGui::TreePop();
    }

    // How the scheduler laid out the game's systems: each stage runs after the one
    // above it, and systems sharing a stage run at the same time.
    ImGui::SeparatorText("Systems");
    const auto stages = m_session->schedule().stages();
    for (size_t i = 0; i < stages.size(); ++i) {
        std::string row;
        for (const std::string& name : stages[i]) row += (row.empty() ? "" : ", ") + name;
        ImGui::BulletText("Stage %zu: %s", i + 1, row.c_str());
    }
    if (stages.empty()) ImGui::TextDisabled("Laid out when Play starts");
    ImGui::TextDisabled("%.0f ticks per second", engine().tickRate());

    ImGui::SeparatorText("Physics (Jolt)");
    if (const auto* physics = m_session->physics()) {
        ImGui::Text(
            "%u bodies, %u active, %u characters", physics->bodyCount(), physics->activeBodyCount(),
            physics->characterCount());
        ImGui::TextDisabled("%zu contacts this tick", physics->contacts().size());
        for (const auto& problem : physics->diagnostics()) {
            ImGui::TextColored(
                gui::semanticColor(gui::currentTheme().error), "%s: %s", displayName(problem.entity).c_str(),
                problem.message.c_str());
        }
    } else {
        ImGui::TextDisabled("Starts with Play; runs after gameplay each tick.");
    }
    ImGui::End();
}

void Editor::drawPreferencesWindow() {
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Preferences", &m_showPreferences, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    shockwave::Renderer& renderer = engine().renderer();
    ImGui::PushItemWidth(-170.0f * ImGui::GetStyle().FontScaleDpi);
    bool changed = false;

    ImGui::SeparatorText("General");
    changed |= ImGui::Checkbox("Reopen the last project at startup", &m_prefs.reopenLastProject);
    changed |= ImGui::SliderInt(
        "Autosave every", &m_prefs.autosaveMinutes, 0, 30, m_prefs.autosaveMinutes == 0 ? "off" : "%d min");
    ImGui::SetItemTooltip(
        "Unsaved changes are kept in the project's Saved folder and offered back if the editor stops");

    ImGui::SeparatorText("Appearance");
    const std::vector<std::string> themes = findThemes();
    const std::string current = m_prefs.theme.empty() ? "Project default" : fs::path(m_prefs.theme).stem().string();
    if (ImGui::BeginCombo("Theme", current.c_str())) {
        if (ImGui::Selectable("Project default", m_prefs.theme.empty())) {
            m_prefs.theme.clear();
            applyProjectTheme();
            changed = true;
        }
        for (const std::string& theme : themes) {
            if (ImGui::Selectable(fs::path(theme).stem().string().c_str(), theme == m_prefs.theme)) {
                m_prefs.theme = theme;
                engine().setGuiTheme(theme);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Themes are .swtheme files in themes/. Edit one while the editor runs to restyle it live.");
    changed |= ImGui::Checkbox("Panels can leave the main window", &m_prefs.detachablePanels);
    ImGui::SetItemTooltip("Drag a panel's tab or title bar outside the editor to give it a window of its own,\n"
                          "on any monitor. Takes effect the next time the editor starts. On a Wayland\n"
                          "desktop the editor then runs through XWayland, since Wayland can't place windows.");

    ImGui::SeparatorText("Audio");
    if (gui::beginPropertyGrid("audio_preferences")) {
        gui::propertyLabel("Master volume");
        if (ImGui::SliderFloat("##master_volume", &m_prefs.masterVolume, 0, 1, "%.2f")) {
            engine().setMasterVolume(m_prefs.masterVolume);
            if (m_audioPreview) m_audioPreview->setMasterVolume(m_prefs.masterVolume);
            changed = true;
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Viewport");
    if (ImGui::Checkbox("VSync", &m_prefs.vsync)) {
        renderer.setVsync(m_prefs.vsync);
        changed = true;
    }
    ImGui::SetItemTooltip("Off renders as fast as the GPU can go");
    if (ImGui::SliderFloat(
            "Frame limit", &m_prefs.frameLimit, 0.0f, 360.0f, m_prefs.frameLimit > 0 ? "%.0f fps" : "none")) {
        if (m_prefs.frameLimit > 0.0f && m_prefs.frameLimit < 10.0f) m_prefs.frameLimit = 10.0f;
        engine().setFrameLimit(m_prefs.frameLimit);
        changed = true;
    }
    ImGui::SetItemTooltip("A precise cap; steadier than uncapped and saves power. 0 is no cap");
    if (ImGui::Checkbox("Low latency", &m_prefs.lowLatency)) {
        engine().setLowLatency(m_prefs.lowLatency);
        changed = true;
    }
    ImGui::SetItemTooltip("Wait for each frame to reach the screen before starting the next, so input feels\n"
                          "more immediate. Costs some frame rate when the GPU is the bottleneck");
    if (ImGui::SliderFloat("Render scale", &m_prefs.renderScale, 0.25f, 2.0f, "%.2fx")) {
        renderer.setRenderScale(m_prefs.renderScale);
        changed = true;
    }
    int shadows = m_prefs.shadowQuality;
    if (ImGui::Combo("Shadows", &shadows, kShadowQualityNames, IM_ARRAYSIZE(kShadowQualityNames))) {
        m_prefs.shadowQuality = shadows;
        renderer.setShadowQuality(static_cast<shockwave::ShadowQuality>(shadows));
        changed = true;
    }
    ImGui::SetItemTooltip("Shadow quality in the editor's viewport. The player uses the project's setting");

    ImGui::SeparatorText("Plugins");
    if (ImGui::BeginCombo("Renderer", renderer.backendName().c_str())) {
        for (const std::string& name : shockwave::Renderer::availableBackends()) {
            if (name == "null") continue;   // headless: can't draw the editor
            if (ImGui::Selectable(name.c_str(), name == renderer.backendName()) && name != renderer.backendName()) {
                engine().switchRenderer(name);
                m_prefs.renderer = name;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The renderer plugin; switching keeps meshes, textures and settings");
    changed |= drawGpuPreference(engine(), m_prefs);
    changed |= ImGui::Checkbox("Reload game code when rebuilt", &m_prefs.autoReloadGameCode);
    if (ImGui::Checkbox("Reload renderer when rebuilt", &m_prefs.autoReloadRenderer)) {
        engine().setAutoReloadRenderer(m_prefs.autoReloadRenderer);
        changed = true;
    }
    if (ImGui::SliderFloat("Field of view", &m_prefs.fieldOfView, 20.0f, 140.0f, "%.0f\xc2\xb0")) {
        m_camera.fovY = glm::radians(m_prefs.fieldOfView);
        changed = true;
    }
    if (ImGui::SliderFloat("Fly speed", &m_prefs.cameraSpeed, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic)) {
        m_camera.moveSpeed = m_prefs.cameraSpeed;
        changed = true;
    }
    changed |= ImGui::Checkbox("Grid", &m_prefs.showGrid);
    changed |= ImGui::Checkbox("Camera icons", &m_prefs.showCameraIcons);
    changed |= ImGui::Checkbox("Stats overlay", &m_prefs.showStats);
    changed |= ImGui::Checkbox("Play looks through the scene camera", &m_prefs.playFromSceneCamera);

    ImGui::SeparatorText("Snapping");
    changed |= ImGui::DragFloat("Move step", &m_prefs.snapMove, 0.01f, 0.01f, 100.0f, "%.2f units");
    changed |= ImGui::DragFloat("Rotate step", &m_prefs.snapRotate, 0.5f, 1.0f, 180.0f, "%.0f\xc2\xb0");
    changed |= ImGui::DragFloat("Scale step", &m_prefs.snapScale, 0.005f, 0.01f, 10.0f, "%.2f");
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::TextDisabled("Saved to %s", m_options.prefsPath.string().c_str());
    if (changed) m_prefsDirty = true;
    ImGui::End();
}

void Editor::drawProjectSettingsWindow() {
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Project settings", &m_showProjectSettings, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    // The manifest both the editor and the player read (docs/Projects.md).
    if (gui::beginPropertyGrid("project")) {
        gui::propertyLabel("Name");
        ImGui::InputText("##name", &m_projectEdit.name);
        gui::propertyLabel("Startup scene");
        const std::string startup = m_project.relative(m_projectEdit.startupScene);
        if (ImGui::BeginCombo("##startup", startup.c_str())) {
            for (const fs::path& scene : m_scenes) {
                if (ImGui::Selectable(m_project.relative(scene).c_str(), scene == m_projectEdit.startupScene)) {
                    m_projectEdit.startupScene = scene;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The scene the player starts in");
        gui::propertyLabel("Tick rate");
        ImGui::InputDouble("##tick", &m_projectEdit.tickRate, 10.0, 30.0, "%.0f per second");
        m_projectEdit.tickRate = std::clamp(m_projectEdit.tickRate, 1.0, 1000.0);
        ImGui::SetItemTooltip("Fixed simulation updates per second, in the editor's Play and the player");
        gui::propertyLabel("Player window");
        int size[2] = {m_projectEdit.width, m_projectEdit.height};
        if (ImGui::InputInt2("##window", size)) {
            m_projectEdit.width = std::clamp(size[0], 320, 16384);
            m_projectEdit.height = std::clamp(size[1], 240, 16384);
        }
        gui::propertyLabel("Player VSync");
        ImGui::Checkbox("##vsync", &m_projectEdit.vsync);
        gui::propertyLabel("Player frame limit");
        float limit = static_cast<float>(m_projectEdit.frameLimit);
        if (ImGui::SliderFloat("##frame_limit", &limit, 0.0f, 360.0f, limit > 0 ? "%.0f fps" : "none"))
            m_projectEdit.frameLimit = limit > 0.0f ? std::max(10.0, std::round(static_cast<double>(limit))) : 0.0;
        ImGui::SetItemTooltip("A precise frame cap for the player; 0 is none (frame_limit)");
        gui::propertyLabel("Player low latency");
        ImGui::Checkbox("##low_latency", &m_projectEdit.lowLatency);
        ImGui::SetItemTooltip("Wait for each frame to reach the screen before the next (low_latency)");
        gui::propertyLabel("Player shadows");
        ImGui::Combo(
            "##shadow_quality", &m_projectEdit.shadowQuality, kShadowQualityNames, IM_ARRAYSIZE(kShadowQualityNames));
        ImGui::SetItemTooltip("Shadow quality the player starts with; player --shadows overrides it (shadow_quality)");
        gui::propertyLabel("Player renderer");
        if (ImGui::BeginCombo("##renderer", m_projectEdit.renderer.c_str())) {
            for (const std::string& name : shockwave::Renderer::availableBackends())
                if (ImGui::Selectable(name.c_str(), name == m_projectEdit.renderer)) m_projectEdit.renderer = name;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The renderer plugin the player starts with; null runs headless (renderer)");
        gui::propertyLabel("Game module");
        ImGui::BeginDisabled();
        std::string module = m_projectEdit.gameModule;
        ImGui::InputText("##module", &module);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Loaded from bin/modules/game_<id>; see docs/Plugins.md");
        gui::propertyLabel("Assets");
        ImGui::TextDisabled("%s", m_project.relative(m_projectEdit.assetFolder).c_str());
        gui::propertyLabel("Scenes");
        ImGui::TextDisabled("%s", m_project.relative(m_projectEdit.sceneFolder).c_str());
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", m_project.file.string().c_str());
    ImGui::Spacing();

    const bool changed =
        m_projectEdit.name != m_project.name || m_projectEdit.startupScene != m_project.startupScene ||
        m_projectEdit.tickRate != m_project.tickRate || m_projectEdit.width != m_project.width ||
        m_projectEdit.height != m_project.height || m_projectEdit.vsync != m_project.vsync ||
        m_projectEdit.frameLimit != m_project.frameLimit || m_projectEdit.lowLatency != m_project.lowLatency ||
        m_projectEdit.shadowQuality != m_project.shadowQuality || m_projectEdit.renderer != m_project.renderer;
    ImGui::BeginDisabled(!changed || m_projectEdit.name.empty() || playing());
    if (gui::button("Save")) {
        std::string err;
        if (shockwave::saveProject(m_projectEdit, err)) {
            m_project = m_projectEdit;
            engine().setTickRate(m_project.tickRate);
            log("Saved project settings");
        } else {
            error("Couldn't save the project: " + err);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    if (gui::button("Revert")) m_projectEdit = m_project;
    ImGui::EndDisabled();
    ImGui::End();
}

void Editor::drawControlsWindow() {
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Controls", &m_showControls, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    static const char* rows[][2] = {
        {"Right mouse (hold)", "Fly: look around"},
        {"W A S D, E / Q", "Fly: move, up and down (while holding right mouse)"},
        {"Shift", "Fly faster"},
        {"Gamepad sticks", "Fly: left stick moves, right stick looks, triggers up and down (viewport focused)"},
        {"Mouse wheel", "Dolly; while flying, change speed"},
        {"Middle mouse drag", "Pan"},
        {"Left click", "Select; Ctrl adds or removes, Shift adds"},
        {"Shift+click in Hierarchy", "Select a range"},
        {"W / E / R", "Move / rotate / scale gizmo"},
        {"T", "Gizmo axes: world or local"},
        {"Ctrl (while dragging)", "Flip snapping"},
        {"F, or double-click", "Focus the selection"},
        {"F2", "Rename"},
        {"G", "Show or hide the grid"},
        {"F8", "Look through the scene camera / the editor camera"},
        {"Ctrl+D / Del", "Duplicate / delete"},
        {"Ctrl+G", "Group the selection"},
        {"Ctrl+A / Esc", "Select all / none"},
        {"Ctrl+Shift+F", "Move the selection to the view"},
        {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
        {"Ctrl+N / O / S", "New / open / save scene"},
        {"Ctrl+P / Ctrl+Shift+P", "Play or stop / pause"},
        {"Ctrl+,", "Preferences"},
    };
    if (ImGui::BeginTable("controls", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        for (const auto& row : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::drawAboutWindow() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(
            "About Shockwave", &m_showAbout, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    ImGui::Text("Shockwave Editor");
    ImGui::TextDisabled("A free, open source 3D engine. MIT licensed.");
    ImGui::Spacing();
    ImGui::Text("Vulkan 1.3 on %s", engine().renderer().gpuName());
    ImGui::Text("%u job system threads", engine().jobs().threadCount());
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    ImGui::End();
}

void Editor::drawUnsavedChangesModal() {
    if (m_openUnsavedModal) {
        ImGui::OpenPopup("Unsaved changes");
        m_openUnsavedModal = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Save changes to %s first?", sceneDisplayName().c_str());
    ImGui::Spacing();
    if (gui::button("Save", ImVec2(110, 0))) {
        if (m_scenePath.empty()) {
            // Never saved: the save dialog answers later, so stop here and let the user
            // pick the action again after saving.
            ImGui::CloseCurrentPopup();
            m_pendingAction = PendingAction::None;
            showSaveSceneDialog();
        } else if (saveScene()) {
            ImGui::CloseCurrentPopup();
            runAction(m_pendingAction, m_pendingPath);
            m_pendingAction = PendingAction::None;
        }
    }
    ImGui::SameLine();
    if (gui::button("Don't save", ImVec2(110, 0))) {
        ImGui::CloseCurrentPopup();
        m_dirty = false;
        runAction(m_pendingAction, m_pendingPath);
        m_pendingAction = PendingAction::None;
    }
    ImGui::SameLine();
    if (gui::button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        m_pendingAction = PendingAction::None;
    }
    ImGui::EndPopup();
}

void Editor::drawRecoveryModal() {
    if (m_openRecoveryModal) {
        ImGui::OpenPopup("Recover unsaved changes?");
        m_openRecoveryModal = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Recover unsaved changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("There's an autosave of %s from %s,", sceneDisplayName().c_str(), fileTime(m_recoveryPath).c_str());
    ImGui::Text("newer than the saved file. The editor may have closed before you saved.");
    ImGui::Spacing();
    if (gui::button("Restore it", ImVec2(130, 0))) {
        shockwave::World restored;
        std::string err;
        if (shockwave::loadScene(restored, engine().assets(), m_recoveryPath.string(), err)) {
            pushUndo(m_world);
            m_world = std::move(restored);
            select(kNullEntity);
            m_dirty = true;
            log("Restored the autosave. Save to keep it.");
        } else {
            error("Couldn't restore the autosave: " + err);
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (gui::button("Discard it", ImVec2(130, 0))) {
        std::error_code ec;
        fs::remove(m_recoveryPath, ec);
        log("Discarded the autosave");
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}   // namespace editor
