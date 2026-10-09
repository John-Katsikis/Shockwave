#include "Editor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

// Shockwave Editor.
//   editor [scene.swscene] [--uncapped] [--frames N]
// With no scene it opens scenes/main.swscene, creating it from the sample scene on first run.
int main(int argc, char** argv) {
    shockwave::EngineConfig config;
    config.window.title = "Shockwave Editor";
    config.window.width = 1600;
    config.window.height = 900;
    config.window.vsync = true;   // an editor idles a lot; toggle in the Renderer panel
    config.gui = true;
    config.quitOnEscape = false;
    config.showFpsInTitle = false;

    editor::EditorOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--uncapped") == 0) {
            config.window.vsync = false;
        } else if (i + 1 < argc && std::strcmp(argv[i], "--frames") == 0) {
            config.maxFrames = std::atoi(argv[++i]);
        } else if (i + 1 < argc && std::strcmp(argv[i], "--select") == 0) {
            options.selectOnStart = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            options.scenePath = argv[i];
        }
    }

    try {
        shockwave::Engine engine(config);
        editor::Editor editor(options);
        engine.run(editor);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
