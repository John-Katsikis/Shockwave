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
        // The shared table: every key round-trips through its own text, presets set only
        // quality keys, and a hand-tuned selection reads as Custom.
        for (const auto& info : graphicsSettingInfo()) {
            auto copy = initial;
            check(info.set(copy, info.get(initial), error) && copy == initial, info.key);
            check(findGraphicsSetting(info.key) == &info, "lookup by key");
        }
        auto edited = initial;
        check(setGraphicsSetting(edited, "anti_aliasing", "fxaa", error), "set anti_aliasing");
        check(setGraphicsSetting(edited, "bloom_quality", "low", error), "set bloom_quality");
        check(setGraphicsSetting(edited, "bloom_scale", "0.25", error), "set bloom_scale");
        check(
            edited.antiAliasing == AntiAliasing::Fxaa && edited.bloomQuality == BloomQuality::Low &&
                edited.bloomScale == 0.25f,
            "setters reach the fields");
        {
            auto taa = edited;
            check(setGraphicsSetting(taa, "anti_aliasing", "taa", error) && taa.antiAliasing == AntiAliasing::Taa,
                  "taa accepted");
        }
        auto unchanged = edited;
        check(!setGraphicsSetting(edited, "anti_aliasing", "msaa", error) && edited == unchanged, "msaa refused");
        check(error.find("taa") != std::string::npos,
              "anti_aliasing refusal lists the choices");
        for (const char* bad : {"2", "-0.5", "nan", "inf", "x"})
            check(!setGraphicsSetting(edited, "bloom_scale", bad, error) && edited == unchanged, "bloom_scale range");
        check(!setGraphicsSetting(edited, "no_such_key", "1", error), "unknown key refused");
        check(
            api.save(edited, error) && loadGraphicsSettings(file, loaded, error) && loaded == edited, "new keys saved");
        for (auto preset : {GraphicsPreset::Low, GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra}) {
            const auto applied = graphicsPreset(preset, edited);
            check(applied.antiAliasing == AntiAliasing::Fxaa, "presets turn on FXAA");
            check(applied.bloomScale == edited.bloomScale, "presets keep personal bloom strength");
            check(matchingGraphicsPreset(applied) == preset, "preset recognised");
        }
        check(graphicsPreset(GraphicsPreset::Low).bloomQuality == BloomQuality::Low, "low preset uses low bloom");
        check(
            graphicsPreset(GraphicsPreset::Low).textureQuality == TextureQuality::Low &&
                graphicsPreset(GraphicsPreset::Low).anisotropy == 4,
            "low preset lowers texture quality and filtering");
        check(
            graphicsPreset(GraphicsPreset::Ultra).textureQuality == TextureQuality::Ultra &&
                GraphicsSettings{}.textureQuality == TextureQuality::Ultra && GraphicsSettings{}.anisotropy == 16,
            "defaults keep full textures and 16x filtering");
        auto textures = initial;
        check(setGraphicsSetting(textures, "texture_quality", "medium", error), "set texture_quality");
        check(setGraphicsSetting(textures, "anisotropy", "2", error), "set anisotropy");
        check(textures.textureQuality == TextureQuality::Medium && textures.anisotropy == 2, "texture setters");
        for (const char* bad : {"3", "0", "32", "x", "16x"})
            check(
                !setGraphicsSetting(textures, "anisotropy", bad, error) && textures.anisotropy == 2,
                "anisotropy values");
        check(
            graphicsPreset(GraphicsPreset::Low).materialQuality == MaterialQuality::Low &&
                graphicsPreset(GraphicsPreset::Medium).materialQuality == MaterialQuality::High &&
                GraphicsSettings{}.materialQuality == MaterialQuality::High,
            "only the low preset drops material detail layers");
        check(
            setGraphicsSetting(textures, "material_quality", "low", error) &&
                textures.materialQuality == MaterialQuality::Low,
            "set material_quality");
        for (const char* bad : {"medium", "ultra", "0", ""})
            check(
                !setGraphicsSetting(textures, "material_quality", bad, error) &&
                    textures.materialQuality == MaterialQuality::Low,
                "material_quality values");
        check(
            GraphicsSettings{}.ambientOcclusion == AmbientOcclusion::Off &&
                graphicsPreset(GraphicsPreset::Low).ambientOcclusion == AmbientOcclusion::Off &&
                graphicsPreset(GraphicsPreset::Medium).ambientOcclusion == AmbientOcclusion::Low &&
                graphicsPreset(GraphicsPreset::High).ambientOcclusion == AmbientOcclusion::Medium &&
                graphicsPreset(GraphicsPreset::Ultra).ambientOcclusion == AmbientOcclusion::High,
            "ambient occlusion is off by default and in the low preset, then rises with the presets");
        check(
            ambientOcclusionSamples(AmbientOcclusion::Off) == 0 && ambientOcclusionSamples(AmbientOcclusion::Low) == 8 &&
                ambientOcclusionSamples(AmbientOcclusion::Medium) == 16 &&
                ambientOcclusionSamples(AmbientOcclusion::High) == 32,
            "ambient occlusion samples per tier");
        check(
            setGraphicsSetting(textures, "ambient_occlusion", "medium", error) &&
                textures.ambientOcclusion == AmbientOcclusion::Medium,
            "set ambient_occlusion");
        for (const char* bad : {"ultra", "on", "1", ""})
            check(
                !setGraphicsSetting(textures, "ambient_occlusion", bad, error) &&
                    textures.ambientOcclusion == AmbientOcclusion::Medium,
                "ambient_occlusion values");
        auto custom = graphicsPreset(GraphicsPreset::High);
        custom.antiAliasing = AntiAliasing::None;
        check(!matchingGraphicsPreset(custom), "hand-tuned settings are Custom");
        const auto schema = graphicsSettingsSchema();
        for (const auto& info : graphicsSettingInfo())
            check(schema.find(info.key) != std::string::npos, "schema lists every key");
        Graphics headless;
        const auto generation = headless.status().generation;
        check(headless.apply(edited, error), "headless apply");
        check(
            headless.status().generation == generation + 1 && headless.status().state == Graphics::State::Applied,
            "status counts applies");
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
