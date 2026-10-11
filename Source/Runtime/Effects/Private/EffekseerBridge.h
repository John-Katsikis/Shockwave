#pragma once

// The bridge between Effekseer and Shockwave's renderers. Effekseer's node renderers
// (sprites, ribbons, rings, tracks and models) fill vertex arrays on the CPU; this
// "capture" renderer collects them as world-space triangles in a ParticleDrawList that
// any RenderBackend can draw. No Effekseer graphics backend, shader or GPU resource is
// involved, which is what keeps effects working across renderer plugins and hot swaps.
//
// Main thread only, like Effects.

#include <Renderer/Particles.h>
#include <Renderer/Renderer.h>

#include <Effekseer.h>
#include <EffekseerRenderer.ModelRendererBase.h>
#include <EffekseerRenderer.Renderer.h>
#include <EffekseerRenderer.Renderer_Impl.h>
#include <EffekseerRenderer.RenderStateBase.h>
#include <EffekseerRenderer.StandardRenderer.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace shockwave::effects {

class CaptureRenderer;

// A texture Effekseer loaded, living in the renderer's bindless table. Effekseer releases
// textures when it frees an effect, possibly mid-simulation on a worker, so the handle is
// queued and freed on the main thread instead of here (TextureGraveyard).
class TextureGraveyard {
public:
    void bury(TextureHandle handle);
    // Main thread, between frames.
    void flush(Renderer* renderer);

private:
    std::mutex m_mutex;
    std::vector<TextureHandle> m_handles;
};

class BridgeTexture final : public Effekseer::Backend::Texture {
public:
    BridgeTexture(TextureHandle handle, int32_t width, int32_t height, TextureGraveyard* graveyard);
    ~BridgeTexture() override;
    TextureHandle handle() const { return m_handle; }

private:
    TextureHandle m_handle;
    TextureGraveyard* m_graveyard;
};

// Creates BridgeTextures; Effekseer asks it for its white and flat-normal stand-ins.
class BridgeDevice final : public Effekseer::Backend::GraphicsDevice {
public:
    BridgeDevice(Renderer* renderer, TextureGraveyard* graveyard) : m_renderer(renderer), m_graveyard(graveyard) {}
    Effekseer::Backend::TextureRef CreateTexture(
        const Effekseer::Backend::TextureParameter& param,
        const Effekseer::CustomVector<uint8_t>& initialData) override;
    // RGBA8 pixels, rows top to bottom.
    Effekseer::Backend::TextureRef
    createTexture(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, bool srgb);
    std::string GetDeviceName() const override { return "Shockwave"; }

private:
    Renderer* m_renderer;
    TextureGraveyard* m_graveyard;
};

// Files an effect pulled in while loading, and what failed: for hot reload and
// validation.
struct LoadReport {
    std::vector<std::filesystem::path> files;
    std::vector<std::string> missing;   // "texture Texture/fire.png", ...
};

// Reads textures with Shockwave's image decoder (PNG, JPEG, TGA...) into the renderer.
class BridgeTextureLoader final : public Effekseer::TextureLoader {
public:
    explicit BridgeTextureLoader(BridgeDevice* device) : m_device(device) {}
    Effekseer::TextureRef Load(const char16_t* path, Effekseer::TextureType textureType) override;
    void Unload(Effekseer::TextureRef) override {}
    LoadReport* report = nullptr;   // set while an effect loads

private:
    BridgeDevice* m_device;
};

// .efkmodel files: Effekseer keeps the vertices on the CPU, which is all the bridge needs.
class BridgeModelLoader final : public Effekseer::ModelLoader {
public:
    Effekseer::ModelRef Load(const char16_t* path) override;
    Effekseer::ModelRef Load(const void* data, int32_t size) override;
    void Unload(Effekseer::ModelRef) override {}
    LoadReport* report = nullptr;
};

// Stands in for Effekseer's StandardRenderer: node renderers ask it for vertex memory
// with the state to draw it with; at each flush the vertices become ParticleVertex
// triangles and a batch. Same duck-typed interface as EffekseerRenderer::StandardRenderer.
class CaptureStandardRenderer {
public:
    explicit CaptureStandardRenderer(CaptureRenderer* renderer) : m_renderer(renderer) {}
    void BeginRenderingAndRenderingIfRequired(
        const EffekseerRenderer::StandardRendererState& state, int32_t count, int& stride, void*& data);
    void ResetAndRenderingIfRequired() { flush(); }
    EffekseerRenderer::StandardRendererState& GetState() { return m_chunks.back().state; }
    // Converts every pending chunk into the capture.
    void flush();

private:
    struct Chunk {
        EffekseerRenderer::StandardRendererState state;
        size_t offset;
        int32_t count;
        int32_t stride;
    };
    CaptureRenderer* m_renderer;
    std::vector<uint8_t> m_scratch;
    size_t m_used = 0;
    std::vector<Chunk> m_chunks;
};

// What a batch is drawn with, taken from a node's parameters.
struct BatchState {
    EffekseerRenderer::RendererShaderType shader = EffekseerRenderer::RendererShaderType::Unlit;
    const EffekseerRenderer::ShaderParameterCollector* collector = nullptr;
    Effekseer::AlphaBlendType blend = Effekseer::AlphaBlendType::Blend;
    Effekseer::CullingType cull = Effekseer::CullingType::Double;
    bool depthTest = true, depthWrite = false;
    float emissive = 1.0f;
    float edgeThreshold = 0.0f, edgeScale = 1.0f;
    uint8_t edgeColor[4] = {0, 0, 0, 0};
    EffekseerRenderer::RendererStateFlipbook flipbook;
    bool materialFile = false;   // a custom .efkmat material
};

class CaptureRenderer final : public EffekseerRenderer::Renderer, public Effekseer::ReferenceObject {
public:
    static constexpr int32_t kSquareMax = 65536;   // quads per node renderer call

    CaptureRenderer(::shockwave::Renderer* renderer, TextureGraveyard* graveyard);
    ~CaptureRenderer() override;

    // The frame's capture: cleared by begin(), filled by Manager::Draw / DrawHandle.
    void begin(const glm::mat4& view, const glm::mat4& projection);
    void end();
    void setDepth(float depth) { m_depth = depth; }
    ParticleDrawList drawList() const;
    uint32_t skippedBatches() const { return m_skipped; }

    // Appends quads (4 vertices each, Effekseer's layout for the shader type) as triangles.
    void addQuads(const BatchState& state, const uint8_t* vertices, int32_t stride, int32_t vertexCount);
    // Appends a model's triangles for one instance.
    void addModel(
        const BatchState& state, const Effekseer::Model& model, int32_t frame, const Effekseer::Matrix44& world,
        const Effekseer::RectF& uv, const Effekseer::RectF& alphaUv, float flipbookIndex, float alphaThreshold,
        const Effekseer::Color& color);

    BridgeDevice& device() { return *m_device.Get(); }

    // ---- What Effekseer's node renderer templates call ----
    CaptureStandardRenderer* GetStandardRenderer() { return &m_standard; }
    int32_t GetSquareMaxCount() const override { return kSquareMax; }
    void OnLostDevice() override {}
    void OnResetDevice() override {}
    void SetRestorationOfStatesFlag(bool) override {}
    bool BeginRendering() override { return true; }
    bool EndRendering() override;
    ::Effekseer::SpriteRendererRef CreateSpriteRenderer() override;
    ::Effekseer::RibbonRendererRef CreateRibbonRenderer() override;
    ::Effekseer::RingRendererRef CreateRingRenderer() override;
    ::Effekseer::ModelRendererRef CreateModelRenderer() override;
    ::Effekseer::TrackRendererRef CreateTrackRenderer() override;
    ::Effekseer::TextureLoaderRef CreateTextureLoader(::Effekseer::FileInterfaceRef) override;
    ::Effekseer::ModelLoaderRef CreateModelLoader(::Effekseer::FileInterfaceRef) override;
    ::Effekseer::MaterialLoaderRef CreateMaterialLoader(::Effekseer::FileInterfaceRef) override { return nullptr; }
    void ResetRenderState() override {}
    EffekseerRenderer::DistortingCallback* GetDistortingCallback() override { return nullptr; }
    void SetDistortingCallback(EffekseerRenderer::DistortingCallback*) override {}
    Effekseer::Backend::GraphicsDeviceRef GetGraphicsDevice() const override;

    int GetRef() override { return ReferenceObject::GetRef(); }
    int AddRef() override { return ReferenceObject::AddRef(); }
    int Release() override { return ReferenceObject::Release(); }

private:
    TextureHandle textureOf(const Effekseer::Backend::TextureRef& texture) const;
    // Starts a batch for the state, or extends the last one; false when it's skipped.
    bool beginBatch(const BatchState& state, uint32_t triangles);

    ::shockwave::Renderer* m_renderer;
    Effekseer::RefPtr<BridgeDevice> m_device;
    CaptureStandardRenderer m_standard{this};
    std::vector<ParticleVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<ParticleBatch> m_batches;
    glm::mat4 m_view{1.0f};
    float m_depth = 0.0f;
    uint32_t m_skipped = 0;
};

}   // namespace shockwave::effects
