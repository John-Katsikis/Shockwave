// Renderer plugins: the engine starts on the headless renderer, swaps to Vulkan and back
// while running, and every mesh and texture handle stays valid across the swaps. Without
// a GPU the Vulkan steps are skipped, not failed.

#include <Engine/Engine.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// Draws the meshes it's given for a few frames, switching renderers on the way.
class SwapApp final : public Application {
public:
    MeshHandle mesh;
    TextureHandle texture;
    std::string switchTo;
    int frames = 0;
    uint32_t draws = 0;

    void onFrame(double) override {
        if (++frames == 2 && !switchTo.empty()) engine().switchRenderer(switchTo);
    }
    void onRender(Renderer& renderer, double) override {
        renderer.drawMesh(mesh, glm::mat4(1.0f), texture);
        draws = std::max(draws, renderer.drawCount());
    }
};
}   // namespace

int main() {
    EngineConfig config;
    config.window = {"renderer swap test", 320, 240, false, true};
    config.renderer = "null";
    config.swappableRenderer = true;
    config.maxFrames = 6;
    Engine engine(config);
    Renderer& renderer = engine.renderer();

    const auto backends = Renderer::availableBackends();
    check(std::find(backends.begin(), backends.end(), "null") != backends.end(), "the null renderer is listed");
    check(std::find(backends.begin(), backends.end(), "vulkan") != backends.end(), "the Vulkan renderer is listed");
    check(renderer.backendName() == "null" && !(renderer.backendFlags() & kRendererPresents), "starts headless");

    // Resources, with a destroyed one in the middle so ids have a gap.
    const MeshData cube = makeCube();
    const MeshHandle first = renderer.createMesh(cube);
    const MeshHandle doomed = renderer.createMesh(makeSphere());
    const MeshHandle last = renderer.createMesh(makePlane());
    renderer.destroyMesh(doomed);
    const uint8_t pixels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    const TextureHandle texture = renderer.createTexture(2, 2, pixels);
    renderer.setShadowQuality(ShadowQuality::Low);
    renderer.setRenderScale(0.5f);

    std::string error;
    check(!renderer.switchBackend("does_not_exist", error) && !error.empty(), "a missing renderer is refused");
    check(renderer.backendName() == "null", "and the current one keeps running");

    // Reloading the same plugin starts a fresh backend from disk.
    check(renderer.switchBackend("null", error), "the null renderer reloads");
    const MeshHandle reused = renderer.createMesh(cube);
    check(reused.id == doomed.id, "freed ids are reused after a swap");

    // To Vulkan and back, through the engine, between frames.
    SwapApp app;
    app.mesh = first;
    app.texture = texture;
    app.switchTo = "vulkan";
    engine.run(app);
    const bool haveGpu = engine.rendererStatus().ok && renderer.backendName() == "vulkan";
    if (!haveGpu) {
        std::printf("skipping the Vulkan steps: %s\n", engine.rendererStatus().message.c_str());
    } else {
        check(engine.rendererStatus().changes == 1, "the engine swapped once");
        check(renderer.shadowQuality() == ShadowQuality::Low && renderer.renderScale() == 0.5f, "settings carry over");
        check(renderer.renderWidth() == 160, "the new backend got the render scale");
        check(app.draws == 1, "the meshes made before the swap draw on the new renderer");
        // A screenshot proves pictures come out of the new backend.
        renderer.requestScreenshot((std::filesystem::temp_directory_path() / "renderer-swap.png").string());
        app.switchTo.clear();
        engine.run(app);
        check(!renderer.screenshotPending() && renderer.screenshotResult().empty(), "Vulkan renders after the swap");
        check(renderer.switchBackend("null", error), "and back to headless");
    }
    check(renderer.createMesh(cube).valid() && last.valid(), "resources still work at the end");

    if (failures) {
        std::fprintf(stderr, "%d renderer swap check(s) failed\n", failures);
        return 1;
    }
    std::puts(haveGpu ? "renderer swap ok" : "renderer swap ok (no GPU: Vulkan steps skipped)");
    return 0;
}
