// Game code and plugins: loading the project's game module, hot reloading it when it's
// rebuilt (from the editor or outside it), and swapping or reloading the renderer.
#include "Editor.h"

#include <Assets/BuiltinAssets.h>
#include <UI/Gui.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;

namespace {
double seconds() { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }
}   // namespace

// ---- Loading ------------------------------------------------------------------------------

void Editor::createGameInstance() {
    m_gameInstance = shockwave::makeGameInstance(m_gameModule ? &m_gameModule->module() : nullptr);
    m_gameInstance->init(&engine());
    m_session = std::make_unique<shockwave::PlaySession>(m_world, engine().jobs(), *m_gameInstance, &engine());
}

void Editor::unloadGameModule() {
    stop();
    m_session.reset();
    if (m_gameInstance) m_gameInstance->shutdown();
    m_gameInstance.reset();
    // Everything that can hold the module's components goes while its code is loaded.
    m_world.clear();
    m_undo.clear();
    m_redo.clear();
    m_editStart = {};
    m_selection.clear();
    m_gameModule.reset();
    m_gameModuleMatches = false;
}

void Editor::loadGameModule(const std::string& id) {
    if (m_gameModule && m_gameModule->id() == id) {
        m_gameModuleMatches = true;
        return;
    }
    unloadGameModule();
    std::string err;
    m_gameModule = shockwave::GameModuleLibrary::load(id, err);
    m_gameModuleMatches = m_gameModule != nullptr;
    if (m_gameModule) {
        if (const auto& createAssets = m_gameModule->module().createAssets) createAssets(engine());
        log("Loaded game module \"" + id + "\"" +
            (m_gameModule->reloadable() ? " from " + m_gameModule->file().filename().string() : " (built in)"));
    } else {
        warn("This project's game module \"" + id + "\" isn't built, so Play is off: " + err);
        if (canBuildGameCode())
            warn(
                "Build it with Build > Build and reload game code, or: cmake --build " +
                std::string(SHOCKWAVE_BUILD_DIR) + " --target game_" + id);
    }
    createGameInstance();
    // Rebuilds are picked up from the module's usual place, and so is a first build.
    m_gameModuleWatcher.watch(shockwave::pluginsReloadable() ? shockwave::pluginPath("game", id) : fs::path());
}

// ---- Hot reload ---------------------------------------------------------------------------

void Editor::reloadGameCode() {
    if (!m_hasProject) return;
    if (!m_gameModule) {   // first build since the project opened
        const std::string id = m_project.gameModule;
        loadGameModule(id);
        return;
    }
    if (!m_gameModule->reloadable()) {
        warn("Game code is built into this editor (SHOCKWAVE_MODULAR is off), so it can't reload");
        return;
    }
    const double start = seconds();
    const bool wasPlaying = playing();
    if (wasPlaying) stop();

    m_session.reset();
    m_gameInstance->shutdown();
    m_gameInstance.reset();

    std::vector<shockwave::World*> worlds{&m_world, &m_editStart};
    for (shockwave::World& world : m_undo) worlds.push_back(&world);
    for (shockwave::World& world : m_redo) worlds.push_back(&world);
    std::string err;
    const bool ok = shockwave::reloadGameModule(m_gameModule, worlds, engine().assets(), err);
    if (ok && m_gameModule->module().createAssets) m_gameModule->module().createAssets(engine());
    createGameInstance();

    if (!ok) {
        error("Couldn't reload game code (the previous code keeps running): " + err);
        return;
    }
    char text[160];
    std::snprintf(
        text, sizeof(text), "Reloaded game code \"%s\" in %.0f ms", std::string(m_gameModule->id()).c_str(),
        (seconds() - start) * 1000.0);
    log(text);
    // Pick up where things were: back into Play if it was running.
    if (wasPlaying) play();
}

bool Editor::canBuildGameCode() const {
    std::error_code ec;
    return shockwave::pluginsReloadable() && fs::is_directory(SHOCKWAVE_BUILD_DIR, ec);
}

void Editor::buildGameCode() {
    if (!m_hasProject || m_build.running || !canBuildGameCode()) return;
    if (m_build.thread.joinable()) m_build.thread.join();
    m_build.target = "game_" + m_project.gameModule;
    m_build.running = true;
    m_build.finished = false;
    m_build.started = seconds();
    log("Building " + m_build.target + "...");
    // CMake's output goes to the Console as it arrives; the reload happens on the main
    // thread once it's done (pollGameCode).
    const std::string command =
        "cmake --build \"" + std::string(SHOCKWAVE_BUILD_DIR) + "\" --target " + m_build.target + " 2>&1";
    m_build.thread = std::thread([this, command] {
        int status = -1;
        if (FILE* pipe = popen(command.c_str(), "r")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), pipe)) {
                std::string text = line;
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                const bool problem = text.find("error") != std::string::npos;
                if (problem)
                    SW_LOG(shockwave::LogGame, Error, "{}", text);
                else if (text.find("warning") != std::string::npos)
                    SW_LOG(shockwave::LogGame, Warning, "{}", text);
                else
                    SW_LOG(shockwave::LogGame, Info, "{}", text);
            }
            status = pclose(pipe);
        }
        m_build.exitCode = status;
        m_build.finished = true;
    });
}

void Editor::pollGameCode() {
    if (std::exchange(m_reloadGameCodeRequested, false)) reloadGameCode();
    if (m_build.finished.exchange(false)) {
        m_build.thread.join();
        m_build.running = false;
        if (m_build.exitCode == 0) {
            char text[160];
            std::snprintf(
                text, sizeof(text), "Built %s in %.1f s", m_build.target.c_str(), seconds() - m_build.started);
            log(text);
            m_gameModuleWatcher.watch(m_gameModuleWatcher.file());   // this build is reloaded here
            reloadGameCode();
        } else {
            error(
                "Building " + m_build.target + " failed; the errors are above. The previous game code keeps running.");
        }
    }
    // Rebuilt outside the editor (an IDE, a terminal, an AI agent running CMake).
    if (m_prefs.autoReloadGameCode && !m_build.running && m_gameModuleWatcher.poll(seconds())) {
        log(m_gameModuleWatcher.file().filename().string() + " was rebuilt: reloading game code");
        reloadGameCode();
    }
    // Renderer swaps and reloads report themselves: the engine logs them to the Console.
}

// ---- Menu ---------------------------------------------------------------------------------

void Editor::drawBuildMenu() {
    if (!ImGui::BeginMenu("Build")) return;
    const bool reloadable = shockwave::pluginsReloadable();
    ImGui::SeparatorText("Game code");
    if (ImGui::MenuItem(
            m_build.running ? "Building..." : "Build and reload game code", "Ctrl+Alt+B", false,
            m_hasProject && canBuildGameCode() && !m_build.running))
        buildGameCode();
    ImGui::SetItemTooltip(
        "Compile the project's game module (cmake --build ... --target game_%s) and reload it.\n"
        "The scene, selection and undo history are kept; Play restarts if it was running.",
        m_project.gameModule.c_str());
    if (ImGui::MenuItem("Reload game code", nullptr, false, m_hasProject && reloadable))
        m_reloadGameCodeRequested = true;
    ImGui::SetItemTooltip("Load the game module again as it is on disk");
    if (ImGui::MenuItem("Reload when rebuilt", nullptr, &m_prefs.autoReloadGameCode, reloadable)) m_prefsDirty = true;
    ImGui::SetItemTooltip("Reload by itself whenever the module is rebuilt, from here or anywhere else");

    ImGui::SeparatorText("Renderer");
    shockwave::Renderer& renderer = engine().renderer();
    for (const std::string& name : shockwave::Renderer::availableBackends()) {
        // The headless renderer can't draw the editor's own interface.
        const bool usable = name != "null";
        if (ImGui::MenuItem(name.c_str(), nullptr, renderer.backendName() == name, usable && renderer.swappable()) &&
            name != renderer.backendName()) {
            engine().switchRenderer(name);
            m_prefs.renderer = name;
            m_prefsDirty = true;
        }
        if (!usable) ImGui::SetItemTooltip("Headless: draws nothing, for servers and tools (player --renderer null)");
    }
    if (ImGui::MenuItem("Reload renderer", "Ctrl+Alt+R", false, renderer.swappable() && reloadable))
        engine().reloadRenderer();
    ImGui::SetItemTooltip("Restart the renderer from its plugin as it is on disk, with any rebuilt shaders.\n"
                          "Meshes, textures and settings carry over.");
    bool autoRenderer = engine().autoReloadRenderer();
    if (ImGui::MenuItem("Reload renderer when rebuilt", nullptr, &autoRenderer, reloadable)) {
        engine().setAutoReloadRenderer(autoRenderer);
        m_prefs.autoReloadRenderer = autoRenderer;
        m_prefsDirty = true;
    }

    ImGui::SeparatorText("Players");
    if (ImGui::MenuItem(m_package.running() ? "Packaging..." : "Package game...", nullptr, false, m_hasProject))
        m_package.open = true;
    ImGui::SetItemTooltip("Make a game folder players can run, without the editor (docs/Packaging.md)");
    ImGui::EndMenu();
}

}   // namespace editor
