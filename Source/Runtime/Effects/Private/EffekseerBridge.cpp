#include "EffekseerBridge.h"

#include <Assets/Image.h>

#include <EffekseerRenderer.RibbonRendererBase.h>
#include <EffekseerRenderer.RingRendererBase.h>
#include <EffekseerRenderer.SpriteRendererBase.h>
#include <EffekseerRenderer.TrackRendererBase.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace shockwave::effects {

namespace fs = std::filesystem;
using EffekseerRenderer::RendererShaderType;

namespace {

fs::path toPath(const char16_t* path) {
    char utf8[1024];
    Effekseer::ConvertUtf16ToUtf8(utf8, static_cast<int32_t>(sizeof(utf8)), path);
    return fs::path(reinterpret_cast<const char8_t*>(utf8));
}

bool readFile(const fs::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

uint32_t packColor(const Effekseer::Color& c) {
    return uint32_t(c.R) | uint32_t(c.G) << 8 | uint32_t(c.B) << 16 | uint32_t(c.A) << 24;
}

uint32_t packColor(const Effekseer::Color& a, const Effekseer::Color& b) {   // a * b
    auto mul = [](uint8_t x, uint8_t y) { return uint32_t((uint32_t(x) * uint32_t(y) + 127) / 255); };
    return mul(a.R, b.R) | mul(a.G, b.G) << 8 | mul(a.B, b.B) << 16 | mul(a.A, b.A) << 24;
}

uint32_t packNormal(const glm::vec3& n) {
    const glm::vec3 len = glm::length(n) > 0.0f ? glm::normalize(n) : glm::vec3(0, 1, 0);
    const glm::vec3 u = glm::clamp((len + 1.0f) * 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f;
    return uint32_t(u.x) | uint32_t(u.y) << 8 | uint32_t(u.z) << 16 | 255u << 24;
}

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// Effekseer's flipbook blending, from its vertex shader (FlipbookInterpolationUtils.fx):
// where the next frame is in the texture, and how far along to it this one is.
void flipbook(
    const EffekseerRenderer::RendererStateFlipbook& fb, float index, glm::vec2 uv, glm::vec2& nextUv, float& rate) {
    nextUv = uv;
    rate = 0.0f;
    if (!fb.EnableInterpolation || fb.FlipbookDivideX <= 0 || fb.FlipbookDivideY <= 0) return;
    rate = index - std::floor(index);
    float current = std::floor(index);
    float next = current + 1.0f;
    const float count = float(fb.FlipbookDivideX * fb.FlipbookDivideY);
    if (fb.UVLoopType == 0) {   // play once
        if (next >= count) next = current = count - 1.0f;
    } else if (fb.UVLoopType == 1) {   // loop
        current = std::fmod(current, count);
        next = std::fmod(next, count);
    } else if (fb.UVLoopType == 2) {   // loop back and forth
        auto bounce = [count](float i) {
            const bool reverse = int(std::floor(i / count)) % 2 == 1;
            i = std::fmod(i, count);
            return reverse ? count - 1.0f - std::floor(i) : i;
        };
        current = bounce(current);
        next = bounce(next);
    }
    const int divideX = fb.FlipbookDivideX;
    const glm::vec2 oneSize(fb.OneSizeX, fb.OneSizeY), offset(fb.OffsetX, fb.OffsetY);
    auto cell = [&](float i) { return glm::vec2(float(int(i) % divideX), float(int(i) / divideX)); };
    const glm::vec2 origin = uv - (cell(current) * oneSize + offset);
    nextUv = origin + cell(next) * oneSize + offset;
}

// Effekseer's texture slots (ShaderParameterCollector::Collect): colour first; then the
// normal map for lit shaders or the background for distortion; then the alpha texture.
int alphaTextureSlot(RendererShaderType shader) {
    if (shader == RendererShaderType::AdvancedUnlit) return 1;
    if (shader == RendererShaderType::AdvancedLit || shader == RendererShaderType::AdvancedBackDistortion) return 2;
    return -1;
}

bool advanced(RendererShaderType shader) {
    return shader == RendererShaderType::AdvancedUnlit || shader == RendererShaderType::AdvancedLit ||
           shader == RendererShaderType::AdvancedBackDistortion;
}

bool distortionShader(RendererShaderType shader) {
    return shader == RendererShaderType::BackDistortion || shader == RendererShaderType::AdvancedBackDistortion;
}

int alphaTextureSlot(RendererShaderType shader);

bool drawable(const BatchState& state) {
    return !state.materialFile &&
           (state.shader == RendererShaderType::Unlit || state.shader == RendererShaderType::Lit ||
            state.shader == RendererShaderType::AdvancedUnlit || state.shader == RendererShaderType::AdvancedLit ||
            distortionShader(state.shader));
}

// One vertex, whatever Effekseer layout it came in.
struct Decoded {
    glm::vec3 position;
    Effekseer::Color color;
    uint32_t normal = 0, tangent = 0;
    glm::vec2 uv{0.0f}, alphaUv{0.0f};
    float flipbookIndex = 0.0f, alphaThreshold = 0.0f;
};

template <class V>
Decoded decodeAs(const uint8_t* data) {
    V v;
    std::memcpy(&v, data, sizeof(V));
    Decoded d;
    d.position = {v.Pos.X, v.Pos.Y, v.Pos.Z};
    d.color = v.Col;
    d.uv = {v.UV[0], v.UV[1]};
    if constexpr (requires { v.Normal; }) d.normal = packColor(v.Normal);
    if constexpr (requires { v.Tangent; }) d.tangent = packColor(v.Tangent);
    if constexpr (requires { v.AlphaUV; }) {
        d.alphaUv = {v.AlphaUV[0], v.AlphaUV[1]};
        d.flipbookIndex = v.FlipbookIndexAndNextRate;
        d.alphaThreshold = v.AlphaThreshold;
    }
    return d;
}

Decoded decode(RendererShaderType shader, const uint8_t* data) {
    switch (shader) {
    case RendererShaderType::Lit:
    case RendererShaderType::BackDistortion: return decodeAs<EffekseerRenderer::LightingVertex>(data);
    case RendererShaderType::AdvancedUnlit: return decodeAs<EffekseerRenderer::AdvancedSimpleVertex>(data);
    case RendererShaderType::AdvancedLit:
    case RendererShaderType::AdvancedBackDistortion: return decodeAs<EffekseerRenderer::AdvancedLightingVertex>(data);
    default: return decodeAs<EffekseerRenderer::SimpleVertex>(data);
    }
}

}   // namespace

// ---- Textures ------------------------------------------------------------------------

void TextureGraveyard::bury(TextureHandle handle) {
    if (!handle.id) return;
    std::lock_guard lock(m_mutex);
    m_handles.push_back(handle);
}

void TextureGraveyard::flush(::shockwave::Renderer* renderer) {
    std::vector<TextureHandle> handles;
    {
        std::lock_guard lock(m_mutex);
        handles.swap(m_handles);
    }
    if (renderer)
        for (TextureHandle handle : handles) renderer->destroyTexture(handle);
}

BridgeTexture::BridgeTexture(TextureHandle handle, int32_t width, int32_t height, TextureGraveyard* graveyard)
    : m_handle(handle), m_graveyard(graveyard) {
    param_.Size = {width, height, 1};
}

BridgeTexture::~BridgeTexture() {
    if (m_graveyard) m_graveyard->bury(m_handle);
}

Effekseer::Backend::TextureRef BridgeDevice::CreateTexture(
    const Effekseer::Backend::TextureParameter& param, const Effekseer::CustomVector<uint8_t>& initialData) {
    // Only Effekseer's own 1x1 white and flat-normal stand-ins come through here; white
    // is texture 0 in every renderer, so neither needs a texture of its own.
    (void)initialData;
    auto texture = Effekseer::MakeRefPtr<BridgeTexture>(TextureHandle{}, param.Size[0], param.Size[1], nullptr);
    return texture;
}

Effekseer::Backend::TextureRef
BridgeDevice::createTexture(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba, bool srgb) {
    TextureHandle handle;
    if (m_renderer) handle = m_renderer->createTexture(width, height, rgba, srgb);
    return Effekseer::MakeRefPtr<BridgeTexture>(
        handle, static_cast<int32_t>(width), static_cast<int32_t>(height), m_renderer ? m_graveyard : nullptr);
}

Effekseer::TextureRef BridgeTextureLoader::Load(const char16_t* path, Effekseer::TextureType textureType) {
    const fs::path file = toPath(path);
    if (report) report->files.push_back(file);
    Image image;
    std::string error;
    if (!loadImage(file, image, error)) {
        if (report) report->missing.push_back("texture " + file.filename().string() + " (" + error + ")");
        return nullptr;
    }
    // Color textures are authored in sRGB; normal maps and distortion offsets are data.
    auto backend =
        m_device->createTexture(image.width, image.height, image.rgba, textureType == Effekseer::TextureType::Color);
    auto texture = Effekseer::MakeRefPtr<Effekseer::Texture>();
    texture->SetBackend(backend);
    return texture;
}

// ---- Models --------------------------------------------------------------------------

Effekseer::ModelRef BridgeModelLoader::Load(const char16_t* path) {
    const fs::path file = toPath(path);
    if (report) report->files.push_back(file);
    std::vector<uint8_t> bytes;
    if (!readFile(file, bytes) || bytes.empty()) {
        if (report) report->missing.push_back("model " + file.filename().string());
        return nullptr;
    }
    return Load(bytes.data(), static_cast<int32_t>(bytes.size()));
}

Effekseer::ModelRef BridgeModelLoader::Load(const void* data, int32_t size) {
    return Effekseer::MakeRefPtr<Effekseer::Model>(data, size);
}

namespace {

// Mesh particles. Effekseer's ModelRendererBase gathers the instances, billboards and
// sorts them; instead of instanced GPU draws, each instance's triangles are written out.
class CaptureModelRenderer final : public EffekseerRenderer::ModelRendererBase {
public:
    explicit CaptureModelRenderer(CaptureRenderer* renderer) : m_renderer(renderer) {}

    void BeginRendering(const NodeParameter& parameter, int32_t count, void* userData) override {
        BeginRendering_(m_renderer, parameter, count, userData);
    }
    void Rendering(const NodeParameter& parameter, const InstanceParameter& instance, void* userData) override {
        Rendering_<CaptureRenderer>(m_renderer, parameter, instance, userData);
    }
    void EndRendering(const NodeParameter& parameter, void*) override {
        if (m_matrixes.empty() || parameter.ModelIndex < 0) return;
        Effekseer::ModelRef model = parameter.IsProceduralMode
                                        ? parameter.EffectPointer->GetProceduralModel(parameter.ModelIndex)
                                        : parameter.EffectPointer->GetModel(parameter.ModelIndex);
        if (model == nullptr || model->GetFrameCount() <= 0) return;
        SortTemporaryValues(m_renderer, parameter);

        const Effekseer::NodeRendererBasicParameter& basic = *parameter.BasicParameterPtr;
        BatchState state;
        state.shader = collector_.ShaderType;
        state.collector = &collector_;
        state.blend = basic.AlphaBlend;
        state.cull = parameter.Culling;
        state.depthTest = parameter.ZTest;
        state.depthWrite = parameter.ZWrite;
        state.emissive = basic.EmissiveScaling;
        state.edgeThreshold = basic.EdgeThreshold;
        state.edgeScale = basic.EdgeColorScaling;
        std::memcpy(state.edgeColor, basic.EdgeColor, 4);
        state.flipbook = EffekseerRenderer::ToState(basic.Flipbook);
        state.materialFile = basic.MaterialType == Effekseer::RendererMaterialType::File;
        state.softFar = basic.SoftParticleDistanceFar;
        state.softNear = basic.SoftParticleDistanceNear;
        state.softNearOffset = basic.SoftParticleDistanceNearOffset;
        state.distortion = basic.DistortionIntensity;
        for (size_t i = 0; i < m_matrixes.size(); ++i) {
            const int32_t frame = m_times[i] % model->GetFrameCount();
            m_renderer->addModel(
                state, *model.Get(), frame, m_matrixes[i], m_uv[i], m_alphaUV[i], m_flipbookIndexAndNextRate[i],
                m_alphaThreshold[i], m_colors[i]);
        }
    }

private:
    CaptureRenderer* m_renderer;
};

}   // namespace

// ---- The standard renderer stand-in -------------------------------------------------

void CaptureStandardRenderer::BeginRenderingAndRenderingIfRequired(
    const EffekseerRenderer::StandardRendererState& state, int32_t count, int& stride, void*& data) {
    switch (state.Collector.ShaderType) {
    case RendererShaderType::Material:
        stride = static_cast<int>(
            sizeof(EffekseerRenderer::DynamicVertex) +
            (state.CustomData1Count + state.CustomData2Count) * sizeof(float));
        break;
    case RendererShaderType::Lit:
    case RendererShaderType::BackDistortion: stride = sizeof(EffekseerRenderer::LightingVertex); break;
    case RendererShaderType::AdvancedLit:
    case RendererShaderType::AdvancedBackDistortion: stride = sizeof(EffekseerRenderer::AdvancedLightingVertex); break;
    case RendererShaderType::AdvancedUnlit: stride = sizeof(EffekseerRenderer::AdvancedSimpleVertex); break;
    default: stride = sizeof(EffekseerRenderer::SimpleVertex); break;
    }
    const size_t bytes = static_cast<size_t>(std::max(count, 0)) * static_cast<size_t>(stride);
    if (bytes == 0) {
        data = nullptr;
        return;
    }
    // Zeroed, so quads a node reserved but didn't fill come out as nothing.
    if (m_scratch.size() < m_used + bytes) m_scratch.resize(std::max(m_used + bytes, m_scratch.size() * 2));
    std::memset(m_scratch.data() + m_used, 0, bytes);
    m_chunks.push_back({state, m_used, count, stride});
    data = m_scratch.data() + m_used;
    m_used += bytes;
}

void CaptureStandardRenderer::flush() {
    for (const Chunk& chunk : m_chunks) {
        const EffekseerRenderer::StandardRendererState& s = chunk.state;
        BatchState state;
        state.shader = s.Collector.ShaderType;
        state.collector = &s.Collector;
        state.blend = s.AlphaBlend;
        state.cull = s.CullingType;
        state.depthTest = s.DepthTest;
        state.depthWrite = s.DepthWrite;
        state.emissive = s.EmissiveScaling;
        state.edgeThreshold = s.EdgeThreshold;
        state.edgeScale = s.EdgeColorScaling;
        std::memcpy(state.edgeColor, s.EdgeColor, 4);
        state.flipbook = s.Flipbook;
        state.materialFile = s.MaterialType == Effekseer::RendererMaterialType::File;
        state.softFar = s.SoftParticleDistanceFar;
        state.softNear = s.SoftParticleDistanceNear;
        state.softNearOffset = s.SoftParticleDistanceNearOffset;
        state.distortion = s.DistortionIntensity;
        m_renderer->addQuads(state, m_scratch.data() + chunk.offset, chunk.stride, chunk.count);
    }
    m_chunks.clear();
    m_used = 0;
}

// ---- Capture renderer ----------------------------------------------------------------

CaptureRenderer::CaptureRenderer(::shockwave::Renderer* renderer, TextureGraveyard* graveyard)
    : m_renderer(renderer), m_device(Effekseer::MakeRefPtr<BridgeDevice>(renderer, graveyard)) {
    GetImpl()->CreateProxyTextures(this);
    // Effekseer's colors are authored for gamma-space blending; Shockwave blends in linear
    // HDR, so textures are decoded as sRGB and vertex colors converted in the shader.
    GetImpl()->MaintainGammaColorInLinearColorSpace = false;
}

CaptureRenderer::~CaptureRenderer() { GetImpl()->DeleteProxyTextures(this); }

Effekseer::Backend::GraphicsDeviceRef CaptureRenderer::GetGraphicsDevice() const { return m_device; }

::Effekseer::SpriteRendererRef CaptureRenderer::CreateSpriteRenderer() {
    return ::Effekseer::SpriteRendererRef(new EffekseerRenderer::SpriteRendererBase<CaptureRenderer, false>(this));
}
::Effekseer::RibbonRendererRef CaptureRenderer::CreateRibbonRenderer() {
    return ::Effekseer::RibbonRendererRef(new EffekseerRenderer::RibbonRendererBase<CaptureRenderer, false>(this));
}
::Effekseer::RingRendererRef CaptureRenderer::CreateRingRenderer() {
    return ::Effekseer::RingRendererRef(new EffekseerRenderer::RingRendererBase<CaptureRenderer, false>(this));
}
::Effekseer::TrackRendererRef CaptureRenderer::CreateTrackRenderer() {
    return ::Effekseer::TrackRendererRef(new EffekseerRenderer::TrackRendererBase<CaptureRenderer, false>(this));
}
::Effekseer::ModelRendererRef CaptureRenderer::CreateModelRenderer() {
    return ::Effekseer::ModelRendererRef(new CaptureModelRenderer(this));
}
::Effekseer::TextureLoaderRef CaptureRenderer::CreateTextureLoader(::Effekseer::FileInterfaceRef) {
    return Effekseer::MakeRefPtr<BridgeTextureLoader>(m_device.Get());
}
::Effekseer::ModelLoaderRef CaptureRenderer::CreateModelLoader(::Effekseer::FileInterfaceRef) {
    return Effekseer::MakeRefPtr<BridgeModelLoader>();
}

bool CaptureRenderer::EndRendering() {
    m_standard.flush();
    return true;
}

void CaptureRenderer::begin(const glm::mat4& view, const glm::mat4& projection) {
    m_vertices.clear();
    m_indices.clear();
    m_batches.clear();
    m_groups.clear();
    m_group = 0;
    m_softOverride = -1.0f;
    m_newGroup = false;
    m_skipped = 0;
    m_view = view;
    // glm's column-major, column-vector matrices have the same memory layout as
    // Effekseer's row-major, row-vector ones.
    Effekseer::Matrix44 camera, proj;
    std::memcpy(camera.Values, &view[0][0], sizeof(camera.Values));
    std::memcpy(proj.Values, &projection[0][0], sizeof(proj.Values));
    SetCameraMatrix(camera);
    SetProjectionMatrix(proj);
}

void CaptureRenderer::end() { m_standard.flush(); }

void CaptureRenderer::beginGroup(uint32_t group, float softDistance) {
    m_standard.flush();
    m_group = group;
    m_softOverride = softDistance;
    m_newGroup = true;
}

void CaptureRenderer::reorderBatches(const std::vector<uint32_t>& order) {
    std::vector<ParticleBatch> batches;
    std::vector<uint32_t> groups;
    batches.reserve(order.size());
    groups.reserve(order.size());
    for (uint32_t i : order) {
        batches.push_back(m_batches[i]);
        groups.push_back(m_groups[i]);
    }
    m_batches.swap(batches);
    m_groups.swap(groups);
}

ParticleDrawList CaptureRenderer::drawList() const {
    ParticleDrawList list;
    list.vertices = m_vertices;
    list.indices = m_indices;
    list.batches = m_batches;
    const Effekseer::Vector3D direction = GetLightDirection();
    const Effekseer::Color light = GetLightColor(), ambient = GetLightAmbientColor();
    // Effekseer's light direction points toward the light; ParticleDrawList's is where it shines.
    list.lightDirection = -glm::vec3(direction.X, direction.Y, direction.Z);
    list.lightColor = glm::vec3(light.R, light.G, light.B) / 255.0f;
    list.ambient = glm::vec3(ambient.R, ambient.G, ambient.B) / 255.0f;
    return list;
}

TextureHandle CaptureRenderer::textureOf(const Effekseer::Backend::TextureRef& texture) const {
    if (const auto* bridge = dynamic_cast<const BridgeTexture*>(texture.Get())) return bridge->handle();
    return {};
}

bool CaptureRenderer::beginBatch(const BatchState& state, uint32_t triangles) {
    if (!drawable(state)) {
        ++m_skipped;
        return false;
    }
    const EffekseerRenderer::ShaderParameterCollector& c = *state.collector;
    ParticleBatch batch;
    batch.firstIndex = static_cast<uint32_t>(m_indices.size());
    batch.texture = textureOf(c.Textures[0]);
    const int alphaSlot = alphaTextureSlot(state.shader);
    if (alphaSlot >= 0 && alphaSlot < c.TextureCount) batch.alphaTexture = textureOf(c.Textures[alphaSlot]);
    batch.blend = static_cast<ParticleBlend>(std::clamp(static_cast<int>(state.blend), 0, 4));
    batch.cull = state.cull == Effekseer::CullingType::Front  ? ParticleCull::Front
                 : state.cull == Effekseer::CullingType::Back ? ParticleCull::Back
                                                              : ParticleCull::None;
    batch.depthTest = state.depthTest;
    batch.depthWrite = state.depthWrite;
    if (state.shader == RendererShaderType::Lit || state.shader == RendererShaderType::AdvancedLit)
        batch.flags |= kParticleLit;
    if (distortionShader(state.shader)) {
        batch.flags |= kParticleDistortion;
        batch.distortion = state.distortion;
    }
    batch.softDistance = m_softOverride >= 0.0f ? m_softOverride : std::max(state.softFar, 0.0f);
    batch.softNear = std::max(state.softNear, 0.0f);
    batch.softNearOffset = state.softNearOffset;
    if (c.TextureWrapTypes[0] == Effekseer::TextureWrapType::Clamp) batch.flags |= kParticleClampUV;
    if (advanced(state.shader)) {
        batch.flags |= kParticleAlphaCutoff;
        if (batch.alphaTexture.id) batch.flags |= kParticleAlphaTexture;
        if (state.flipbook.EnableInterpolation) batch.flags |= kParticleFlipbookBlend;
        const float scale = state.edgeScale;
        batch.edgeColor = glm::vec4(
            srgbToLinear(state.edgeColor[0] / 255.0f) * scale, srgbToLinear(state.edgeColor[1] / 255.0f) * scale,
            srgbToLinear(state.edgeColor[2] / 255.0f) * scale, state.edgeThreshold);
    }
    batch.emissive = state.emissive;
    batch.depth = m_depth;
    batch.indexCount = triangles * 3;

    // Back to back with the same look, in the same effect: one draw.
    if (!m_batches.empty() && !m_newGroup) {
        ParticleBatch& last = m_batches.back();
        if (last.firstIndex + last.indexCount == batch.firstIndex && last.texture.id == batch.texture.id &&
            last.alphaTexture.id == batch.alphaTexture.id && last.blend == batch.blend && last.cull == batch.cull &&
            last.depthTest == batch.depthTest && last.depthWrite == batch.depthWrite && last.flags == batch.flags &&
            last.emissive == batch.emissive && last.edgeColor == batch.edgeColor &&
            last.softDistance == batch.softDistance && last.softNear == batch.softNear &&
            last.softNearOffset == batch.softNearOffset && last.distortion == batch.distortion) {
            last.indexCount += batch.indexCount;
            return true;
        }
    }
    m_batches.push_back(batch);
    m_groups.push_back(m_group);
    m_newGroup = false;
    return true;
}

void CaptureRenderer::addQuads(const BatchState& state, const uint8_t* vertices, int32_t stride, int32_t vertexCount) {
    const int32_t quads = vertexCount / 4;
    if (quads <= 0 || !beginBatch(state, static_cast<uint32_t>(quads) * 2)) return;
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const bool advancedShader = advanced(state.shader);
    m_vertices.reserve(m_vertices.size() + static_cast<size_t>(quads) * 4);
    for (int32_t i = 0; i < quads * 4; ++i) {
        const Decoded d = decode(state.shader, vertices + static_cast<size_t>(i) * static_cast<size_t>(stride));
        ParticleVertex v{};
        v.position = d.position;
        v.color = packColor(d.color);
        v.uv = d.uv;
        v.alphaUV = d.alphaUv;
        float rate = 0.0f;
        flipbook(state.flipbook, d.flipbookIndex, d.uv, v.nextUV, rate);
        v.normal = d.normal ? d.normal : packNormal({0, 1, 0});
        v.tangent = d.tangent ? d.tangent : packNormal({1, 0, 0});
        v.params = glm::packHalf2x16({rate, advancedShader ? d.alphaThreshold : 0.0f});
        m_vertices.push_back(v);
    }
    m_indices.reserve(m_indices.size() + static_cast<size_t>(quads) * 6);
    for (int32_t q = 0; q < quads; ++q) {
        const uint32_t b = base + static_cast<uint32_t>(q) * 4;
        // Effekseer's quad order (EffekseerRendererGL's index buffer).
        for (uint32_t index : {b + 3, b + 1, b + 0, b + 3, b + 0, b + 2}) m_indices.push_back(index);
    }
}

void CaptureRenderer::addModel(
    const BatchState& state, const Effekseer::Model& model, int32_t frame, const Effekseer::Matrix44& world,
    const Effekseer::RectF& uv, const Effekseer::RectF& alphaUv, float flipbookIndex, float alphaThreshold,
    const Effekseer::Color& color) {
    const int32_t faceCount = model.GetFaceCount(frame);
    const int32_t vertexCount = model.GetVertexCount(frame);
    if (faceCount <= 0 || vertexCount <= 0 || !beginBatch(state, static_cast<uint32_t>(faceCount))) return;
    const Effekseer::Model::Vertex* source = model.GetVertexes(frame);
    const Effekseer::Model::Face* faces = model.GetFaces(frame);
    const auto& m = world.Values;   // row vectors: p' = p * M
    auto transform = [&](const Effekseer::Vector3D& p, float w) {
        return glm::vec3(
            p.X * m[0][0] + p.Y * m[1][0] + p.Z * m[2][0] + w * m[3][0],
            p.X * m[0][1] + p.Y * m[1][1] + p.Z * m[2][1] + w * m[3][1],
            p.X * m[0][2] + p.Y * m[1][2] + p.Z * m[2][2] + w * m[3][2]);
    };
    const uint32_t base = static_cast<uint32_t>(m_vertices.size());
    const float threshold = advanced(state.shader) ? alphaThreshold : 0.0f;
    for (int32_t i = 0; i < vertexCount; ++i) {
        const Effekseer::Model::Vertex& s = source[i];
        ParticleVertex v{};
        v.position = transform(s.Position, 1.0f);
        v.color = packColor(s.VColor, color);
        v.uv = {s.UV.X * uv.Width + uv.X, s.UV.Y * uv.Height + uv.Y};
        v.alphaUV = {s.UV.X * alphaUv.Width + alphaUv.X, s.UV.Y * alphaUv.Height + alphaUv.Y};
        float rate = 0.0f;
        flipbook(state.flipbook, flipbookIndex, v.uv, v.nextUV, rate);
        v.normal = packNormal(transform(s.Normal, 0.0f));
        v.tangent = packNormal(transform(s.Tangent, 0.0f));
        v.params = glm::packHalf2x16({rate, threshold});
        m_vertices.push_back(v);
    }
    for (int32_t f = 0; f < faceCount; ++f)
        for (int32_t index : faces[f].Indexes) m_indices.push_back(base + static_cast<uint32_t>(index));
}

}   // namespace shockwave::effects
