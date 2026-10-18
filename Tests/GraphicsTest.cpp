#include <Engine/Engine.h>
#include <Engine/GraphicsSettings.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

using namespace shockwave;
namespace fs = std::filesystem;
namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
class App final : public Application {
    int frames = 0;
    GraphicsSettings before;
    void onStart() override {
        before = engine().graphics().current();
        std::string error;
        auto next = before;
        next.renderScale = 0.5f;
        check(engine().graphics().apply(next, error), "queue startup setting");
        check(engine().graphics().current() == before, "apply must defer renderer changes");
        next.renderScale = std::numeric_limits<float>::quiet_NaN();
        check(!engine().graphics().apply(next, error), "reject invalid request without replacing pending change");
    }
    void onFrame(double) override {
        ++frames;
        const auto current = engine().graphics().current();
        check(current.renderScale == (frames == 1 ? 0.5f : 0.75f), "next boundary commits queued request");
    }
    void onRender(Renderer&, double) override {
        if (frames != 1) return;
        auto s = engine().graphics().current();
        s.renderScale = 0.75f;
        s.shadows = ShadowQuality::Low;
        s.lodErrorPixels = 2;
        s.frameLimit = 120;
        s.lowLatency = true;
        s.vsync = false;
        std::string error;
        check(engine().graphics().apply(s, error), "queue while recording");
        check(engine().graphics().current().renderScale == 0.5f, "recording frame stays unchanged");
    }
};
}   // namespace
int main() {
    const auto temp =
        fs::temp_directory_path() /
        ("shockwave-graphics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);
    try {
        std::string error;
        Graphics api;
        auto initial = api.current();
        auto low = graphicsPreset(GraphicsPreset::Low, initial);
        check(low.vsync == initial.vsync && low.frameLimit == initial.frameLimit, "quality preset keeps pacing");
        check(api.apply(low, error) && api.current() == low, "headless API applies in memory");
        check(!api.save(low, error), "unconfigured save must report failure");
        const auto file = temp / "Graphics.swgraphics";
        check(api.configure(initial, file, error), "configure defaults and user path");
        check(api.save(low, error), "save selection");
        GraphicsSettings loaded;
        check(loadGraphicsSettings(file, loaded, error) && loaded == low, "round trip all values");
        check(api.save(initial, error), "replace existing preference file");
        check(loadGraphicsSettings(file, loaded, error) && loaded == initial, "replacement persisted");
        for (const char* text :
             {"swgraphics 2\n", "swgraphics 1\nrender_scale nan\n", "swgraphics 1\nframe_limit inf\n",
              "swgraphics 1\nrender_scale 0.1\n", "swgraphics 1\nframe_limit 5\n",
              "swgraphics 1\nshadow_quality banana\n", "swgraphics 1\nvsync maybe\n",
              "swgraphics 1\nlod_error_pixels -1\n", "swgraphics 1\nunknown true\n",
              "swgraphics 1\nvsync true\nvsync false\n", "swgraphics 1\nrender_scale 1 junk\n"}) {
            std::ofstream(file) << text;
            loaded = low;
            check(
                !loadGraphicsSettings(file, loaded, error) && loaded == low, "malformed load leaves destination alone");
            check(error.find(file.string() + ":") == 0, "diagnostic includes file and line");
        }
        std::ofstream(file) << "# partial config\nswgraphics 1\nvsync false # comment\n";
        loaded = low;
        check(
            loadGraphicsSettings(file, loaded, error) && !loaded.vsync && loaded.renderScale == low.renderScale,
            "partial files inherit values");
        auto invalid = initial;
        invalid.frameLimit = std::numeric_limits<double>::infinity();
        check(!api.apply(invalid, error) && api.current() == low, "invalid API request changes nothing");
        check(!api.save(invalid, error), "invalid saves refused");
        std::ofstream(temp / "not-a-directory") << "keep me";
        check(!saveGraphicsSettings(temp / "not-a-directory" / "file", low, error), "write failure reported");
        Project project;
        project.root = temp;
        project.shadowQuality = 1;
        check(
            projectGraphicsSettings(project, loaded, error) && loaded.shadows == ShadowQuality::Low,
            "project manifest supplies defaults");
        fs::create_directories(temp / "Config");
        std::ofstream(temp / "Config" / "Graphics.swgraphics") << "swgraphics 1\nrender_scale 0.75\n";
        check(
            projectGraphicsSettings(project, loaded, error) && loaded.renderScale == 0.75f &&
                loaded.shadows == ShadowQuality::Low,
            "project config overlays manifest");
        const auto defaults = loaded;
        std::ofstream(temp / "Config" / "Graphics.swgraphics") << "swgraphics 1\nrender_scale 9\n";
        check(
            !projectGraphicsSettings(project, loaded, error) && loaded == defaults,
            "invalid project config leaves defaults unchanged");
        EngineConfig config;
        config.renderer = "null";
        config.window.hidden = true;
        config.workerThreads = 1;
        config.maxFrames = 2;
        config.showFpsInTitle = false;
        Engine engine(config);
        App app;
        engine.run(app);
        auto actual = engine.graphics().current();
        check(
            actual.shadows == ShadowQuality::Low && actual.lodErrorPixels == 2 && actual.frameLimit == 120 &&
                actual.lowLatency && !actual.vsync,
            "wrapper updates every renderer and pacing field");
        fs::remove_all(temp);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        fs::remove_all(temp);
        return 1;
    }
}
