#include <shockwave/Engine.h>

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>

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

} // namespace

// Milestone 3: a 3D scene to fly through. Hold the right mouse button to look,
// WASD to move, E/Q (or Space/Ctrl) up and down, Shift for speed, scroll to change speed.
class Sandbox : public shockwave::Game {
public:
    explicit Sandbox(int grid) : m_grid(grid) {}

    void onStart() override {
        shockwave::Renderer& renderer = engine().renderer();

        m_cube = renderer.createMesh(shockwave::makeCube(1.0f));
        const float groundSize = std::max(400.0f, m_grid * 3.0f + 100.0f);
        m_ground = renderer.createMesh(shockwave::makePlane(groundSize, groundSize / 2.0f));

        const auto checker = makeChecker(512, 8, {0.55f, 0.57f, 0.6f}, {0.42f, 0.44f, 0.47f});
        m_groundTexture = renderer.createTexture(512, 512, checker);
        const auto panel = makePanel(256);
        m_cubeTexture = renderer.createTexture(256, 256, panel);

        const int count = m_grid * m_grid;
        m_cubes.resize(count);
        for (int i = 0; i < count; ++i) {
            Cube& c = m_cubes[i];
            c.base = {(i % m_grid - m_grid / 2) * 3.0f, 0.0f, (i / m_grid - m_grid / 2) * 3.0f};
            c.spin = 0.3f + static_cast<float>((i * 7919) % 100) / 60.0f;
            c.phase = static_cast<float>(i) * 0.37f;
            c.color = hueColor(static_cast<float>((i * 37) % count) / count);
        }

        shockwave::FlyCamera::bindDefaultControls(engine().input());
        m_camera.position = {0.0f, 12.0f, m_grid * 1.5f + 15.0f};
        m_camera.lookAt({0.0f, 0.0f, 0.0f});
        std::puts("Hold right mouse to look, WASD to move, E/Q up/down, Shift faster, Esc quits.");
    }

    // Fixed-rate simulation, spread across cores.
    void onUpdate(double dt) override {
        m_time += dt;
        engine().jobs().parallelFor(m_cubes.size(), 128, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                Cube& c = m_cubes[i];
                c.previous = c.current;
                c.current.angle += c.spin * static_cast<float>(dt);
                c.current.height = 1.5f + std::sin(static_cast<float>(m_time) * 1.3f + c.phase) * 1.0f;
            }
        });
    }

    // In-game debug overlay (run with --gui): any game can draw Dear ImGui in onGui.
    void onGui() override {
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("Sandbox", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            shockwave::Renderer& renderer = engine().renderer();
            ImGui::Text("%.0f fps, %u draws", engine().fps(), renderer.drawCount());
            ImGui::Text("%u x %u", renderer.renderWidth(), renderer.renderHeight());
            float scale = renderer.renderScale();
            if (ImGui::SliderFloat("Render scale", &scale, 0.25f, 2.0f, "%.2fx")) renderer.setRenderScale(scale);
        }
        ImGui::End();
    }

    // Camera runs per rendered frame so looking around is as smooth as the display allows.
    void onFrame(double frameDt) override { m_camera.update(engine().input(), frameDt); }

    void onRender(shockwave::Renderer& renderer, double alpha) override {
        renderer.setClearColor({0.32f, 0.42f, 0.58f, 1.0f});
        renderer.setCamera(m_camera.view(), m_camera.projection(renderer.aspectRatio()));

        renderer.drawMesh(m_ground, glm::mat4(1.0f), m_groundTexture);

        const float a = static_cast<float>(alpha);
        engine().jobs().parallelFor(m_cubes.size(), 128, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const Cube& c = m_cubes[i];
                const float angle = glm::mix(c.previous.angle, c.current.angle, a);
                const float height = glm::mix(c.previous.height, c.current.height, a);
                glm::mat4 model = glm::translate(glm::mat4(1.0f), c.base + glm::vec3(0.0f, height, 0.0f));
                model = glm::rotate(model, angle, glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
                renderer.drawMesh(m_cube, model, m_cubeTexture, c.color);
            }
        });
    }

private:
    struct CubeState {
        float angle = 0.0f;
        float height = 1.5f;
    };
    struct Cube {
        glm::vec3 base{0.0f};
        float spin = 1.0f;
        float phase = 0.0f;
        glm::vec4 color{1.0f};
        CubeState previous, current;
    };

    int m_grid;
    shockwave::FlyCamera m_camera;
    shockwave::MeshHandle m_cube, m_ground;
    shockwave::TextureHandle m_groundTexture, m_cubeTexture;
    std::vector<Cube> m_cubes;
    double m_time = 0.0;
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
        if (i + 1 < argc && std::strcmp(argv[i], "--scale") == 0) renderScale = static_cast<float>(std::atof(argv[i + 1]));
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
