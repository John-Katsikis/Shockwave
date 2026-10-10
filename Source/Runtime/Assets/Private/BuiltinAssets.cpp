#include <Assets/BuiltinAssets.h>

#include <Assets/Assets.h>
#include <Renderer/Mesh.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace shockwave {
namespace {
std::vector<uint8_t> makeChecker(uint32_t size, uint32_t cells, glm::vec3 a, glm::vec3 b) {
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const bool odd = ((x * cells / size) + (y * cells / size)) % 2;
            const glm::vec3 c = odd ? a : b;
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

// One-unit tile with a bright edge and faint quarter lines: a measuring grid when the
// floor repeats it once per unit.
std::vector<uint8_t> makeGrid(uint32_t size) {
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    const uint32_t quarter = size / 4;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float g = 0.34f;
            if (x % quarter < 1 || y % quarter < 1) g = 0.40f;
            if (x < 2 || y < 2 || x >= size - 1 || y >= size - 1) g = 0.55f;
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(g * 255.0f);
            p[1] = uint8_t(g * 1.02f * 255.0f);
            p[2] = uint8_t(g * 1.06f * 255.0f);
            p[3] = 255;
        }
    }
    return pixels;
}

}   // namespace
void createBuiltinAssets(Renderer& renderer, Assets& assets) {
    // The CPU copy stays with the name, so editors can pick the actual triangles.
    auto add = [&](const char* name, MeshData data) {
        auto shared = std::make_shared<const MeshData>(std::move(data));
        assets.addMesh(name, renderer.createMesh(*shared), shared);
    };
    add("cube", makeCube(1));
    add("sphere", makeSphere(0.5f));
    add("plane", makePlane(1, 1));
    add("floor", makePlane(1, 40));
    assets.addTexture(
        "checker", renderer.createTexture(256, 256, makeChecker(256, 8, {0.80f, 0.80f, 0.82f}, {0.55f, 0.56f, 0.6f})));
    assets.addTexture("panel", renderer.createTexture(256, 256, makePanel(256)));
    assets.addTexture("grid", renderer.createTexture(256, 256, makeGrid(256)));
}
}   // namespace shockwave
