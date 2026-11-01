#pragma once

// The Vulkan 1.3 renderer: forward PBR with cascaded and local-light shadows, a render
// graph, bindless textures and Dear ImGui. Loaded as the "vulkan" renderer plugin.

#include <Renderer/RenderBackend.h>

#include <memory>

namespace shockwave {

class VulkanRenderer final : public RenderBackend {
public:
    VulkanRenderer(Window& window, JobSystem& jobs, bool vsync);
    ~VulkanRenderer() override;

    uint32_t maxMeshes() const override;
    uint32_t maxTextures() const override;
    void createMesh(MeshHandle id, std::span<const Vertex> vertices, std::span<const uint32_t> indices) override;
    void destroyMesh(MeshHandle id) override;
    void setMeshSkin(MeshHandle id, std::span<const VertexSkin> skin) override;
    void
    createTexture(TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) override;
    void
    updateTexture(TextureHandle id, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) override;
    void destroyTexture(TextureHandle id) override;
    uint64_t guiTexture(TextureHandle id) override;

    void setMeshLods(MeshHandle id, std::span<const MeshLod> lods, std::span<const uint32_t> lodIndices) override;
    bool supportsTextureFormat(TextureFormat format) const override;
    bool supportsTextureShape(TextureShape shape) const override;
    void setReflectionProbes(std::span<const ReflectionProbeDesc> probes) override;
    void setLocalReflections(bool enabled) override;
    void drawDecals(std::span<const DecalDesc> decals) override;
    void setDecalQuality(DecalQuality quality) override;
    DecalStats decalStats() const override;
    void setLightCulling(LightCulling culling) override;
    void setParticleSettings(const ParticleSettings& settings) override;
    ClusterStats clusterStats() const override;
    bool requestSceneCapture(std::string& error) override;
    bool sceneCapturePending() const override;
    SceneCapture takeSceneCapture() override;
    void createTextureData(TextureHandle id, const TextureData& data) override;
    void setMeshUv1(MeshHandle id, std::span<const glm::vec2> uv1) override;
    void setMeshLightmapUv(MeshHandle id, std::span<const glm::vec2> uv) override;
    void createSampler(SamplerHandle id, const SamplerDesc& desc) override;
    void setAnisotropy(uint32_t anisotropy) override;
    void setMaterialQuality(MaterialQuality quality) override;
    void updateTextureData(TextureHandle id, const TextureData& data) override;
    void setLodSettings(const LodSettings& settings) override;
    LodStats lodStats() const override;

    void setClearColor(const Color& color) override;
    void setCamera(const glm::mat4& view, const glm::mat4& projection) override;
    void setView(RenderViewId id) override;
    void cutCamera() override;
    void drawMesh(
        MeshHandle mesh, const glm::mat4& transform, TextureHandle texture, const glm::vec4& tint,
        const DrawMaterial& material, const RenderInstanceKey& key) override;
    void addLight(const LightDesc& light) override;
    void drawParticles(const ParticleDrawList& particles) override;
    void drawSkinnedMesh(
        MeshHandle mesh, const glm::mat4& transform, std::span<const glm::mat4> joints, TextureHandle texture,
        const glm::vec4& tint, const DrawMaterial& material, const RenderInstanceKey& key) override;

    void setGrid(const GridSettings& grid) override;
    void setLighting(const LightingDesc& lighting) override;
    void setDebugView(DebugView view) override;
    void setShadowQuality(ShadowQuality quality) override;
    void setAntiAliasing(AntiAliasing aa) override;
    void setAmbientOcclusion(AmbientOcclusion ao) override;
    void setBloom(BloomQuality quality, float scale) override;
    void setRenderScale(float scale) override;
    void setViewport(uint32_t width, uint32_t height) override;
    void setVsync(bool vsync) override;
    void setLowLatency(bool lowLatency) override;

    uint32_t lightCount() const override;
    uint32_t droppedLights() const override;
    ShadowStats shadowStats() const override;
    std::string frameGraph() const override;
    uint32_t renderWidth() const override;
    uint32_t renderHeight() const override;
    uint64_t sceneTexture() const override;
    const std::vector<PassTiming>& passTimings() const override;
    float gpuFrameMs() const override;
    float aspectRatio() const override;
    const char* gpuName() const override;
    const GpuChoice* gpuChoice() const override;
    uint32_t drawCount() const override;
    uint32_t culledDraws() const override;
    ParticleStats particleStats() const override;
    void setPostProcess(const PostProcessDesc& desc) override;
    void setPostProcessSettings(const PostProcessSettings& settings) override;
    PostProcessStats postProcessStats() const override;
    void setShadingSettings(const ShadingSettings& settings) override;
    ShadingStats shadingStats() const override;
    uint32_t unkeyedDraws() const override;
    TemporalStats temporalStats() const override;

    RendererCapabilities capabilities() const override;
    std::vector<EffectiveSetting> effectiveSettings() const override;
    RendererMemoryReport memoryReport() const override;

    void requestScreenshot(const std::string& path) override;
    bool screenshotPending() const override;
    const std::string& screenshotResult() const override;

    float waitForPresent() override;
    const char* presentTiming() const override;
    const char* presentModeName() const override;

    bool beginFrame() override;
    void endFrame() override;
    void onResize() override;
    void waitIdle() override;
    void initGui() override;
    void shutdownGui() override;
    void newGuiFrame() override;
    void renderGuiWindows() override;

    glm::uvec2 uiOutputSize() const override;
    void drawUi(const UiDrawList& list) override;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

}   // namespace shockwave
