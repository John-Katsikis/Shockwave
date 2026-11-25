#include <Assets/BuiltinAssets.h>

#include <Assets/Assets.h>
#include <Assets/MeshOptimise.h>
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

// Ripples for the built-in water (W3): a height field of a few sine waves whose
// frequencies are whole numbers of cycles per tile, so it repeats seamlessly, turned into
// tangent-space normals (linear RGB, blue up).
std::vector<uint8_t> makeRipples(uint32_t size) {
    struct Wave {
        float kx, ky, amplitude, phase;
    };
    constexpr Wave waves[] = {{1, 2, 0.30f, 0.0f}, {3, -1, 0.22f, 1.7f},  {-2, 3, 0.18f, 4.1f},
                              {5, 2, 0.10f, 2.3f}, {-4, -5, 0.07f, 5.2f}, {7, -3, 0.05f, 0.9f}};
    constexpr float kTau = 6.28318531f;
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            float du = 0.0f, dv = 0.0f;   // the height's slope per tile
            for (const Wave& w : waves) {
                const float c = std::cos(kTau * (w.kx * u + w.ky * v) + w.phase) * w.amplitude * kTau;
                du += c * w.kx;
                dv += c * w.ky;
            }
            const glm::vec3 n = glm::normalize(glm::vec3(-du * 0.04f, -dv * 0.04f, 1.0f));
            uint8_t* p = &pixels[(size_t(y) * size + x) * 4];
            p[0] = uint8_t(std::lround((n.x * 0.5f + 0.5f) * 255.0f));
            p[1] = uint8_t(std::lround((n.y * 0.5f + 0.5f) * 255.0f));
            p[2] = uint8_t(std::lround((n.z * 0.5f + 0.5f) * 255.0f));
            p[3] = 255;
        }
    }
    return pixels;
}

}   // namespace
void createBuiltinAssets(Renderer& renderer, Assets& assets) {
    // The CPU copy stays with the name, so editors can pick the actual triangles.
    auto add = [&](const char* name, MeshData data) {
        optimiseMesh(data);   // reordered; the sphere also gets levels of detail
        auto shared = std::make_shared<const MeshData>(std::move(data));
        const MeshHandle mesh = renderer.createMesh(*shared);
        if (!shared->lods.empty()) renderer.setMeshLods(mesh, shared->lods, shared->lodIndices);
        assets.addMesh(name, mesh, shared);
    };
    add("cube", makeCube(1));
    add("sphere", makeSphere(0.5f));
    add("plane", makePlane(1, 1));
    add("floor", makePlane(1, 40));
    assets.addTexture(
        "checker", renderer.createTexture(256, 256, makeChecker(256, 8, {0.80f, 0.80f, 0.82f}, {0.55f, 0.56f, 0.6f})));
    assets.addTexture("panel", renderer.createTexture(256, 256, makePanel(256)));
    assets.addTexture("grid", renderer.createTexture(256, 256, makeGrid(256)));

    // The built-in water material (W3): Hierarchy > Create > Water plane uses it, and a
    // scene names it as "water", like the built-in meshes.
    const TextureHandle ripples = renderer.createTexture(256, 256, makeRipples(256), false);
    assets.addTexture("water-ripples", ripples);
    auto water = std::make_shared<MaterialAsset>();
    water->desc.params.family = int(MaterialFamily::Water);
    water->desc.slots[size_t(MaterialSlot::WaterNormal)].texture.asset = "water-ripples";
    water->desc.chain = {"water"};
    water->desc.features = materialFeatures(water->desc);
    std::array<TextureHandle, kAllMaterialSlots> textures{};
    textures[size_t(MaterialSlot::WaterNormal)] = ripples;
    buildDrawMaterial(*water, textures, {});
    assets.addMaterial("water", std::move(water));
}
}   // namespace shockwave
