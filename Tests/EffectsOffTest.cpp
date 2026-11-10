// Off really means off (S10): each optional effect is turned on and then off again while
// the engine runs, the way a player does in a menu. On must cost a pass and GPU memory;
// off must give both back, and the total must return to where it started. Starting with
// an effect off is the fixtures' job (Tests/Graphics*Check.cmake); this is the change
// mid-run, where a leak would hide. Runs on a real GPU backend ("vulkan", gpu label).
#include <Engine/Engine.h>
#include <Engine/GraphicsSettings.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::fprintf(stderr, "FAILED: %s\n", what.c_str());
}

struct Effect {
    const char* name;
    const char* key;
    const char* on;
    const char* off;
    GpuMemoryCategory category;
    const char* pass;   // a word its passes have in the frame graph; null: not checked
};
const Effect kEffects[] = {
    {"bloom", "bloom_quality", "high", "off", GpuMemoryCategory::Bloom, "bloom"},
    {"FXAA", "anti_aliasing", "fxaa", "none", GpuMemoryCategory::AntiAliasing, "fxaa"},
    {"TAA", "anti_aliasing", "taa", "none", GpuMemoryCategory::TemporalHistory,
     nullptr},   // no resolve pass without a camera
    {"ambient occlusion", "ambient_occlusion", "high", "off", GpuMemoryCategory::AmbientOcclusion, "ambient"},
    {"shadows", "shadow_quality", "high", "off", GpuMemoryCategory::ShadowMaps, nullptr},
};

constexpr int kSettleFrames = 12;   // past the frames in flight that keep freed images alive

struct App final : Application {
    GraphicsSettings off;
    size_t effect = 0;
    enum class Step { Baseline, On, Off } step = Step::Baseline;
    int wait = 0;
    uint64_t baseline = 0;
    bool finished = false;

    void apply(const char* key, const char* value) {
        GraphicsSettings next = engine().graphics().requested();
        std::string error;
        check(setGraphicsSetting(next, key, value, error), std::string(key) + " " + value + ": " + error);
        check(engine().graphics().apply(next, error), std::string("apply ") + key + ": " + error);
        wait = 0;
    }
    void onStart() override {
        // Everything optional off: the least the renderer can run with.
        off = graphicsPreset(GraphicsPreset::Low);
        std::string error;
        for (const auto& [key, value] : std::vector<std::pair<const char*, const char*>>{
                 {"bloom_quality", "off"},
                 {"anti_aliasing", "none"},
                 {"ambient_occlusion", "off"},
                 {"shadow_quality", "off"},
                 {"decals_quality", "off"},
                 {"fog_quality", "off"},
                 {"transmission_quality", "off"},
                 {"skin_quality", "off"},
                 {"vsync", "false"}})
            check(setGraphicsSetting(off, key, value, error), std::string(key) + ": " + error);
        check(engine().graphics().apply(off, error), "apply everything off: " + error);
    }
    void onFrame(double) override {
        if (engine().graphics().status().state == Graphics::State::Pending) return;
        if (++wait < kSettleFrames) return;
        Renderer& renderer = engine().renderer();
        const RendererMemoryReport memory = renderer.memoryReport();
        const std::string graph = renderer.frameGraph();
        if (step == Step::Baseline) {
            baseline = memory.gpuBytes();
            for (const Effect& e : kEffects)
                check(memory[e.category].bytes == 0, std::string(e.name) + " has memory before it was ever on");
            step = Step::On;
            apply(kEffects[0].key, kEffects[0].on);
            return;
        }
        const Effect& e = kEffects[effect];
        if (step == Step::On) {
            check(memory[e.category].bytes > 0, std::string(e.name) + " on has no GPU memory of its own");
            check(memory.gpuBytes() > baseline, std::string(e.name) + " on doesn't add to the total");
            if (e.pass)
                check(graph.find(e.pass) != std::string::npos, std::string(e.name) + " on has no pass:\n" + graph);
            std::printf(
                "%-18s on  %8.2f MiB of its own, total %8.2f MiB\n", e.name,
                double(memory[e.category].bytes) / 1048576.0, double(memory.gpuBytes()) / 1048576.0);
            step = Step::Off;
            apply(e.key, e.off);
            return;
        }
        check(
            memory[e.category].bytes == 0,
            std::string(e.name) + " off keeps " + std::to_string(memory[e.category].bytes) + " bytes");
        check(
            memory.gpuBytes() == baseline, std::string(e.name) + " off leaves the total at " +
                                               std::to_string(memory.gpuBytes()) + " bytes, not the " +
                                               std::to_string(baseline) + " it started from");
        if (e.pass)
            check(graph.find(e.pass) == std::string::npos, std::string(e.name) + " off still has a pass:\n" + graph);
        std::printf("%-18s off: total back to %8.2f MiB\n", e.name, double(memory.gpuBytes()) / 1048576.0);
        if (++effect == std::size(kEffects)) {
            finished = true;
            engine().quit();
            return;
        }
        step = Step::On;
        apply(kEffects[effect].key, kEffects[effect].on);
    }
};
}   // namespace

int main(int argc, char** argv) {
    EngineConfig config;
    config.window = {"effects off test", 320, 180, false, true};
    config.renderer = argc > 1 ? argv[1] : "vulkan";
    config.maxFrames = 2000;
    config.showFpsInTitle = false;
    config.pipelineCache = false;
    Engine engine(config);
    App app;
    engine.run(app);
    check(app.finished, "every effect was tried (stopped at " + std::to_string(app.effect) + ")");
    if (failures) {
        std::fprintf(stderr, "effects_off: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::puts("effects_off: ok");
    return 0;
}
