#include <Assets/ReflectionData.h>

#include "LightingJson.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <sstream>
#include <thread>

namespace shockwave {

namespace fs = std::filesystem;
using namespace lighting_json;

bool validReflectionResolution(uint32_t size) {
    return std::find(std::begin(kReflectionResolutions), std::end(kReflectionResolutions), size) !=
           std::end(kReflectionResolutions);
}

uint64_t reflectionCubeBytes(uint32_t size) {
    uint64_t total = 0;
    for (uint32_t l = 0; l < kReflectionLevels; ++l) {
        const uint64_t s = std::max(1u, size >> l);
        total += 6 * s * s * 8;
    }
    return total;
}

// ---- The cube ----

bool ReflectionCube::valid() const {
    if (!validReflectionResolution(size) || levels.size() != kReflectionLevels) return false;
    for (uint32_t l = 0; l < kReflectionLevels; ++l) {
        const size_t s = levelSize(l);
        if (levels[l].size() != 6 * s * s * 4) return false;
        for (float v : levels[l])
            if (!std::isfinite(v)) return false;
    }
    return true;
}

TextureData reflectionTextureData(const ReflectionCube& cube) {
    TextureData t;
    t.format = TextureFormat::RGBA16F;
    t.shape = TextureShape::Cube;
    t.srgb = false;
    t.width = t.height = cube.size;
    for (uint32_t l = 0; l < cube.levels.size(); ++l) {
        const std::vector<uint8_t> halves = lightmapHalfFloats(cube.levels[l]);
        t.levels.push_back({cube.levelSize(l), cube.levelSize(l), t.bytes.size(), halves.size()});
        t.bytes.insert(t.bytes.end(), halves.begin(), halves.end());
    }
    return t;
}

namespace {
// Bilinear within one face of one level, clamped at its edges.
glm::vec3 sampleFace(const std::vector<float>& level, uint32_t size, uint32_t face, const glm::vec2& st) {
    const float x = (st.x * 0.5f + 0.5f) * float(size) - 0.5f, y = (st.y * 0.5f + 0.5f) * float(size) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    const float* base = level.data() + size_t(face) * size * size * 4;
    auto at = [&](int px, int py) {
        px = std::clamp(px, 0, int(size) - 1);
        py = std::clamp(py, 0, int(size) - 1);
        const float* t = base + (size_t(py) * size + size_t(px)) * 4;
        return glm::vec3(t[0], t[1], t[2]);
    };
    const int ix = int(fx), iy = int(fy);
    return glm::mix(glm::mix(at(ix, iy), at(ix + 1, iy), tx), glm::mix(at(ix, iy + 1), at(ix + 1, iy + 1), tx), ty);
}
// A level chain with any number of levels (the prefilter's source has all of them).
glm::vec3 sampleChain(const std::vector<std::vector<float>>& chain, uint32_t size, const glm::vec3& d, float lod) {
    glm::vec2 st;
    const uint32_t face = cubeFaceOf(d, st);
    lod = std::clamp(lod, 0.0f, float(chain.size() - 1));
    const uint32_t lo = uint32_t(lod), hi = std::min<uint32_t>(lo + 1, uint32_t(chain.size() - 1));
    const glm::vec3 a = sampleFace(chain[lo], std::max(1u, size >> lo), face, st);
    if (hi == lo || lod == float(lo)) return a;
    return glm::mix(a, sampleFace(chain[hi], std::max(1u, size >> hi), face, st), lod - float(lo));
}
}   // namespace

glm::vec3 sampleReflectionCube(const ReflectionCube& cube, const glm::vec3& direction, float lod) {
    if (cube.levels.empty() || cube.size == 0) return glm::vec3(0.0f);
    return sampleChain(cube.levels, cube.size, direction, lod);
}

std::array<glm::vec3, 6> reflectionFaceMeans(const ReflectionCube& cube, uint32_t level) {
    std::array<glm::vec3, 6> means{};
    if (level >= cube.levels.size()) return means;
    const size_t texels = size_t(cube.levelSize(level)) * cube.levelSize(level);
    for (uint32_t f = 0; f < 6; ++f) {
        glm::dvec3 sum(0.0);
        const float* p = cube.levels[level].data() + f * texels * 4;
        for (size_t i = 0; i < texels; ++i) sum += glm::dvec3(p[4 * i], p[4 * i + 1], p[4 * i + 2]);
        means[f] = glm::vec3(sum / double(std::max<size_t>(texels, 1)));
    }
    return means;
}

// ---- .ktx2 ----

namespace {
constexpr uint8_t kKtx2Identifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
constexpr uint32_t kVkFormatRgba16f = 97;   // VK_FORMAT_R16G16B16A16_SFLOAT
constexpr uint32_t kDfdBlockSize = 24 + 16 * 4;
constexpr uint32_t kDfdTotal = 4 + kDfdBlockSize;
constexpr uint32_t kLevelIndexAt = 80;

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
uint32_t getU32(std::span<const uint8_t> b, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(b[at + i]) << (8 * i);
    return v;
}
uint64_t getU64(std::span<const uint8_t> b, size_t at) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(b[at + i]) << (8 * i);
    return v;
}
}   // namespace

bool encodeReflectionKtx2(const ReflectionCube& cube, std::vector<uint8_t>& ktx2, std::string& error) {
    if (!cube.valid()) {
        error = "a reflection cube needs a size of 64, 128, 256 or 512, " + std::to_string(kReflectionLevels) +
                " whole levels and finite values";
        return false;
    }
    const uint32_t levels = kReflectionLevels;
    const uint32_t dfdAt = kLevelIndexAt + 24 * levels;
    const uint32_t dataAt = (dfdAt + kDfdTotal + 7) / 8 * 8;
    std::vector<std::vector<uint8_t>> halves(levels);
    for (uint32_t l = 0; l < levels; ++l) halves[l] = lightmapHalfFloats(cube.levels[l]);
    // Smallest level first in the file; each level's offset is a multiple of 8 (its sizes are).
    std::vector<uint64_t> offsets(levels);
    uint64_t at = dataAt;
    for (uint32_t l = levels; l-- > 0;) {
        offsets[l] = at;
        at += halves[l].size();
    }
    std::vector<uint8_t> out;
    out.insert(out.end(), std::begin(kKtx2Identifier), std::end(kKtx2Identifier));
    putU32(out, kVkFormatRgba16f);
    putU32(out, 2);   // typeSize
    putU32(out, cube.size);
    putU32(out, cube.size);
    putU32(out, 0);   // depth
    putU32(out, 0);   // layers
    putU32(out, 6);   // faces
    putU32(out, levels);
    putU32(out, 0);   // no supercompression
    putU32(out, dfdAt);
    putU32(out, kDfdTotal);
    putU32(out, 0);   // no key/value data
    putU32(out, 0);
    putU64(out, 0);   // no supercompression global data
    putU64(out, 0);
    for (uint32_t l = 0; l < levels; ++l) {
        putU64(out, offsets[l]);
        putU64(out, halves[l].size());
        putU64(out, halves[l].size());
    }
    // Basic data format descriptor: RGBSDA, BT.709, linear, 8 bytes a texel, four float16
    // samples (R, G, B, A = channel 15), as the lightmaps'.
    putU32(out, kDfdTotal);
    putU32(out, 0);
    putU32(out, (kDfdBlockSize << 16) | 2);
    putU32(out, 1 | (1 << 8) | (1 << 16));
    putU32(out, 0);
    putU32(out, 8);
    putU32(out, 0);
    const uint32_t channels[4] = {0, 1, 2, 15};
    for (uint32_t c = 0; c < 4; ++c) {
        putU32(out, (c * 16) | (15u << 16) | ((0xC0u | channels[c]) << 24));
        putU32(out, 0);
        putU32(out, 0xBF800000u);
        putU32(out, 0x3F800000u);
    }
    out.resize(dataAt, 0);
    for (uint32_t l = levels; l-- > 0;) out.insert(out.end(), halves[l].begin(), halves[l].end());
    ktx2 = std::move(out);
    return true;
}

bool isReflectionKtx2(std::span<const uint8_t> ktx2) {
    return ktx2.size() >= 40 && std::memcmp(ktx2.data(), kKtx2Identifier, 12) == 0 && getU32(ktx2, 36) == 6;
}

bool decodeReflectionKtx2(std::span<const uint8_t> ktx2, ReflectionCube& cube, std::string& error) {
    if (ktx2.size() < kLevelIndexAt || std::memcmp(ktx2.data(), kKtx2Identifier, 12) != 0) {
        error = ktx2.size() < 12 || std::memcmp(ktx2.data(), kKtx2Identifier, 12) != 0 ? "not a KTX2 file"
                                                                                       : "KTX2 header is cut short";
        return false;
    }
    const uint32_t format = getU32(ktx2, 12), typeSize = getU32(ktx2, 16), w = getU32(ktx2, 20), h = getU32(ktx2, 24),
                   depth = getU32(ktx2, 28), layers = getU32(ktx2, 32), faces = getU32(ktx2, 36),
                   levels = getU32(ktx2, 40), super = getU32(ktx2, 44);
    if (faces != 6) {
        error = "a reflection cube has 6 faces; this file has " + std::to_string(faces);
        return false;
    }
    if (format != kVkFormatRgba16f || typeSize != 2 || depth != 0 || layers != 0 || super != 0) {
        error = "reflection cubes must be uncompressed RGBA16F (VK_FORMAT_R16G16B16A16_SFLOAT) cube maps";
        return false;
    }
    if (w != h || !validReflectionResolution(w)) {
        error = "a reflection cube's faces must be square, 64, 128, 256 or 512 texels";
        return false;
    }
    if (levels != kReflectionLevels) {
        error = "a reflection cube has " + std::to_string(kReflectionLevels) + " levels; this file has " +
                std::to_string(levels);
        return false;
    }
    if (ktx2.size() < kLevelIndexAt + 24 * size_t(levels)) {
        error = "KTX2 level index is cut short";
        return false;
    }
    ReflectionCube out;
    out.size = w;
    out.levels.resize(levels);
    for (uint32_t l = 0; l < levels; ++l) {
        const uint64_t offset = getU64(ktx2, kLevelIndexAt + 24 * l), length = getU64(ktx2, kLevelIndexAt + 24 * l + 8);
        const uint64_t s = out.levelSize(l);
        if (length != 6 * s * s * 8 || offset > ktx2.size() || length > ktx2.size() - offset) {
            error = "reflection cube level " + std::to_string(l) + " doesn't fit the file";
            return false;
        }
        std::vector<float>& values = out.levels[l];
        values.resize(size_t(length / 2));
        for (size_t i = 0; i < values.size(); ++i) {
            const uint16_t bits = uint16_t(ktx2[offset + 2 * i] | (ktx2[offset + 2 * i + 1] << 8));
            values[i] = glm::unpackHalf1x16(bits);
            if (!std::isfinite(values[i])) {
                error = "reflection cube has a value that isn't a finite number";
                return false;
            }
        }
    }
    cube = std::move(out);
    return true;
}

// ---- Capture ----

void reflectionFaceCamera(
    uint32_t face, const glm::vec3& position, float nearPlane, float farPlane, glm::mat4& view, glm::mat4& projection) {
    // Looking along the face's axis, with up opposite to where its t (rows) grows, so the
    // captured image is the face as cubeFaceDirection lays it out (mirrored on some faces;
    // resampleReflectionFace goes by directions, so that doesn't matter).
    face = std::min(face, 5u);
    const glm::vec3 forward = cubeFaceDirection(face, glm::vec2(0.0f));
    const glm::vec3 up = cubeFaceDirection(face, glm::vec2(0.0f, -1.0f)) - forward;
    view = glm::lookAt(position, position + forward, up);
    // A finite reverse-Z projection: depth 1 at near, 0 at far.
    const float n = std::max(nearPlane, 1e-4f);
    const float f = std::max(farPlane, n * 1.001f);
    projection = glm::mat4(0.0f);
    projection[0][0] = 1.0f;   // 90 degrees: tan 45 = 1
    projection[1][1] = 1.0f;
    projection[2][2] = n / (f - n);
    projection[2][3] = -1.0f;
    projection[3][2] = f * n / (f - n);
}

bool resampleReflectionFace(
    const SceneCapture& capture, uint32_t face, const glm::mat4& viewProjection, uint32_t size, std::vector<float>& out,
    std::string& error) {
    if (capture.width == 0 || capture.height == 0 ||
        capture.rgba.size() != size_t(capture.width) * capture.height * 4) {
        error = capture.error.empty() ? "the capture is empty" : capture.error;
        return false;
    }
    const uint32_t W = capture.width, H = capture.height;
    auto pixel = [&](float px, float py) {
        // Bilinear in the captured image, clamped at its edges.
        px = std::clamp(px, 0.0f, float(W - 1));
        py = std::clamp(py, 0.0f, float(H - 1));
        const uint32_t x0 = uint32_t(px), y0 = uint32_t(py);
        const uint32_t x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
        const float tx = px - float(x0), ty = py - float(y0);
        auto at = [&](uint32_t x, uint32_t y) {
            const float* p = &capture.rgba[(size_t(y) * W + x) * 4];
            return glm::vec3(p[0], p[1], p[2]);
        };
        return glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx), glm::mix(at(x0, y1), at(x1, y1), tx), ty);
    };
    // A few samples per texel when the capture is finer than the face, so it's box-filtered.
    const uint32_t k = std::clamp((std::max(W, H) + size - 1) / size, 1u, 4u);
    const size_t first = out.size();
    out.resize(first + size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            glm::vec3 sum(0.0f);
            for (uint32_t j = 0; j < k; ++j)
                for (uint32_t i = 0; i < k; ++i) {
                    const glm::vec2 st =
                        (glm::vec2(x, y) + (glm::vec2(i, j) + 0.5f) / float(k)) / float(size) * 2.0f - 1.0f;
                    const glm::vec4 clip = viewProjection * glm::vec4(cubeFaceDirection(face, st), 0.0f);
                    if (!(clip.w > 1e-6f)) {
                        error = "the capture camera doesn't look at face " + std::to_string(face);
                        out.resize(first);
                        return false;
                    }
                    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                    if (std::abs(ndc.x) > 1.01f || std::abs(ndc.y) > 1.01f) {
                        error = "the capture camera doesn't cover face " + std::to_string(face);
                        out.resize(first);
                        return false;
                    }
                    sum += pixel((ndc.x * 0.5f + 0.5f) * float(W) - 0.5f, (0.5f - ndc.y * 0.5f) * float(H) - 0.5f);
                }
            float* t = &out[first + (size_t(y) * size + x) * 4];
            const glm::vec3 c = sum / float(k * k);
            t[0] = c.r;
            t[1] = c.g;
            t[2] = c.b;
            t[3] = 1.0f;
        }
    return true;
}

// ---- Prefilter: Shaders/environment_filter.comp on the CPU ----

namespace {
constexpr float kPi = 3.14159265f;

uint32_t reverseBits(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}
glm::vec2 hammersley(uint32_t i, uint32_t count) {
    return {(float(i) + 0.5f) / float(count), float(reverseBits(i)) * 2.3283064365386963e-10f};
}
glm::vec3 sampleGGX(const glm::vec2& xi, float a) {
    const float phi = 2.0f * kPi * xi.x;
    const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}
float distributionGGX(float nDotH, float a) {
    const float a2 = a * a;
    const float d = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}
glm::vec3 toWorld(const glm::vec3& v, const glm::vec3& n) {
    const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    const glm::vec3 t = glm::normalize(glm::cross(up, n));
    const glm::vec3 b = glm::cross(n, t);
    return t * v.x + b * v.y + n * v.z;
}
// Level 0's full chain down to 1 texel, each level the 2 x 2 average of the one above.
std::vector<std::vector<float>> boxChain(const std::vector<float>& level0, uint32_t size) {
    std::vector<std::vector<float>> chain{level0};
    for (uint32_t s = size; s > 1; s >>= 1) {
        const uint32_t n = s / 2;
        const std::vector<float>& above = chain.back();
        std::vector<float> next(6 * size_t(n) * n * 4);
        for (uint32_t f = 0; f < 6; ++f)
            for (uint32_t y = 0; y < n; ++y)
                for (uint32_t x = 0; x < n; ++x)
                    for (uint32_t c = 0; c < 4; ++c) {
                        auto at = [&](uint32_t px, uint32_t py) {
                            return above[((size_t(f) * s + py) * s + px) * 4 + c];
                        };
                        next[((size_t(f) * n + y) * n + x) * 4 + c] =
                            0.25f *
                            (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                    }
        chain.push_back(std::move(next));
    }
    return chain;
}
}   // namespace

void prefilterReflectionCube(ReflectionCube& cube) {
    if (cube.size == 0 || cube.levels.empty()) return;
    const uint32_t size = cube.size;
    const std::vector<std::vector<float>> source = boxChain(cube.levels[0], size);
    const float sourceSize = float(size), sourceLevels = float(source.size());
    const uint32_t samples = kReflectionFilterSamples;
    cube.levels.resize(kReflectionLevels);
    for (uint32_t l = 1; l < kReflectionLevels; ++l) {
        const uint32_t s = cube.levelSize(l);
        const float roughness = float(l) / float(kReflectionLevels - 1);
        const float a = roughness * roughness;
        std::vector<float>& out = cube.levels[l];
        out.assign(6 * size_t(s) * s * 4, 0.0f);
        // One thread per face; each texel's sum is computed alone, so the result is the same
        // however the work is split.
        std::vector<std::thread> threads;
        for (uint32_t f = 0; f < 6; ++f)
            threads.emplace_back([&, f] {
                for (uint32_t y = 0; y < s; ++y)
                    for (uint32_t x = 0; x < s; ++x) {
                        const glm::vec2 st = (glm::vec2(x, y) + 0.5f) / float(s) * 2.0f - 1.0f;
                        const glm::vec3 n = glm::normalize(cubeFaceDirection(f, st));
                        glm::vec3 total(0.0f);
                        float weight = 0.0f;
                        for (uint32_t i = 0; i < samples; ++i) {
                            const glm::vec3 h = toWorld(sampleGGX(hammersley(i, samples), a), n);
                            const float nDotH = glm::dot(n, h);
                            const glm::vec3 dir = 2.0f * nDotH * h - n;
                            const float nDotL = glm::dot(n, dir);
                            if (nDotL <= 0.0f) continue;
                            const float pdf = distributionGGX(std::max(nDotH, 0.0f), a) * 0.25f;
                            const float texel = 4.0f * kPi / (6.0f * sourceSize * sourceSize);
                            const float sampleArea = 1.0f / (float(samples) * pdf + 1e-6f);
                            const float level =
                                std::clamp(0.5f * std::log2(sampleArea / texel) + 1.0f, 0.0f, sourceLevels - 1.0f);
                            total += sampleChain(source, size, dir, level) * nDotL;
                            weight += nDotL;
                        }
                        const glm::vec3 c = total / std::max(weight, 1e-4f);
                        float* t = &out[((size_t(f) * s + y) * s + x) * 4];
                        t[0] = c.r;
                        t[1] = c.g;
                        t[2] = c.b;
                        t[3] = 1.0f;
                    }
            });
        for (std::thread& t : threads) t.join();
    }
}

// ---- Manifest ----

const ReflectionProbeRecord* ReflectionManifest::probe(const BindingId& binding) const {
    for (const ReflectionProbeRecord& p : probes)
        if (p.binding == binding) return &p;
    return nullptr;
}

bool parseReflectionManifest(
    std::string_view text, const std::string& file, ReflectionManifest& manifest, std::string& error) {
    ReflectionManifest m;
    std::set<std::string> seen;
    std::set<BindingId> bindings;
    std::vector<std::string> w;
    size_t start = 0;
    uint32_t lineNumber = 0;
    bool header = false;
    auto fail = [&](const std::string& message) {
        error = file + ":" + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++lineNumber;
        std::string tokenError;
        if (!tokenize(line, w, tokenError)) return fail(tokenError);
        if (w.empty()) {
            if (end >= text.size()) break;
            continue;
        }
        if (!header) {
            if (w.size() != 2 || w[0] != "swreflection") return fail("the first line must be \"swreflection 1\"");
            if (w[1] != "1") return fail("version " + w[1] + " isn't supported; this build reads swreflection 1");
            header = true;
            continue;
        }
        const std::string& key = w[0];
        if (key != "probe" && !seen.insert(key).second) return fail("\"" + key + "\" is given twice");
        auto single = [&]() -> const std::string* {
            if (w.size() != 2) {
                fail(key + " takes one value");
                return nullptr;
            }
            return &w[1];
        };
        if (key == "scene" || key == "tool") {
            const std::string* v = single();
            if (!v) return false;
            (key == "scene" ? m.scene : m.tool) = *v;
        } else if (key == "color") {
            const std::string* v = single();
            if (!v) return false;
            if (*v != "linear_rec709_radiance")
                return fail("color must be linear_rec709_radiance (linear, before exposure)");
            m.colorConvention = *v;
        } else if (key == "algorithm") {
            const std::string* v = single();
            if (!v) return false;
            if (!u32Value(*v, m.algorithm) || m.algorithm < 1) return fail("algorithm must be a whole number from 1");
        } else if (key == "scene_hash") {
            const std::string* v = single();
            if (!v) return false;
            if (!parseLightingHash(*v, m.sceneHash)) return fail("scene_hash must be 16 lowercase hex digits");
        } else if (key == "probe") {
            if (w.size() < 2) return fail("probe needs a binding id");
            ReflectionProbeRecord p;
            if (!parseBindingId(w[1], p.binding) || !p.binding.valid())
                return fail("probe binding must be 32 lowercase hex digits, not all zero");
            if ((w.size() - 2) % 2 != 0) return fail("probe: every field needs a value");
            std::set<std::string> fields;
            for (size_t i = 2; i < w.size(); i += 2) {
                const std::string& field = w[i];
                const std::string& value = w[i + 1];
                if (!fields.insert(field).second) return fail("probe: \"" + field + "\" is given twice");
                if (field == "path") {
                    p.path = value;
                    if (p.path.empty() || p.path.find("..") != std::string::npos || p.path[0] == '/' ||
                        p.path.find('\\') != std::string::npos || p.path.find(':') != std::string::npos)
                        return fail("probe path must be inside Content, with forward slashes");
                } else if (field == "hash") {
                    if (!parseLightingHash(value, p.hash)) return fail("probe hash must be 16 lowercase hex digits");
                } else if (field == "probe_hash") {
                    if (!parseLightingHash(value, p.probeHash))
                        return fail("probe probe_hash must be 16 lowercase hex digits");
                } else if (field == "bytes") {
                    const auto [q, ec] = std::from_chars(value.data(), value.data() + value.size(), p.bytes);
                    if (ec != std::errc{} || q != value.data() + value.size())
                        return fail("probe bytes must be a whole number");
                } else if (field == "size") {
                    if (!u32Value(value, p.size) || !validReflectionResolution(p.size))
                        return fail("probe size must be 64, 128, 256 or 512");
                } else {
                    return fail("probe: unknown field \"" + field + "\"");
                }
            }
            for (const char* required : {"path", "hash", "bytes", "size", "probe_hash"})
                if (!fields.count(required)) return fail("probe needs " + std::string(required));
            if (!bindings.insert(p.binding).second) return fail("probe " + w[1] + " is listed twice");
            m.probes.push_back(std::move(p));
        } else {
            return fail("unknown key \"" + key + "\"");
        }
        if (end >= text.size()) break;
    }
    if (!header) {
        lineNumber = 1;
        return fail("the first line must be \"swreflection 1\"");
    }
    for (const char* required : {"scene", "algorithm", "scene_hash"})
        if (!seen.count(required)) {
            error = file + ": \"" + required + "\" is missing";
            return false;
        }
    manifest = std::move(m);
    return true;
}

std::string reflectionManifestText(const ReflectionManifest& m) {
    std::ostringstream out;
    out << "swreflection 1\n";
    out << "# Generated by reflection capture (docs/Lighting.md). Capture again rather than edit.\n";
    out << "scene " << quotedText(m.scene) << "\n";
    if (!m.tool.empty()) out << "tool " << quotedText(m.tool) << "\n";
    out << "color " << m.colorConvention << "\n";
    out << "algorithm " << m.algorithm << "\n";
    out << "scene_hash " << formatLightingHash(m.sceneHash) << "\n";
    for (const ReflectionProbeRecord& p : m.probes)
        out << "probe " << formatBindingId(p.binding) << " path " << quotedText(p.path) << " hash "
            << formatLightingHash(p.hash) << " bytes " << p.bytes << " size " << p.size << " probe_hash "
            << formatLightingHash(p.probeHash) << "\n";
    return out.str();
}

bool loadReflectionManifest(const fs::path& file, ReflectionManifest& manifest, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readLightingFile(file, bytes, error)) return false;
    return parseReflectionManifest(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), file.generic_string(), manifest,
        error);
}

// ---- JSON ----

std::string reflectionCubeJson(const ReflectionCube& cube) {
    std::string out = "{\n  \"format\": \"reflection_cube\",\n  \"convention\": \"linear_rec709_radiance\",\n"
                      "  \"size\": " +
                      std::to_string(cube.size) +
                      ",\n  \"face_order\": [\"+x\", \"-x\", \"+y\", \"-y\", \"+z\", \"-z\"],\n"
                      "  \"face_means\": [";
    const auto means = reflectionFaceMeans(cube, 0);
    for (size_t f = 0; f < 6; ++f)
        out += std::string(f ? ", " : "") + "[" + number(means[f].r) + ", " + number(means[f].g) + ", " +
               number(means[f].b) + "]";
    out += "],\n  \"levels\": [";
    for (size_t l = 0; l < cube.levels.size(); ++l) {
        out += l ? ",\n    [" : "\n    [";
        const std::vector<float>& v = cube.levels[l];
        for (size_t i = 0; i < v.size(); ++i) {
            if (i % 16 == 0 && i)
                out += ",\n     ";
            else if (i)
                out += ", ";
            out += number(v[i]);
        }
        out += "]";
    }
    return out + "\n  ]\n}\n";
}

bool reflectionCubeFromJson(std::string_view text, ReflectionCube& cube, std::string& error) {
    Json root;
    if (!parseJson(text, root, error)) return false;
    const Json* format = root.field("format");
    if (root.type != Json::Type::Object || !format || format->text != "reflection_cube") {
        error = "not an exported reflection cube (format \"reflection_cube\")";
        return false;
    }
    ReflectionCube out;
    if (!uintField(root, "size", out.size, error)) return false;
    if (!validReflectionResolution(out.size)) {
        error = "size must be 64, 128, 256 or 512";
        return false;
    }
    const Json* levels = root.field("levels");
    if (!levels || levels->type != Json::Type::Array || levels->items.size() != kReflectionLevels) {
        error = "levels must be an array of " + std::to_string(kReflectionLevels) + " levels";
        return false;
    }
    for (uint32_t l = 0; l < kReflectionLevels; ++l) {
        std::vector<double> v;
        const size_t s = out.levelSize(l);
        if (!numbers(&levels->items[l], v, 6 * s * s * 4, error, "a level")) return false;
        out.levels.emplace_back(v.begin(), v.end());
    }
    cube = std::move(out);
    return true;
}

// ---- Schema ----

std::string reflectionManifestSchema() {
    return R"(## Reflection probes (.swreflection)

A scene's captured reflection probes (G07). `BakedLighting { reflections "..." }` names
the manifest; `player --capture-reflections` and the editor's Lighting window write it.
Capturing needs a GPU renderer; everything else here is checked on the CPU.

```
swreflection 1
scene "Scenes/Arena.swscene"
tool "shockwave reflections 1"
color linear_rec709_radiance
algorithm 1
scene_hash 0123456789abcdef
probe <32 hex digits> path "Lighting/Arena/Reflection_<id>.ktx2" hash H bytes N size 128 probe_hash H
```

| Key | Required | Meaning |
|---|---|---|
| `swreflection 1` | first line | Format and version |
| `scene` | yes | The scene captured, inside Content |
| `tool` | no | What captured it |
| `color linear_rec709_radiance` | no | Stored values are linear Rec.709 radiance, before exposure |
| `algorithm` | yes | Capture and filter version; another version is out of date |
| `scene_hash` | yes | Everything static the captures saw: geometry, lights, surroundings, the lighting bake. A change leaves probes usable but stale |
| `probe <id> path P hash H bytes N size S probe_hash H` | | One probe: its cube file and content hash, its resolution, and its own placement and settings when captured (a change drops that probe until it's captured again) |

Cube files are uncompressed `.ktx2`: `VK_FORMAT_R16G16B16A16_SFLOAT`, 6 faces (+X, -X,
+Y, -Y, +Z, -Z), 6 levels, linear. Level *L* holds roughness *L* / 5, the same steps
as the global specular cube; level 0 is the mirror image as captured. Face texel
(*i*, *j*) of a face of size *n* looks along `cubeFaceDirection(face, st)` with
`st = ((i, j) + 0.5) / n * 2 - 1` (Renderer/ReflectionProbes.h): the environment
cube's orientation, with the faces aligned to the world's axes whatever the probe's
rotation. A 128 cube is about 1 MiB on the GPU, 256 about 4 MiB, 512 about 16 MiB.
)";
}

}   // namespace shockwave
