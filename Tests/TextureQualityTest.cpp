// Texture quality (A01) and material bindings (G02): the mip arithmetic, CPU tails, the
// sampler cache, and residency changes while frames run. Runs on the renderer named on
// the command line ("null" by default; "vulkan" in the gpu-labelled test), so the same
// transitions are checked with and without real images being swapped.

#include <Assets/ImportSettings.h>
#include <Engine/Engine.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::vector<uint8_t> pixels(uint32_t width, uint32_t height, uint8_t a, uint8_t b) {
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t v = (x + y) % 2 ? a : b;
            uint8_t* p = rgba.data() + (size_t(y) * width + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    return rgba;
}

void mathChecks() {
    // Exact BC7 bytes (docs/Graphics/01-SettingsAndTextures.md 1.11).
    check(textureChainBytes(TextureFormat::BC7, 4096, 4096) == 22369648, "BC7 4096 chain");
    check(textureChainBytes(TextureFormat::BC7, 4096, 4096, 3) == 349552, "BC7 chain from 512");
    check(textureChainBytes(TextureFormat::BC7, 1, 1) == 16, "a 1x1 BC7 level is one block");
    check(textureChainBytes(TextureFormat::RGBA8, 4, 2) == 4 * 2 * 4 + 2 * 1 * 4 + 1 * 1 * 4, "RGBA8 4x2 chain");
    check(textureMipCount(4096, 1024) == 13 && textureMipCount(1, 1) == 1, "mip counts");
    // Floors, the last level, and the 32-pixel minimum.
    check(residentFirstMip(3, 4096, 4096, 13) == 3, "low on 4096 starts at 512");
    check(residentFirstMip(3, 64, 64, 7) == 1, "never below 32 on the longer side");
    check(residentFirstMip(3, 16, 16, 5) == 0, "already small textures stay whole");
    check(residentFirstMip(3, 4096, 4096, 2) == 1, "never past the last level");
    check(residentFirstMip(0, 4096, 4096, 13) == 0, "ultra keeps everything");
    check(residentLod(5.0f, 3) == 2.0f && residentLod(1.0f, 3) == 0.0f, "explicit LOD maps to resident levels");
    for (auto q : {TextureQuality::Low, TextureQuality::Medium, TextureQuality::High, TextureQuality::Ultra})
        check(textureQualityFirstMip(q) == 3u - uint32_t(q), std::string("floor for ") + textureQualityName(q));

    // CPU tails average in linear light: black and white make sRGB 188, not 128.
    TextureData source;
    source.width = source.height = 2;
    source.srgb = true;
    source.bytes = pixels(2, 2, 0, 255);
    source.levels = {{2, 2, 0, source.bytes.size()}};
    const TextureData tail = textureTail(source, 1);
    check(tail.width == 1 && tail.height == 1 && tail.levels.size() == 1, "tail of 2x2 is 1x1");
    check(std::abs(int(tail.bytes[0]) - 188) <= 1 && tail.bytes[3] == 255, "sRGB tail averages in linear light");
    source.srgb = false;
    check(std::abs(int(textureTail(source, 1).bytes[0]) - 128) <= 1, "linear data averages directly");
    // A texture with its own levels is sliced, not rebuilt.
    TextureData chain;
    chain.format = TextureFormat::RGBA8;
    chain.width = chain.height = 4;
    for (uint32_t size = 4, offset = 0; size; size /= 2) {
        chain.levels.push_back({size, size, offset, size_t(size) * size * 4});
        for (uint32_t i = 0; i < size * size * 4; ++i) chain.bytes.push_back(uint8_t(size));
        offset += size * size * 4;
    }
    const TextureData sliced = textureTail(chain, 1);
    check(sliced.width == 2 && sliced.levels.size() == 2 && sliced.levels[0].offset == 0, "slices keep levels");
    check(sliced.bytes.size() == 2 * 2 * 4 + 4 && sliced.bytes[0] == 2 && sliced.bytes.back() == 1, "slice data");
    check(textureTail(chain, 0).bytes == chain.bytes, "level 0 returns the texture unchanged");

    // UV transforms: identity is exact, and rotation goes the way glTF's sample renderer does.
    UvTransform t;
    check(t.identity() && t.apply({0.3f, 0.7f}) == glm::vec2(0.3f, 0.7f), "identity transform");
    t.rotation = 1.5707963f;
    const glm::vec2 turned = t.apply({1.0f, 0.0f});
    check(std::abs(turned.x) < 1e-6f && std::abs(turned.y + 1.0f) < 1e-6f, "rotation maps +u to -v");
    t = {};
    t.offset = {0.5f, 0.25f};
    t.scale = {2.0f, -1.0f};
    check(t.apply({1.0f, 1.0f}) == glm::vec2(2.5f, -0.75f), "scale then offset");

    // .swimport categories.
    TextureCategory category = TextureCategory::Material;
    check(parseTextureCategory("ui", category) && category == TextureCategory::Ui, "category ui parses");
    check(!parseTextureCategory("font", category), "unknown categories are refused");
}

class QualityApp final : public Application {
public:
    std::vector<TextureHandle> material;
    TextureHandle fixed, doomed;
    MeshHandle mesh;
    int phase = 0;
    std::chrono::steady_clock::time_point phaseStart = std::chrono::steady_clock::now();

    bool settled() { return engine().renderer().textureResidency().pending == 0; }
    uint32_t firstMip(TextureHandle t) {
        uint32_t w = 0, h = 0, first = 99;
        engine().renderer().textureResidency(t, w, h, first);
        return first;
    }
    void onStart() override {
        Renderer& r = engine().renderer();
        mesh = r.createMesh(makeCube());
        const auto big = pixels(256, 256, 30, 220), mid = pixels(64, 64, 30, 220), small = pixels(16, 16, 30, 220);
        material = {
            r.createTexture(256, 256, big, true, TextureUsage::Material),
            r.createTexture(64, 64, mid, false, TextureUsage::Material),
            r.createTexture(16, 16, small, true, TextureUsage::Material)};
        fixed = r.createTexture(256, 256, big, true, TextureUsage::Fixed);
        doomed = r.createTexture(256, 256, big, true, TextureUsage::Material);
        const auto stats = r.textureResidency();
        check(stats.textures == 4 && stats.reduced == 0 && stats.pending == 0, "starts whole at ultra");
        check(stats.residentBytes == stats.fullBytes, "ultra resident bytes are the full chains");

        // Samplers: cached by description, 0 is the default, anisotropy reaches every one.
        check(r.sampler(SamplerDesc{}).id == 0, "the default sampler is handle 0");
        SamplerDesc pixelArt;
        pixelArt.magFilter = pixelArt.minFilter = TextureFilter::Nearest;
        pixelArt.mipmaps = false;
        pixelArt.wrapU = TextureWrap::Clamp;
        const SamplerHandle a = r.sampler(pixelArt), b = r.sampler(pixelArt);
        check(a.id != 0 && a == b, "equal descriptions share one sampler");
        check(r.samplerDesc(a) == pixelArt, "the cache keeps the description");
        r.setAnisotropy(4);
        check(r.anisotropy() == 4, "anisotropy is kept");

        r.setTextureQuality(TextureQuality::Low);
        r.destroyTexture(doomed);   // destroyed while its change is queued
    }
    void onRender(Renderer& r, double) override {
        // Draw with every texture so swaps happen while they're in use.
        for (size_t i = 0; i < material.size(); ++i)
            r.drawMesh(mesh, glm::translate(glm::mat4(1.0f), glm::vec3(float(i), 0, -5)), material[i]);
        r.drawMesh(mesh, glm::translate(glm::mat4(1.0f), glm::vec3(-1, 0, -5)), fixed);
    }
    void onFrame(double) override {
        Renderer& r = engine().renderer();
        if (!settled()) {
            // By the clock: the headless renderer runs thousands of frames a second.
            if (std::chrono::steady_clock::now() - phaseStart > std::chrono::seconds(20)) {
                const auto stats = r.textureResidency();
                check(
                    false, "residency never settled (phase " + std::to_string(phase) + ", " +
                               std::to_string(stats.pending) + " pending)");
                engine().quit();
            }
            return;
        }
        switch (phase) {
        case 0: {   // low
            check(
                firstMip(material[0]) == 3 && firstMip(material[1]) == 1 && firstMip(material[2]) == 0,
                "low drops levels down to the 32-pixel minimum");
            check(firstMip(fixed) == 0, "fixed textures keep every level");
            const auto stats = r.textureResidency();
            check(stats.reduced == 2 && stats.residentBytes < stats.fullBytes / 4, "low uses a fraction of the memory");
            // A reload at low comes back at low.
            r.updateTexture(material[0], 512, 512, pixels(512, 512, 10, 240));
            check(firstMip(material[0]) == 3, "a reloaded texture keeps the current quality");
            // Rapid changes in one frame: only the last one counts.
            r.setTextureQuality(TextureQuality::Ultra);
            r.setTextureQuality(TextureQuality::Medium);
            r.setTextureQuality(TextureQuality::High);
            r.setAnisotropy(16);
            break;
        }
        case 1:   // high
            check(
                r.textureQuality() == TextureQuality::High && firstMip(material[0]) == 1 && firstMip(material[1]) == 1,
                "the last of several changes wins");
            // A change while the previous one is still running.
            r.setTextureQuality(TextureQuality::Low);
            break;
        case 2:   // low, then straight back
            r.setTextureQuality(TextureQuality::Ultra);
            break;
        case 3: {   // ultra
            const auto stats = r.textureResidency();
            check(
                stats.reduced == 0 && stats.residentBytes == stats.fullBytes,
                "back to ultra restores every level (" + std::to_string(stats.reduced) + " reduced, " +
                    std::to_string(stats.residentBytes) + " of " + std::to_string(stats.fullBytes) + " bytes)");
            check(firstMip(material[0]) == 0, "ultra keeps the reloaded size");
            engine().quit();
            break;
        }
        }
        ++phase;
        phaseStart = std::chrono::steady_clock::now();
    }
};
}   // namespace

int main(int argc, char** argv) {
    mathChecks();
    EngineConfig config;
    config.window = {"texture quality test", 320, 240, false, true};
    config.renderer = argc > 1 ? argv[1] : "null";
    config.maxFrames = 0;   // until the last phase quits
    config.showFpsInTitle = false;
    Engine engine(config);
    QualityApp app;
    engine.run(app);
    check(app.phase == 4, "every phase ran (stopped at " + std::to_string(app.phase) + ")");
    if (failures) return 1;
    std::printf("texture quality: all checks passed on %s\n", config.renderer.c_str());
    return 0;
}
