// The headless renderer: accepts everything and draws nothing. For dedicated servers,
// tests and tools that run game logic without a GPU, and as the smallest example of a
// renderer plugin (see Renderer/RenderBackend.h).

#include <Core/Plugin.h>
#include <Platform/Window.h>
#include <Renderer/RenderBackend.h>

#include <algorithm>
#include <atomic>
#include <string>

namespace shockwave {
namespace {

class NullRenderer final : public RenderBackend {
public:
    explicit NullRenderer(Window& window) : m_window(window) {}

    uint32_t maxMeshes() const override { return 1u << 20; }
    uint32_t maxTextures() const override { return 1u << 20; }
    void createMesh(MeshHandle, std::span<const Vertex>, std::span<const uint32_t>) override {}
    void destroyMesh(MeshHandle) override {}
    void setMeshSkin(MeshHandle, std::span<const VertexSkin>) override {}
    void createTexture(TextureHandle, uint32_t, uint32_t, std::span<const uint8_t>, bool) override {}
    void updateTexture(TextureHandle, uint32_t, uint32_t, std::span<const uint8_t>, bool) override {}
    void destroyTexture(TextureHandle) override {}
    uint64_t guiTexture(TextureHandle) override { return 0; }

    void setClearColor(const Color&) override {}
    void setCamera(const glm::mat4&, const glm::mat4&) override {}
    void drawMesh(
        MeshHandle, const glm::mat4&, TextureHandle, const glm::vec4&, const DrawMaterial&,
        const RenderInstanceKey& key) override {
        m_draws.fetch_add(1, std::memory_order_relaxed);
        if (!key.valid()) m_unkeyed.fetch_add(1, std::memory_order_relaxed);
    }
    void drawSkinnedMesh(
        MeshHandle, const glm::mat4&, std::span<const glm::mat4>, TextureHandle, const glm::vec4&, const DrawMaterial&,
        const RenderInstanceKey& key) override {
        m_draws.fetch_add(1, std::memory_order_relaxed);
        if (!key.valid()) m_unkeyed.fetch_add(1, std::memory_order_relaxed);
    }
    void addLight(const LightDesc&) override { m_lights.fetch_add(1, std::memory_order_relaxed); }
    void drawParticles(const ParticleDrawList&) override {}

    void setGrid(const GridSettings&) override {}
    void setLighting(const LightingDesc&) override {}
    void setDebugView(DebugView) override {}
    void setShadowQuality(ShadowQuality quality) override { m_shadows = quality; }
    void setAntiAliasing(AntiAliasing aa) override { m_antiAliasing = aa; }
    void setBloom(BloomQuality quality, float) override { m_bloom = quality; }
    void setAnisotropy(uint32_t anisotropy) override { m_anisotropy = anisotropy; }
    void setMaterialQuality(MaterialQuality quality) override { m_material = quality; }
    void setAmbientOcclusion(AmbientOcclusion ao) override { m_ao = ao; }
    void setRenderScale(float scale) override { m_scale = scale; }
    void setViewport(uint32_t width, uint32_t height) override {
        m_viewportWidth = width;
        m_viewportHeight = height;
    }
    void setVsync(bool vsync) override { m_vsync = vsync; }
    void setLowLatency(bool lowLatency) override { m_lowLatency = lowLatency; }

    uint32_t lightCount() const override { return std::min(m_lastLights, kMaxLights); }
    uint32_t droppedLights() const override { return m_lastLights > kMaxLights ? m_lastLights - kMaxLights : 0; }
    ShadowStats shadowStats() const override { return {}; }
    std::string frameGraph() const override { return "The null renderer draws nothing.\n"; }
    uint32_t renderWidth() const override { return static_cast<uint32_t>(static_cast<float>(outputWidth()) * m_scale); }
    uint32_t renderHeight() const override {
        return static_cast<uint32_t>(static_cast<float>(outputHeight()) * m_scale);
    }
    uint64_t sceneTexture() const override { return 0; }
    const std::vector<PassTiming>& passTimings() const override { return m_timings; }
    float gpuFrameMs() const override { return 0.0f; }
    float aspectRatio() const override {
        return outputHeight() ? static_cast<float>(outputWidth()) / static_cast<float>(outputHeight()) : 1.0f;
    }
    const char* gpuName() const override { return "none (null renderer)"; }
    uint32_t drawCount() const override { return m_lastDraws; }
    uint32_t unkeyedDraws() const override { return m_lastUnkeyed; }

    RendererCapabilities capabilities() const override {
        RendererCapabilities caps;
        caps.backend = "null";
        caps.features.push_back({"presentation", false, "headless: nothing is drawn or shown"});
        return caps;
    }
    std::vector<EffectiveSetting> effectiveSettings() const override {
        const char* why = "the null renderer draws nothing";
        auto onOff = [](bool on) { return std::string(on ? "on" : "off"); };
        return {
            {"render_scale", std::to_string(m_scale), std::to_string(m_scale), "", ""},
            {"shadow_quality", shadowName(m_shadows), "off", m_shadows == ShadowQuality::Off ? "" : why, ""},
            {"vsync", onOff(m_vsync), "off", m_vsync ? why : "", ""},
            {"low_latency", onOff(m_lowLatency), "off", m_lowLatency ? why : "", ""},
            {"anti_aliasing", antiAliasingName(m_antiAliasing), "none", m_antiAliasing == AntiAliasing::None ? "" : why,
             ""},
            {"bloom_quality", bloomQualityName(m_bloom), "off", m_bloom == BloomQuality::Off ? "" : why, ""},
            {"anisotropy", std::to_string(m_anisotropy), "1", m_anisotropy == 1 ? "" : why, ""},
            {"material_quality", materialQualityName(m_material), "low", m_material == MaterialQuality::Low ? "" : why,
             ""},
            {"ambient_occlusion", ambientOcclusionName(m_ao), "off", m_ao == AmbientOcclusion::Off ? "" : why, ""},
        };
    }

    void requestScreenshot(const std::string&) override {
        m_screenshotResult = "the null renderer draws nothing, so there's no picture to save; use --renderer vulkan";
    }
    bool screenshotPending() const override { return false; }
    const std::string& screenshotResult() const override { return m_screenshotResult; }

    float waitForPresent() override { return 0.0f; }
    const char* presentTiming() const override { return "none"; }
    const char* presentModeName() const override { return "none (headless)"; }

    bool beginFrame() override { return true; }
    void endFrame() override {
        m_lastDraws = m_draws.exchange(0);
        m_lastUnkeyed = m_unkeyed.exchange(0);
        m_lastLights = m_lights.exchange(0);
    }
    void onResize() override {}
    void waitIdle() override {}
    // Never called: without kRendererDrawsGui the engine doesn't hand it a GUI.
    void initGui() override {}
    void shutdownGui() override {}
    void newGuiFrame() override {}
    void renderGuiWindows() override {}

    // Game UI is laid out at the window's size and then dropped.
    glm::uvec2 uiOutputSize() const override { return {outputWidth(), outputHeight()}; }
    void drawUi(const UiDrawList&) override {}

private:
    uint32_t outputWidth() const { return m_viewportWidth ? m_viewportWidth : m_window.pixelWidth(); }
    uint32_t outputHeight() const { return m_viewportHeight ? m_viewportHeight : m_window.pixelHeight(); }

    static const char* shadowName(ShadowQuality quality) {
        constexpr const char* names[] = {"off", "low", "medium", "high", "ultra"};
        return names[std::min<uint32_t>(static_cast<uint32_t>(quality), 4)];
    }

    Window& m_window;
    std::atomic<uint32_t> m_draws{0}, m_lights{0}, m_unkeyed{0};
    uint32_t m_lastDraws = 0, m_lastLights = 0, m_lastUnkeyed = 0;
    ShadowQuality m_shadows = ShadowQuality::High;
    AntiAliasing m_antiAliasing = AntiAliasing::None;
    uint32_t m_anisotropy = 16;
    MaterialQuality m_material = MaterialQuality::High;
    AmbientOcclusion m_ao = AmbientOcclusion::Off;
    BloomQuality m_bloom = BloomQuality::High;
    bool m_vsync = true, m_lowLatency = false;
    float m_scale = 1.0f;
    uint32_t m_viewportWidth = 0, m_viewportHeight = 0;
    std::vector<PassTiming> m_timings;
    std::string m_screenshotResult;
};

std::unique_ptr<RenderBackend> create(const RenderBackendDesc& desc) {
    return std::make_unique<NullRenderer>(desc.window);
}

const RendererPluginApi api{.flags = 0, .create = create};
const PluginDesc plugin{
    .kind = "renderer",
    .name = "null",
    .description = "Headless: draws nothing and needs no GPU",
    .api = &api,
};

}   // namespace
}   // namespace shockwave

SHOCKWAVE_PLUGIN(shockwave::plugin)
