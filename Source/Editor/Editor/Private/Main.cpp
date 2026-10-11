#include "Editor.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>

// Shockwave Editor.
//   editor [project folder | project.swproject | scene.swscene | prefab.swprefab] [options]
//
// With nothing given it opens the project in the working folder (or a folder above it),
// else the last project, else the project hub. A scene or prefab opens inside its project.
//
// Options:
//   --project path         the project to open (same as giving it first)
//   --assets folder        import this folder instead of the project's assets
//   --theme file.swtheme   UI theme for this run (Preferences sets the usual one)
//   --uncapped             vsync off for this run
//   --renderer name        renderer plugin to start with (Build > Renderer sets the usual one)
//   --content-folder path  show a folder inside Content on startup
//   --preferences          show the Preferences window on startup
//   --prefs file           preferences file to use instead of the user's (tests)
//   --play                 press Play once the scene is open (tests)
//   --select N             select the Nth entity of the scene (screenshots)
//   --frames N             quit after N frames (smoke tests)
int main(int argc, char** argv) {
    try {
        shockwave::EngineConfig config;
        config.window.title = "Shockwave Editor";
        config.window.width = 1600;
        config.window.height = 900;
        config.gui = true;
        config.quitOnEscape = false;
        config.showFpsInTitle = false;

        editor::EditorOptions options;
        options.prefsPath = editor::defaultPreferencesPath();
        bool uncapped = false;
        std::string renderer;   // --renderer
        auto integer = [](const std::string& text, const char* flag, int minimum) {
            int value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size() || value < minimum)
                throw std::runtime_error(std::string("Invalid value for ") + flag);
            return value;
        };
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
                return argv[++i];
            };
            if (arg == "--project")
                options.path = value();
            else if (arg == "--assets")
                options.assetFolder = value();
            else if (arg == "--theme")
                options.theme = value();
            else if (arg == "--content-folder") {
                options.contentFolder = value();
                options.focusContent = true;
            } else if (arg == "--preferences")
                options.showPreferences = true;
            else if (arg == "--prefs")
                options.prefsPath = value();
            else if (arg == "--frames")
                config.maxFrames = integer(value(), "--frames", 1);
            else if (arg == "--select")
                options.selectOnStart = integer(value(), "--select", 0);
            else if (arg == "--uncapped")
                uncapped = true;
            else if (arg == "--renderer")
                renderer = value();
            else if (arg == "--play")
                options.playOnStart = true;
            else if (arg == "--help") {
                std::puts("editor [project folder | project.swproject | scene.swscene | prefab.swprefab]\n"
                          "       [--project path] [--assets folder] [--content-folder path] [--preferences]\n"
                          "       [--theme file] [--uncapped] [--renderer name] [--prefs file] [--play] [--select N]\n"
                          "       [--frames N]");
                return 0;
            } else if (!arg.empty() && arg[0] != '-')
                options.path = arg;
            else
                throw std::runtime_error("Unknown option: " + arg);
        }
        std::error_code ec;
        if (options.path.empty()) {
            if (const auto found = shockwave::findProjectFile(std::filesystem::current_path(ec)))
                options.path = found->string();
        }

        std::string error;
        if (!editor::loadPreferences(options.prefsPath, options.prefs, error)) {
            std::fprintf(stderr, "Editor: preferences not loaded (using defaults): %s\n", error.c_str());
        }
        if (!options.theme.empty())
            config.guiTheme = options.theme;
        else if (!options.prefs.theme.empty())
            config.guiTheme = options.prefs.theme;
        config.window.vsync = options.prefs.vsync && !uncapped;
        config.guiViewports = options.prefs.detachablePanels;
        if (uncapped) options.prefs.vsync = false, options.prefs.frameLimit = 0;
        // The renderer can be swapped and reloaded while the editor runs (Build menu).
        config.swappableRenderer = true;
        if (!renderer.empty()) options.prefs.renderer = renderer;
        config.renderer = options.prefs.renderer;

        std::unique_ptr<shockwave::Engine> started;
        try {
            started = std::make_unique<shockwave::Engine>(config);
        } catch (const std::exception& e) {
            if (config.renderer == "vulkan") throw;
            std::fprintf(
                stderr, "Editor: the %s renderer didn't start (%s); using vulkan\n", config.renderer.c_str(), e.what());
            config.renderer = options.prefs.renderer = "vulkan";
            started = std::make_unique<shockwave::Engine>(config);
        }
        shockwave::Engine& engine = *started;
        editor::Editor editor(std::move(options));
        engine.run(editor);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Editor: %s\n", e.what());
        return 1;
    }
    return 0;
}
