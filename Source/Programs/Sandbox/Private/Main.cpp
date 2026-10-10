#include <Engine/Engine.h>

#include <glm/gtc/constants.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

namespace {

// Procedural textures until asset loading lands.
std::vector<uint8_t> makeChecker(uint32_t size, uint32_t cells, glm::vec3 a, glm::vec3 b) {
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const bool odd = ((x * cells / size) + (y * cells / size)) % 2;
            const bool line = (x % (size / cells)) < 2 || (y % (size / cells)) < 2;
            glm::vec3 c = odd ? a : b;
            if (line) c *= 0.6f;
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(c.r * 255.0f);
            p[1] = uint8_t(c.g * 255.0f);
            p[2] = uint8_t(c.b * 255.0f);
            p[3] = 255;
        }
    }
    return pixels;
}

std::vector<uint8_t> makePanel(uint32_t size) {
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float edge = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
            const float border = edge < 0.06f ? 0.35f : 1.0f;
            const float rivet = (std::abs(u - 0.15f) < 0.03f || std::abs(u - 0.85f) < 0.03f) &&
                                        (std::abs(v - 0.15f) < 0.03f || std::abs(v - 0.85f) < 0.03f)
                                    ? 0.5f
                                    : 1.0f;
            const float shade = 0.85f + 0.15f * std::sin(u * 40.0f) * std::sin(v * 3.0f);
            const uint8_t g = uint8_t(255.0f * border * rivet * shade);
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            p[0] = p[1] = p[2] = g;
            p[3] = 255;
        }
    }
    return pixels;
}

glm::vec4 hueColor(float h) {
    const glm::vec3 k = glm::abs(glm::fract(glm::vec3(h) + glm::vec3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f);
    return glm::vec4(glm::clamp(k - 1.0f, 0.0f, 1.0f) * 0.8f + 0.2f, 1.0f);
}

}   // namespace

// A 3D scene to fly through: 10,000 entities, moved by the engine's Spin and Bob
// systems and drawn by renderWorld, all spread across the job system.
// Hold the right mouse button to look, WASD to move, E/Q (or Space/Ctrl) up and down,
// Shift for speed, scroll to change speed.
class Sandbox : public shockwave::Application {
public:
    explicit Sandbox(int grid) : m_grid(grid) {}

    void onStart() override {
        using namespace shockwave;
        Renderer& renderer = engine().renderer();

        const MeshHandle cube = renderer.createMesh(makeCube(1.0f));
        const float groundSize = std::max(400.0f, m_grid * 3.0f + 100.0f);
        const MeshHandle ground = renderer.createMesh(makePlane(groundSize, groundSize / 2.0f));
        const auto checker = makeChecker(512, 8, {0.55f, 0.57f, 0.6f}, {0.42f, 0.44f, 0.47f});
        const TextureHandle groundTexture = renderer.createTexture(512, 512, checker);
        const auto panel = makePanel(256);
        const TextureHandle cubeTexture = renderer.createTexture(256, 256, panel);

        const Entity env = m_world.create();
        m_world.add<Environment>(env, {{0.32f, 0.42f, 0.58f, 1.0f}});
        const Entity floor = m_world.create();
        m_world.add<Transform>(floor);
        m_world.add<MeshRenderer>(floor, {ground, groundTexture});

        const int count = m_grid * m_grid;
        const glm::vec3 axis = glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f));
        for (int i = 0; i < count; ++i) {
            const Entity e = m_world.create();
            Bob bob{
                .amplitude = 1.0f, .frequency = 1.3f / glm::two_pi<float>(), .phase = static_cast<float>(i) * 0.37f};
            bob.offset = bob.amplitude * std::sin(bob.phase);   // start mid-wave, with no jump on the first tick
            m_world.add<Transform>(
                e, {{(i % m_grid - m_grid / 2) * 3.0f, 1.5f + bob.offset, (i / m_grid - m_grid / 2) * 3.0f}});
            m_world.add<MeshRenderer>(e, {cube, cubeTexture, hueColor(static_cast<float>((i * 37) % count) / count)});
            m_world.add<Spin>(e, {axis, glm::degrees(0.3f + static_cast<float>((i * 7919) % 100) / 60.0f)});
            m_world.add<Bob>(e, bob);
        }
        addBuiltinSystems(m_schedule);

        FlyCamera::bindDefaultControls(engine().input());
        m_camera.position = {0.0f, 12.0f, m_grid * 1.5f + 15.0f};
        m_camera.lookAt({0.0f, 0.0f, 0.0f});
        std::puts("Hold right mouse to look, WASD to move, E/Q up/down, Shift faster, Esc quits.");
    }

    // Fixed-rate simulation: the schedule runs Spin and Bob side by side where it can,
    // each split across every core.
    void onUpdate(double dt) override {
        shockwave::storePreviousTransforms(m_world, engine().jobs());
        m_schedule.run(m_world, engine().jobs(), dt);
    }

    // In-game debug overlay (run with --gui): any game can draw Dear ImGui in onGui.
    void onGui() override {
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("Sandbox", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            shockwave::Renderer& renderer = engine().renderer();
            ImGui::Text("%.0f fps, %u draws", engine().fps(), renderer.drawCount());
            ImGui::Text("%zu entities, %u threads", m_world.entityCount(), engine().jobs().threadCount());
            ImGui::Text("%u x %u", renderer.renderWidth(), renderer.renderHeight());
            float scale = renderer.renderScale();
            if (ImGui::SliderFloat("Render scale", &scale, 0.25f, 2.0f, "%.2fx")) renderer.setRenderScale(scale);
        }
        ImGui::End();
    }

    // Camera runs per rendered frame so looking around is as smooth as the display allows.
    void onFrame(double frameDt) override { m_camera.update(engine().input(), frameDt); }

    void onRender(shockwave::Renderer& renderer, double alpha) override {
        renderer.setCamera(m_camera.view(), m_camera.projection(renderer.aspectRatio()));
        shockwave::renderWorld(m_world, renderer, engine().jobs(), alpha, &engine().assets());
    }

private:
    int m_grid;
    shockwave::FlyCamera m_camera;
    shockwave::World m_world;
    shockwave::Schedule m_schedule;
};

int main(int argc, char** argv) {
    shockwave::EngineConfig config;
    config.window.title = "Shockwave Sandbox";
    float renderScale = 1.0f;
    int grid = 100;   // 100 x 100 = 10,000 cubes
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--vsync") == 0) config.window.vsync = true;
        if (std::strcmp(argv[i], "--gui") == 0) config.gui = true;
        if (i + 1 < argc && std::strcmp(argv[i], "--frames") == 0) config.maxFrames = std::atoi(argv[i + 1]);
        if (i + 1 < argc && std::strcmp(argv[i], "--grid") == 0) grid = std::max(1, std::atoi(argv[i + 1]));
        if (i + 1 < argc && std::strcmp(argv[i], "--scale") == 0)
            renderScale = static_cast<float>(std::atof(argv[i + 1]));
    }

    try {
        shockwave::Engine engine(config);
        engine.renderer().setRenderScale(renderScale);
        Sandbox game(grid);
        engine.run(game);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
