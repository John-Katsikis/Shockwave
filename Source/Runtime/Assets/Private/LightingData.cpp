#include <Assets/LightingData.h>

#include "LightingJson.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

namespace shockwave {

namespace fs = std::filesystem;
using namespace lighting_json;

// ---- Identity ----

namespace {
bool hexDigit(char c, uint32_t& value) {
    if (c >= '0' && c <= '9') {
        value = uint32_t(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        value = uint32_t(c - 'a' + 10);
        return true;
    }
    return false;
}
bool parseHex64(std::string_view text, uint64_t& out) {
    if (text.size() != 16) return false;
    uint64_t value = 0;
    for (char c : text) {
        uint32_t digit = 0;
        if (!hexDigit(c, digit)) return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}
std::string hex64(uint64_t value) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}
uint64_t fnv(uint64_t hash, std::span<const uint8_t> data) {
    for (uint8_t b : data) hash = (hash ^ b) * 0x100000001b3ull;
    return hash;
}
void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
}   // namespace

bool parseBindingId(std::string_view text, BindingId& id) {
    if (text.size() != 32) return false;
    BindingId parsed;
    if (!parseHex64(text.substr(0, 16), parsed.hi) || !parseHex64(text.substr(16), parsed.lo)) return false;
    id = parsed;
    return true;
}

std::string formatBindingId(const BindingId& id) { return hex64(id.hi) + hex64(id.lo); }

BindingId newBindingId() {
    static thread_local std::mt19937_64 engine = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    BindingId id;
    while (!id.valid()) id = {engine(), engine()};
    return id;
}

BindingId deriveBindingId(const BindingId& instance, const BindingId& local) {
    static constexpr std::string_view kNamespace = "shockwave.bake.prefab.1";
    std::vector<uint8_t> data(kNamespace.begin(), kNamespace.end());
    for (uint64_t v : {instance.hi, instance.lo, local.hi, local.lo}) putU64(data, v);
    BindingId id{fnv(0xcbf29ce484222325ull, data), fnv(0x84222325cbf29ce4ull, data)};
    if (!id.valid()) id.lo = 1;
    return id;
}

// ---- Hashing ----

LightingHasher& LightingHasher::bytes(std::span<const uint8_t> data) {
    m_hash = fnv(m_hash, data);
    return *this;
}
LightingHasher& LightingHasher::u32(uint32_t value) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = uint8_t(value >> (8 * i));
    return bytes(b);
}
LightingHasher& LightingHasher::u64(uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = uint8_t(value >> (8 * i));
    return bytes(b);
}
LightingHasher& LightingHasher::f32(float value) {
    if (value == 0.0f) value = 0.0f;   // -0 hashes as +0
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return u32(bits);
}
LightingHasher& LightingHasher::mat(const glm::mat4& m) {
    for (int c = 0; c < 4; ++c) vec(m[c]);
    return *this;
}
LightingHasher& LightingHasher::text(std::string_view s) {
    u64(s.size());
    return bytes({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
}
uint64_t hashLightingBytes(std::span<const uint8_t> data) { return LightingHasher().bytes(data).value(); }
std::string formatLightingHash(uint64_t hash) { return hex64(hash); }
bool parseLightingHash(std::string_view text, uint64_t& hash) { return parseHex64(text, hash); }

// ---- Spherical harmonics ----

std::array<float, 9> shBasis(const glm::vec3& d) {
    return {
        0.282095f,
        0.488603f * d.y,
        0.488603f * d.z,
        0.488603f * d.x,
        1.092548f * d.x * d.y,
        1.092548f * d.y * d.z,
        0.315392f * (3.0f * d.z * d.z - 1.0f),
        1.092548f * d.x * d.z,
        0.546274f * (d.x * d.x - d.y * d.y),
    };
}

Sh9 shRadianceToIrradiance(const Sh9& radiance) {
    static constexpr float kBand[9] = {1.0f, 2.0f / 3, 2.0f / 3, 2.0f / 3, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
    Sh9 out;
    for (size_t i = 0; i < 9; ++i) out[i] = radiance[i] * kBand[i];
    return out;
}

glm::vec3 shEvaluate(const Sh9& sh, const glm::vec3& normal) {
    const auto y = shBasis(normal);
    glm::vec3 sum(0.0f);
    for (size_t i = 0; i < 9; ++i) sum += sh[i] * y[i];
    return glm::max(sum, glm::vec3(0.0f));
}

Sh9 shConstant(const glm::vec3& c) {
    // The radiance projection of a constant is c * 4 pi * Y00 in the first coefficient only.
    Sh9 sh{};
    sh[0] = c * (4.0f * 3.14159265358979f * 0.282095f);
    return shRadianceToIrradiance(sh);
}

// ---- Little-endian reading and writing ----

namespace {
struct Writer {
    std::vector<uint8_t>& out;
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    }
    void u64(uint64_t v) { putU64(out, v); }
    void f32(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    void raw(std::string_view s) { out.insert(out.end(), s.begin(), s.end()); }
};
struct Reader {
    std::span<const uint8_t> data;
    size_t at = 0;
    bool failed = false;
    bool need(size_t n) {
        if (failed || n > data.size() - at) failed = true;
        return !failed;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(data[at + i]) << (8 * i);
        at += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(data[at + i]) << (8 * i);
        at += 8;
        return v;
    }
    float f32() {
        const uint32_t bits = u32();
        float v = 0;
        std::memcpy(&v, &bits, 4);
        if (!std::isfinite(v)) failed = true;
        return v;
    }
    bool magic(std::string_view m) {
        if (!need(m.size())) return false;
        if (std::memcmp(data.data() + at, m.data(), m.size()) != 0) return failed = true, false;
        at += m.size();
        return true;
    }
    size_t left() const { return failed ? 0 : data.size() - at; }
};
constexpr std::string_view kBakeMagic{"SWBAKE\0\0", 8};
constexpr std::string_view kProbeMagic{"SWPROBE\0", 8};
}   // namespace

// ---- Lightmap .ktx2 ----

namespace {
constexpr uint8_t kKtx2Identifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
constexpr uint32_t kVkFormatRgba16f = 97;   // VK_FORMAT_R16G16B16A16_SFLOAT
constexpr uint32_t kDfdBlockSize = 24 + 16 * 4;
constexpr uint32_t kDfdTotal = 4 + kDfdBlockSize;
constexpr uint32_t kLevelIndexAt = 80;
constexpr uint32_t kDfdAt = kLevelIndexAt + 24;
constexpr uint32_t kDataAt = (kDfdAt + kDfdTotal + 7) / 8 * 8;
constexpr uint32_t kMaxLightmapSize = 16384;
}   // namespace

std::vector<uint8_t> lightmapHalfFloats(std::span<const float> rgba) {
    std::vector<uint8_t> out(rgba.size() * 2);
    for (size_t i = 0; i < rgba.size(); ++i) {
        const uint16_t h = static_cast<uint16_t>(glm::packHalf1x16(rgba[i]));
        out[2 * i] = uint8_t(h);
        out[2 * i + 1] = uint8_t(h >> 8);
    }
    return out;
}

bool encodeLightmapKtx2(
    uint32_t width, uint32_t height, std::span<const float> rgba, std::vector<uint8_t>& ktx2, std::string& error) {
    if (width == 0 || height == 0 || width > kMaxLightmapSize || height > kMaxLightmapSize) {
        error = "lightmap size must be 1.." + std::to_string(kMaxLightmapSize);
        return false;
    }
    if (rgba.size() != size_t(width) * height * 4) {
        error = "lightmap needs width x height x 4 values";
        return false;
    }
    for (float v : rgba)
        if (!std::isfinite(v)) {
            error = "lightmap has a value that isn't a finite number";
            return false;
        }
    const std::vector<uint8_t> halves = lightmapHalfFloats(rgba);
    std::vector<uint8_t> out;
    Writer w{out};
    out.insert(out.end(), std::begin(kKtx2Identifier), std::end(kKtx2Identifier));
    w.u32(kVkFormatRgba16f);
    w.u32(2);   // typeSize
    w.u32(width);
    w.u32(height);
    w.u32(0);   // depth
    w.u32(0);   // layers
    w.u32(1);   // faces
    w.u32(1);   // levels
    w.u32(0);   // no supercompression
    w.u32(kDfdAt);
    w.u32(kDfdTotal);
    w.u32(0);   // no key/value data
    w.u32(0);
    w.u64(0);   // no supercompression global data
    w.u64(0);
    w.u64(kDataAt);
    w.u64(halves.size());
    w.u64(halves.size());
    // Basic data format descriptor: RGBSDA, BT.709 primaries, linear, 8 bytes a texel, four
    // signed float 16-bit samples (R, G, B, A = channel 15).
    w.u32(kDfdTotal);
    w.u32(0);
    w.u32((kDfdBlockSize << 16) | 2);
    w.u32(1 | (1 << 8) | (1 << 16));
    w.u32(0);
    w.u32(8);
    w.u32(0);
    const uint32_t channels[4] = {0, 1, 2, 15};
    for (uint32_t c = 0; c < 4; ++c) {
        w.u32((c * 16) | (15u << 16) | ((0xC0u | channels[c]) << 24));
        w.u32(0);
        w.u32(0xBF800000u);
        w.u32(0x3F800000u);
    }
    out.resize(kDataAt, 0);
    out.insert(out.end(), halves.begin(), halves.end());
    ktx2 = std::move(out);
    return true;
}

bool decodeLightmapKtx2(
    std::span<const uint8_t> ktx2, uint32_t& width, uint32_t& height, std::vector<float>& rgba, std::string& error) {
    Reader r{ktx2};
    if (ktx2.size() < 12 || std::memcmp(ktx2.data(), kKtx2Identifier, 12) != 0) {
        error = "not a KTX2 file";
        return false;
    }
    r.at = 12;
    const uint32_t format = r.u32(), typeSize = r.u32(), w = r.u32(), h = r.u32();
    const uint32_t depth = r.u32(), layers = r.u32(), faces = r.u32(), levels = r.u32(), super = r.u32();
    r.at = kLevelIndexAt;
    const uint64_t offset = r.u64(), length = r.u64();
    r.u64();
    if (r.failed) {
        error = "KTX2 header is cut short";
        return false;
    }
    if (format != kVkFormatRgba16f || typeSize != 2 || depth != 0 || layers != 0 || faces != 1 || levels != 1 ||
        super != 0) {
        error = "lightmaps must be one level of uncompressed RGBA16F (VK_FORMAT_R16G16B16A16_SFLOAT)";
        return false;
    }
    if (w == 0 || h == 0 || w > kMaxLightmapSize || h > kMaxLightmapSize) {
        error = "lightmap size is out of range";
        return false;
    }
    const uint64_t expected = uint64_t(w) * h * 8;
    if (length != expected || offset > ktx2.size() || length > ktx2.size() - offset) {
        error = "lightmap level doesn't fit the file";
        return false;
    }
    rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); ++i) {
        const uint16_t bits = uint16_t(ktx2[offset + 2 * i] | (ktx2[offset + 2 * i + 1] << 8));
        rgba[i] = glm::unpackHalf1x16(bits);
        if (!std::isfinite(rgba[i])) {
            error = "lightmap has a value that isn't a finite number";
            return false;
        }
    }
    width = w;
    height = h;
    return true;
}

// ---- Receivers ----

bool writeLightingReceivers(std::span<const LightingReceiver> receivers, std::vector<uint8_t>& out) {
    out.clear();
    Writer w{out};
    w.raw(kBakeMagic);
    w.u32(1);
    w.u32(uint32_t(receivers.size()));
    for (const LightingReceiver& r : receivers) {
        if (r.lightmapUv.size() != r.sourceVertex.size()) return false;
        w.u64(r.binding.hi);
        w.u64(r.binding.lo);
        w.u32(r.part);
        w.u32(r.atlas);
        w.u64(r.geometryHash);
        w.u32(uint32_t(r.sourceVertex.size()));
        w.u32(uint32_t(r.indices.size()));
        for (size_t i = 0; i < r.sourceVertex.size(); ++i) {
            w.u32(r.sourceVertex[i]);
            w.f32(r.lightmapUv[i].x);
            w.f32(r.lightmapUv[i].y);
        }
        for (uint32_t index : r.indices) w.u32(index);
    }
    return true;
}

bool readLightingReceivers(
    std::span<const uint8_t> bytes, std::vector<LightingReceiver>& receivers, std::string& error) {
    Reader r{bytes};
    if (!r.magic(kBakeMagic)) {
        error = "not a receivers file (.swbake)";
        return false;
    }
    if (r.u32() != 1) {
        error = "receivers file version isn't 1";
        return false;
    }
    const uint32_t count = r.u32();
    // Each receiver takes at least 40 bytes: refuse counts the file can't hold.
    if (r.failed || count > r.left() / 40) {
        error = "receivers file is cut short";
        return false;
    }
    std::vector<LightingReceiver> out(count);
    for (LightingReceiver& rec : out) {
        rec.binding.hi = r.u64();
        rec.binding.lo = r.u64();
        rec.part = r.u32();
        rec.atlas = r.u32();
        rec.geometryHash = r.u64();
        const uint32_t vertices = r.u32(), indices = r.u32();
        if (r.failed || uint64_t(vertices) * 12 + uint64_t(indices) * 4 > r.left()) {
            error = "receivers file is cut short";
            return false;
        }
        if (indices % 3 != 0) {
            error = "receiver " + formatBindingId(rec.binding) + " has an index count that isn't whole triangles";
            return false;
        }
        rec.sourceVertex.resize(vertices);
        rec.lightmapUv.resize(vertices);
        for (uint32_t i = 0; i < vertices; ++i) {
            rec.sourceVertex[i] = r.u32();
            rec.lightmapUv[i].x = r.f32();
            rec.lightmapUv[i].y = r.f32();
        }
        rec.indices.resize(indices);
        for (uint32_t& index : rec.indices) {
            index = r.u32();
            if (index >= vertices) {
                error = "receiver " + formatBindingId(rec.binding) + " has an index past its vertices";
                return false;
            }
        }
        if (r.failed) {
            error = "receivers file has a value that isn't a finite number";
            return false;
        }
    }
    if (r.left() != 0) {
        error = "receivers file has bytes after its last receiver";
        return false;
    }
    receivers = std::move(out);
    return true;
}

// ---- Probes ----

glm::vec3 LightingProbeVolume::probePosition(uint32_t x, uint32_t y, uint32_t z) const {
    glm::vec3 local(0.0f);
    const uint32_t idx[3] = {x, y, z};
    for (int a = 0; a < 3; ++a)
        local[a] =
            counts[a] > 1 ? -halfExtents[a] + 2.0f * halfExtents[a] * float(idx[a]) / float(counts[a] - 1) : 0.0f;
    return glm::vec3(toWorld * glm::vec4(local, 1.0f));
}

bool writeLightingProbes(const LightingProbeSet& set, std::vector<uint8_t>& out) {
    out.clear();
    Writer w{out};
    w.raw(kProbeMagic);
    w.u32(1);
    for (const glm::vec3& c : set.environment) w.f32(c.r), w.f32(c.g), w.f32(c.b);
    w.u32(uint32_t(set.volumes.size()));
    for (const LightingProbeVolume& v : set.volumes) {
        if (uint64_t(v.counts.x) * v.counts.y * v.counts.z != v.probes.size()) return false;
        w.u64(v.binding.hi);
        w.u64(v.binding.lo);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) w.f32(v.toWorld[c][r]);
        for (int a = 0; a < 3; ++a) w.f32(v.halfExtents[a]);
        for (int a = 0; a < 3; ++a) w.u32(v.counts[a]);
        w.f32(v.blendDistance);
        w.u32(uint32_t(v.priority));
        w.f32(v.maxDistance);
        for (const LightingProbe& p : v.probes) {
            w.u32(p.valid ? 1u : 0u);
            for (const glm::vec3& c : p.sh) w.f32(c.r), w.f32(c.g), w.f32(c.b);
            for (const glm::vec2& d : p.distance) w.f32(d.x), w.f32(d.y);
        }
    }
    return true;
}

bool readLightingProbes(std::span<const uint8_t> bytes, LightingProbeSet& set, std::string& error) {
    Reader r{bytes};
    if (!r.magic(kProbeMagic)) {
        error = "not a probes file (.swprobe)";
        return false;
    }
    if (r.u32() != 1) {
        error = "probes file version isn't 1";
        return false;
    }
    LightingProbeSet out;
    for (glm::vec3& c : out.environment) c = {r.f32(), r.f32(), r.f32()};
    const uint32_t count = r.u32();
    if (r.failed || count > r.left() / 128) {
        error = "probes file is cut short";
        return false;
    }
    constexpr uint64_t kProbeBytes = 4 + 27 * 4 + kProbeVisibilityTexels * 8;
    out.volumes.resize(count);
    for (LightingProbeVolume& v : out.volumes) {
        v.binding.hi = r.u64();
        v.binding.lo = r.u64();
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) v.toWorld[c][row] = r.f32();
        for (int a = 0; a < 3; ++a) v.halfExtents[a] = r.f32();
        for (int a = 0; a < 3; ++a) v.counts[a] = r.u32();
        v.blendDistance = r.f32();
        v.priority = int32_t(r.u32());
        v.maxDistance = r.f32();
        if (r.failed) {
            error = "probes file is cut short or has a value that isn't a finite number";
            return false;
        }
        const uint64_t probes = uint64_t(v.counts.x) * v.counts.y * v.counts.z;
        if (v.counts.x == 0 || v.counts.y == 0 || v.counts.z == 0 || probes > kMaxProbesPerVolume) {
            error = "probe volume " + formatBindingId(v.binding) + " has a grid of " + std::to_string(probes) +
                    " probes; 1.." + std::to_string(kMaxProbesPerVolume) + " are allowed";
            return false;
        }
        if (probes * kProbeBytes > r.left()) {
            error = "probes file is cut short";
            return false;
        }
        v.probes.resize(size_t(probes));
        for (LightingProbe& p : v.probes) {
            const uint32_t valid = r.u32();
            if (valid > 1) r.failed = true;
            p.valid = valid == 1;
            for (glm::vec3& c : p.sh) c = {r.f32(), r.f32(), r.f32()};
            for (glm::vec2& d : p.distance) d = {r.f32(), r.f32()};
        }
        if (r.failed) {
            error = "probes file has a value that isn't a finite number or a validity that isn't 0 or 1";
            return false;
        }
    }
    if (r.left() != 0) {
        error = "probes file has bytes after its last volume";
        return false;
    }
    set = std::move(out);
    return true;
}

glm::vec2 octahedralEncode(const glm::vec3& d) {
    glm::vec3 n = d / (std::abs(d.x) + std::abs(d.y) + std::abs(d.z));
    glm::vec2 p(n.x, n.z);
    if (n.y < 0.0f) {
        p = (1.0f - glm::abs(glm::vec2(p.y, p.x))) * glm::vec2(p.x >= 0.0f ? 1.0f : -1.0f, p.y >= 0.0f ? 1.0f : -1.0f);
    }
    return p * 0.5f + 0.5f;
}

glm::vec3 octahedralDecode(const glm::vec2& uv) {
    const glm::vec2 p = uv * 2.0f - 1.0f;
    glm::vec3 n(p.x, 1.0f - std::abs(p.x) - std::abs(p.y), p.y);
    if (n.y < 0.0f) {
        const glm::vec2 folded =
            (1.0f - glm::abs(glm::vec2(n.z, n.x))) * glm::vec2(n.x >= 0.0f ? 1.0f : -1.0f, n.z >= 0.0f ? 1.0f : -1.0f);
        n.x = folded.x;
        n.z = folded.y;
    }
    return glm::normalize(n);
}

float probeVisibility(const LightingProbe& probe, const glm::vec3& direction, float distance, float spacing) {
    // Bilinear lookup of the moments, clamped at the map's edges.
    const glm::vec2 uv = octahedralEncode(direction) * float(kProbeVisibilitySize) - 0.5f;
    const glm::vec2 base = glm::floor(uv);
    const glm::vec2 f = uv - base;
    glm::vec2 moments(0.0f);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int x = std::clamp(int(base.x) + i, 0, int(kProbeVisibilitySize) - 1);
            const int y = std::clamp(int(base.y) + j, 0, int(kProbeVisibilitySize) - 1);
            const float w = (i ? f.x : 1.0f - f.x) * (j ? f.y : 1.0f - f.y);
            moments += probe.distance[size_t(y) * kProbeVisibilitySize + size_t(x)] * w;
        }
    // A little slack so a receiver right on a surface the probe sees isn't rejected.
    const float d = std::max(0.0f, distance - 0.05f * spacing);
    if (d <= moments.x) return 1.0f;
    // Chebyshev's bound on the chance the path is clear, with a variance floor so a probe
    // whose rays all hit at one distance still fades rather than cutting off.
    const float floorVariance = (0.02f * spacing) * (0.02f * spacing);
    const float variance = std::max(moments.y - moments.x * moments.x, floorVariance);
    const float delta = d - moments.x;
    const float chebyshev = variance / (variance + delta * delta);
    return chebyshev * chebyshev * chebyshev;
}

namespace {
// One volume's interpolated lighting at a point inside it (local coordinates given).
bool sampleVolume(
    const LightingProbeVolume& v, const glm::vec3& point, const glm::vec3& local, Sh9& out, bool& fallback) {
    const glm::vec3 scale(
        glm::length(glm::vec3(v.toWorld[0])), glm::length(glm::vec3(v.toWorld[1])),
        glm::length(glm::vec3(v.toWorld[2])));
    float spacing = std::numeric_limits<float>::max();
    glm::uvec3 i0(0u), i1(0u);
    glm::vec3 f(0.0f);
    for (int a = 0; a < 3; ++a) {
        if (v.counts[a] <= 1) continue;
        const float cells = float(v.counts[a] - 1);
        spacing = std::min(spacing, 2.0f * v.halfExtents[a] * scale[a] / cells);
        const float g = std::clamp((local[a] + v.halfExtents[a]) / (2.0f * v.halfExtents[a]) * cells, 0.0f, cells);
        i0[a] = std::min(uint32_t(g), v.counts[a] - 2);
        i1[a] = i0[a] + 1;
        f[a] = g - float(i0[a]);
    }
    if (spacing == std::numeric_limits<float>::max()) spacing = std::max(v.maxDistance, 1e-3f);
    auto probeAt = [&](uint32_t x, uint32_t y, uint32_t z) -> const LightingProbe& {
        return v.probes[x + v.counts.x * (y + size_t(v.counts.y) * z)];
    };
    Sh9 sum{};
    float total = 0.0f;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        const glm::uvec3 c((corner & 1) ? i1.x : i0.x, (corner & 2) ? i1.y : i0.y, (corner & 4) ? i1.z : i0.z);
        float w = 1.0f;
        for (int a = 0; a < 3; ++a) w *= (corner >> a) & 1 ? f[a] : 1.0f - f[a];
        if (w <= 0.0f) continue;
        const LightingProbe& p = probeAt(c.x, c.y, c.z);
        if (!p.valid) continue;
        const glm::vec3 offset = point - v.probePosition(c.x, c.y, c.z);
        const float distance = glm::length(offset);
        if (distance > 1e-5f) w *= probeVisibility(p, offset / distance, distance, spacing);
        if (w <= 1e-6f) continue;
        for (size_t k = 0; k < 9; ++k) sum[k] += p.sh[k] * w;
        total += w;
    }
    if (total > 1e-5f) {
        for (size_t k = 0; k < 9; ++k) out[k] = sum[k] / total;
        return true;
    }
    // Every neighbour is invalid or behind a wall: the nearest valid probe close by that
    // sees the point, never one merely nearest through a wall.
    float best = std::numeric_limits<float>::max();
    const LightingProbe* chosen = nullptr;
    const glm::ivec3 centre(i0);
    for (int z = centre.z - 2; z <= centre.z + 3; ++z)
        for (int y = centre.y - 2; y <= centre.y + 3; ++y)
            for (int x = centre.x - 2; x <= centre.x + 3; ++x) {
                if (x < 0 || y < 0 || z < 0 || x >= int(v.counts.x) || y >= int(v.counts.y) || z >= int(v.counts.z))
                    continue;
                const LightingProbe& p = probeAt(uint32_t(x), uint32_t(y), uint32_t(z));
                if (!p.valid) continue;
                const glm::vec3 offset = point - v.probePosition(uint32_t(x), uint32_t(y), uint32_t(z));
                const float distance = glm::length(offset);
                if (distance >= best) continue;
                if (distance > 1e-5f && probeVisibility(p, offset / distance, distance, spacing) < 0.5f) continue;
                best = distance;
                chosen = &p;
            }
    if (!chosen) return false;
    out = chosen->sh;
    fallback = true;
    return true;
}
}   // namespace

ProbeSample sampleProbes(const LightingProbeSet& set, const glm::vec3& point) {
    struct Inside {
        const LightingProbeVolume* volume;
        glm::vec3 local;
        float boundary;
        float size;
    };
    std::vector<Inside> inside;
    for (const LightingProbeVolume& v : set.volumes) {
        if (v.probes.empty()) continue;
        const glm::vec3 local = glm::vec3(glm::inverse(v.toWorld) * glm::vec4(point, 1.0f));
        bool contained = true;
        float boundary = 1.0f;
        for (int a = 0; a < 3; ++a) {
            const float slack = v.halfExtents[a] - std::abs(local[a]);
            if (slack < -1e-4f) contained = false;
            const float scale = glm::length(glm::vec3(v.toWorld[a]));
            if (v.blendDistance > 0.0f) boundary = std::min(boundary, std::max(slack, 0.0f) * scale / v.blendDistance);
        }
        if (!contained) continue;
        const glm::vec3 extent =
            v.halfExtents * glm::vec3(
                                glm::length(glm::vec3(v.toWorld[0])), glm::length(glm::vec3(v.toWorld[1])),
                                glm::length(glm::vec3(v.toWorld[2])));
        inside.push_back({&v, local, std::clamp(boundary, 0.0f, 1.0f), extent.x * extent.y * extent.z});
    }
    // Higher priority first; among equals the smaller (more specific) volume, then file order.
    std::stable_sort(inside.begin(), inside.end(), [](const Inside& a, const Inside& b) {
        if (a.volume->priority != b.volume->priority) return a.volume->priority > b.volume->priority;
        return a.size < b.size;
    });
    ProbeSample result;
    float remaining = 1.0f;
    for (const Inside& in : inside) {
        Sh9 sh{};
        bool fallback = false;
        if (!sampleVolume(*in.volume, point, in.local, sh, fallback)) continue;
        // A volume with no blend distance covers fully right up to its faces.
        const float w = (in.volume->blendDistance > 0.0f ? in.boundary : 1.0f) * remaining;
        if (w <= 0.0f) continue;
        for (size_t k = 0; k < 9; ++k) result.sh[k] += sh[k] * w;
        remaining -= w;
        result.coverage += w;
        ++result.volumesUsed;
        result.fallback = result.fallback || fallback;
        if (remaining <= 1e-6f) break;
    }
    for (size_t k = 0; k < 9; ++k) result.sh[k] += set.environment[k] * remaining;
    return result;
}

// ---- Manifest ----

const char* lightingResourceKindName(LightingResource::Kind kind) {
    switch (kind) {
    case LightingResource::Kind::Lightmap: return "lightmap";
    case LightingResource::Kind::Receivers: return "receivers";
    case LightingResource::Kind::Probes: return "probes";
    }
    return "?";
}

const LightingResource* LightingManifest::resource(LightingResource::Kind kind, uint32_t index) const {
    uint32_t seen = 0;
    for (const LightingResource& r : resources)
        if (r.kind == kind && seen++ == index) return &r;
    return nullptr;
}

bool parseLightingManifest(
    std::string_view text, const std::string& file, LightingManifest& manifest, std::string& error) {
    LightingManifest m;
    m.quality.clear();
    std::set<std::string> seen;
    std::set<std::pair<BindingId, uint32_t>> receivers;
    std::set<BindingId> volumes;
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
            if (w.size() != 2 || w[0] != "swlighting") return fail("the first line must be \"swlighting 1\"");
            if (w[1] != "1") return fail("version " + w[1] + " isn't supported; this build reads swlighting 1");
            header = true;
            continue;
        }
        const std::string& key = w[0];
        // Key/value pairs after a record's leading values: "hash 0123... width 1024".
        auto pairs = [&](size_t from, const std::vector<std::string>& allowed,
                         std::vector<std::pair<std::string, std::string>>& out) {
            if ((w.size() - from) % 2 != 0) return fail(key + ": every field needs a value");
            std::set<std::string> fields;
            for (size_t i = from; i < w.size(); i += 2) {
                if (std::find(allowed.begin(), allowed.end(), w[i]) == allowed.end())
                    return fail(key + ": unknown field \"" + w[i] + "\"");
                if (!fields.insert(w[i]).second) return fail(key + ": \"" + w[i] + "\" is given twice");
                out.push_back({w[i], w[i + 1]});
            }
            return true;
        };
        const bool record = key == "resource" || key == "receiver" || key == "volume";
        if (!record && !seen.insert(key).second) return fail("\"" + key + "\" is given twice");
        auto single = [&]() -> const std::string* {
            if (w.size() != 2) {
                fail(key + " takes one value");
                return nullptr;
            }
            return &w[1];
        };
        auto unsignedField = [&](uint32_t& out, uint32_t lo, uint32_t hi) {
            const std::string* v = single();
            if (!v) return false;
            if (!u32Value(*v, out) || out < lo || out > hi)
                return fail(key + " must be a whole number in " + std::to_string(lo) + ".." + std::to_string(hi));
            return true;
        };
        auto hashField = [&](uint64_t& out) {
            const std::string* v = single();
            if (!v) return false;
            if (!parseLightingHash(*v, out)) return fail(key + " must be 16 lowercase hex digits");
            return true;
        };
        if (key == "scene") {
            const std::string* v = single();
            if (!v) return false;
            m.scene = *v;
        } else if (key == "tool") {
            const std::string* v = single();
            if (!v) return false;
            m.tool = *v;
        } else if (key == "color") {
            const std::string* v = single();
            if (!v) return false;
            if (*v != "linear_rec709_irradiance_over_pi")
                return fail("color must be linear_rec709_irradiance_over_pi (B = E / pi, linear)");
            m.colorConvention = *v;
        } else if (key == "quality") {
            const std::string* v = single();
            if (!v) return false;
            if (*v != "preview" && *v != "final") return fail("quality must be preview or final");
            m.quality = *v;
        } else if (key == "algorithm") {
            if (!unsignedField(m.algorithm, 1, 1000000)) return false;
        } else if (key == "samples") {
            if (!unsignedField(m.samples, 1, 1u << 20)) return false;
        } else if (key == "bounces") {
            if (!unsignedField(m.bounces, 0, 16)) return false;
        } else if (key == "seed") {
            if (!unsignedField(m.seed, 0, UINT32_MAX)) return false;
        } else if (key == "atlas_size") {
            if (!unsignedField(m.atlasSize, 64, 16384)) return false;
        } else if (key == "probe_rays") {
            if (!unsignedField(m.probeRays, 16, 65536)) return false;
        } else if (key == "lighting_hash") {
            if (!hashField(m.lightingHash)) return false;
        } else if (key == "geometry_hash") {
            if (!hashField(m.geometryHash)) return false;
        } else if (key == "resource") {
            if (w.size() < 3)
                return fail("resource needs a kind and a path: resource lightmap \"Lighting/...\" hash ...");
            LightingResource r;
            if (w[1] == "lightmap")
                r.kind = LightingResource::Kind::Lightmap;
            else if (w[1] == "receivers")
                r.kind = LightingResource::Kind::Receivers;
            else if (w[1] == "probes")
                r.kind = LightingResource::Kind::Probes;
            else
                return fail("resource kind must be lightmap, receivers or probes");
            r.path = w[2];
            if (r.path.empty() || r.path.find("..") != std::string::npos || r.path[0] == '/' ||
                r.path.find('\\') != std::string::npos || r.path.find(':') != std::string::npos)
                return fail("resource path must be inside Content, with forward slashes");
            std::vector<std::pair<std::string, std::string>> fields;
            const bool lightmap = r.kind == LightingResource::Kind::Lightmap;
            if (!pairs(
                    3,
                    lightmap ? std::vector<std::string>{"hash", "bytes", "width", "height"}
                             : std::vector<std::string>{"hash", "bytes"},
                    fields))
                return false;
            bool hasHash = false, hasBytes = false, hasWidth = false, hasHeight = false;
            for (auto& [field, value] : fields) {
                if (field == "hash") {
                    if (!parseLightingHash(value, r.hash)) return fail("resource hash must be 16 lowercase hex digits");
                    hasHash = true;
                } else if (field == "bytes") {
                    const auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), r.bytes);
                    if (ec != std::errc{} || p != value.data() + value.size())
                        return fail("resource bytes must be a whole number");
                    hasBytes = true;
                } else if (field == "width") {
                    if (!u32Value(value, r.width) || r.width == 0 || r.width > 16384)
                        return fail("width must be 1..16384");
                    hasWidth = true;
                } else if (field == "height") {
                    if (!u32Value(value, r.height) || r.height == 0 || r.height > 16384)
                        return fail("height must be 1..16384");
                    hasHeight = true;
                }
            }
            if (!hasHash || !hasBytes) return fail("resource needs hash and bytes");
            if (lightmap && (!hasWidth || !hasHeight)) return fail("a lightmap resource needs width and height");
            for (const LightingResource& other : m.resources)
                if (other.path == r.path) return fail("resource \"" + r.path + "\" is listed twice");
            m.resources.push_back(r);
        } else if (key == "receiver" || key == "volume") {
            if (w.size() < 2) return fail(key + " needs a binding id");
            LightingBinding b;
            if (!parseBindingId(w[1], b.binding) || !b.binding.valid())
                return fail(key + " binding must be 32 lowercase hex digits, not all zero");
            std::vector<std::pair<std::string, std::string>> fields;
            const bool receiver = key == "receiver";
            if (!pairs(
                    2, receiver ? std::vector<std::string>{"part", "atlas", "hash"} : std::vector<std::string>{"hash"},
                    fields))
                return false;
            bool hasHash = false;
            for (auto& [field, value] : fields) {
                if (field == "hash") {
                    if (!parseLightingHash(value, b.hash)) return fail(key + " hash must be 16 lowercase hex digits");
                    hasHash = true;
                } else if (field == "part") {
                    if (!u32Value(value, b.part)) return fail("receiver part must be a whole number");
                } else if (field == "atlas") {
                    if (!u32Value(value, b.atlas) || b.atlas > 255) return fail("receiver atlas must be 0..255");
                }
            }
            if (!hasHash) return fail(key + " needs a hash");
            if (receiver) {
                if (!receivers.insert({b.binding, b.part}).second)
                    return fail("receiver " + w[1] + " part " + std::to_string(b.part) + " is listed twice");
                m.receivers.push_back(b);
            } else {
                if (!volumes.insert(b.binding).second) return fail("volume " + w[1] + " is listed twice");
                m.volumes.push_back(b);
            }
        } else {
            return fail("unknown key \"" + key + "\"");
        }
        if (end >= text.size()) break;
    }
    if (!header) {
        lineNumber = 1;
        return fail("the first line must be \"swlighting 1\"");
    }
    for (const char* required :
         {"scene", "algorithm", "quality", "samples", "bounces", "seed", "atlas_size", "lighting_hash",
          "geometry_hash"})
        if (!seen.count(required)) {
            error = file + ": \"" + required + "\" is missing";
            return false;
        }
    for (const LightingBinding& r : m.receivers)
        if (!m.resource(LightingResource::Kind::Lightmap, r.atlas)) {
            error = file + ": receiver " + formatBindingId(r.binding) + " uses lightmap " + std::to_string(r.atlas) +
                    ", which isn't listed";
            return false;
        }
    manifest = std::move(m);
    return true;
}

std::string lightingManifestText(const LightingManifest& m) {
    std::ostringstream out;
    out << "swlighting 1\n";
    out << "# Generated by the lighting baker (docs/Lighting.md). Rebake rather than edit.\n";
    out << "scene " << quotedText(m.scene) << "\n";
    if (!m.tool.empty()) out << "tool " << quotedText(m.tool) << "\n";
    out << "color " << m.colorConvention << "\n";
    out << "algorithm " << m.algorithm << "\n";
    out << "quality " << m.quality << "\n";
    out << "samples " << m.samples << "\n";
    out << "bounces " << m.bounces << "\n";
    out << "seed " << m.seed << "\n";
    out << "atlas_size " << m.atlasSize << "\n";
    out << "probe_rays " << m.probeRays << "\n";
    out << "lighting_hash " << formatLightingHash(m.lightingHash) << "\n";
    out << "geometry_hash " << formatLightingHash(m.geometryHash) << "\n";
    for (const LightingResource& r : m.resources) {
        out << "resource " << lightingResourceKindName(r.kind) << " " << quotedText(r.path) << " hash "
            << formatLightingHash(r.hash) << " bytes " << r.bytes;
        if (r.kind == LightingResource::Kind::Lightmap) out << " width " << r.width << " height " << r.height;
        out << "\n";
    }
    for (const LightingBinding& b : m.receivers)
        out << "receiver " << formatBindingId(b.binding) << " part " << b.part << " atlas " << b.atlas << " hash "
            << formatLightingHash(b.hash) << "\n";
    for (const LightingBinding& b : m.volumes)
        out << "volume " << formatBindingId(b.binding) << " hash " << formatLightingHash(b.hash) << "\n";
    return out.str();
}

bool readLightingFile(const fs::path& file, std::vector<uint8_t>& bytes, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "can't open " + file.generic_string();
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "can't read " + file.generic_string();
        return false;
    }
    return true;
}

bool writeLightingFileAtomic(const fs::path& file, std::span<const uint8_t> bytes, std::string& error) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    const fs::path temp = file.parent_path() / ("." + file.filename().string() + ".writing");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "can't write " + temp.generic_string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        if (!out) {
            error = "can't write " + temp.generic_string();
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        error = "can't replace " + file.generic_string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool loadLightingManifest(const fs::path& file, LightingManifest& manifest, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readLightingFile(file, bytes, error)) return false;
    return parseLightingManifest(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), file.generic_string(), manifest,
        error);
}

// ---- JSON ----

namespace {
std::string shJson(const Sh9& sh) {
    std::string out = "[";
    for (size_t i = 0; i < 9; ++i)
        for (int c = 0; c < 3; ++c) out += (i || c ? ", " : "") + number(sh[i][c]);
    return out + "]";
}
bool shFrom(const Json* j, Sh9& sh, std::string& error) {
    std::vector<double> v;
    if (!numbers(j, v, 27, error, "sh")) return false;
    for (size_t i = 0; i < 9; ++i) sh[i] = glm::vec3(float(v[3 * i]), float(v[3 * i + 1]), float(v[3 * i + 2]));
    return true;
}
}   // namespace

std::string receiversJson(std::span<const LightingReceiver> receivers) {
    std::string out = "{\n  \"format\": \"swbake\",\n  \"version\": 1,\n  \"receivers\": [";
    for (size_t r = 0; r < receivers.size(); ++r) {
        const LightingReceiver& rec = receivers[r];
        out += r ? ",\n    {" : "\n    {";
        out += "\"binding\": \"" + formatBindingId(rec.binding) + "\", \"part\": " + std::to_string(rec.part) +
               ", \"atlas\": " + std::to_string(rec.atlas) + ", \"geometry_hash\": \"" +
               formatLightingHash(rec.geometryHash) + "\",\n     \"source_vertex\": [";
        for (size_t i = 0; i < rec.sourceVertex.size(); ++i)
            out += (i ? ", " : "") + std::to_string(rec.sourceVertex[i]);
        out += "],\n     \"lightmap_uv\": [";
        for (size_t i = 0; i < rec.lightmapUv.size(); ++i)
            out += (i ? ", " : "") + number(rec.lightmapUv[i].x) + ", " + number(rec.lightmapUv[i].y);
        out += "],\n     \"indices\": [";
        for (size_t i = 0; i < rec.indices.size(); ++i) out += (i ? ", " : "") + std::to_string(rec.indices[i]);
        out += "]}";
    }
    return out + (receivers.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

bool receiversFromJson(std::string_view text, std::vector<LightingReceiver>& receivers, std::string& error) {
    Json root;
    if (!parseJson(text, root, error)) return false;
    const Json* format = root.field("format");
    if (root.type != Json::Type::Object || !format || format->text != "swbake") {
        error = "not an exported receivers file (format \"swbake\")";
        return false;
    }
    const Json* list = root.field("receivers");
    if (!list || list->type != Json::Type::Array) {
        error = "receivers must be an array";
        return false;
    }
    std::vector<LightingReceiver> out;
    for (const Json& j : list->items) {
        LightingReceiver r;
        std::vector<double> v;
        if (!idField(j, "binding", r.binding, error) || !uintField(j, "part", r.part, error) ||
            !uintField(j, "atlas", r.atlas, error) || !hashField(j, "geometry_hash", r.geometryHash, error))
            return false;
        if (!numbers(j.field("source_vertex"), v, SIZE_MAX, error, "source_vertex")) return false;
        for (double d : v) r.sourceVertex.push_back(uint32_t(d));
        if (!numbers(j.field("lightmap_uv"), v, r.sourceVertex.size() * 2, error, "lightmap_uv")) return false;
        for (size_t i = 0; i < r.sourceVertex.size(); ++i)
            r.lightmapUv.push_back({float(v[2 * i]), float(v[2 * i + 1])});
        if (!numbers(j.field("indices"), v, SIZE_MAX, error, "indices")) return false;
        for (double d : v) {
            if (d < 0 || d >= double(r.sourceVertex.size())) {
                error = "receiver index past its vertices";
                return false;
            }
            r.indices.push_back(uint32_t(d));
        }
        out.push_back(std::move(r));
    }
    receivers = std::move(out);
    return true;
}

std::string probesJson(const LightingProbeSet& set) {
    std::string out =
        "{\n  \"format\": \"swprobe\",\n  \"version\": 1,\n  \"environment\": " + shJson(set.environment) +
        ",\n  \"volumes\": [";
    for (size_t vi = 0; vi < set.volumes.size(); ++vi) {
        const LightingProbeVolume& v = set.volumes[vi];
        out += vi ? ",\n    {" : "\n    {";
        out += "\"binding\": \"" + formatBindingId(v.binding) + "\", \"to_world\": [";
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) out += (c || r ? ", " : "") + number(v.toWorld[c][r]);
        out += "], \"half_extents\": [" + number(v.halfExtents.x) + ", " + number(v.halfExtents.y) + ", " +
               number(v.halfExtents.z) + "], \"counts\": [" + std::to_string(v.counts.x) + ", " +
               std::to_string(v.counts.y) + ", " + std::to_string(v.counts.z) +
               "], \"blend_distance\": " + number(v.blendDistance) + ", \"priority\": " + std::to_string(v.priority) +
               ", \"max_distance\": " + number(v.maxDistance) + ",\n     \"probes\": [";
        for (size_t p = 0; p < v.probes.size(); ++p) {
            const LightingProbe& probe = v.probes[p];
            out += p ? ",\n       {" : "\n       {";
            out += std::string("\"valid\": ") + (probe.valid ? "true" : "false") + ", \"sh\": " + shJson(probe.sh) +
                   ", \"distance\": [";
            for (size_t t = 0; t < kProbeVisibilityTexels; ++t)
                out += (t ? ", " : "") + number(probe.distance[t].x) + ", " + number(probe.distance[t].y);
            out += "]}";
        }
        out += "]}";
    }
    return out + (set.volumes.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

bool probesFromJson(std::string_view text, LightingProbeSet& set, std::string& error) {
    Json root;
    if (!parseJson(text, root, error)) return false;
    const Json* format = root.field("format");
    if (root.type != Json::Type::Object || !format || format->text != "swprobe") {
        error = "not an exported probes file (format \"swprobe\")";
        return false;
    }
    LightingProbeSet out;
    if (!shFrom(root.field("environment"), out.environment, error)) return false;
    const Json* list = root.field("volumes");
    if (!list || list->type != Json::Type::Array) {
        error = "volumes must be an array";
        return false;
    }
    for (const Json& j : list->items) {
        LightingProbeVolume v;
        std::vector<double> n;
        if (!idField(j, "binding", v.binding, error)) return false;
        if (!numbers(j.field("to_world"), n, 16, error, "to_world")) return false;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) v.toWorld[c][r] = float(n[size_t(c * 4 + r)]);
        if (!numbers(j.field("half_extents"), n, 3, error, "half_extents")) return false;
        v.halfExtents = {float(n[0]), float(n[1]), float(n[2])};
        if (!numbers(j.field("counts"), n, 3, error, "counts")) return false;
        for (int a = 0; a < 3; ++a) {
            if (n[size_t(a)] < 1 || n[size_t(a)] > kMaxProbesPerVolume) {
                error = "counts must be 1.." + std::to_string(kMaxProbesPerVolume);
                return false;
            }
            v.counts[a] = uint32_t(n[size_t(a)]);
        }
        if (uint64_t(v.counts.x) * v.counts.y * v.counts.z > kMaxProbesPerVolume) {
            error = "a volume holds at most " + std::to_string(kMaxProbesPerVolume) + " probes";
            return false;
        }
        const Json* blend = j.field("blend_distance");
        const Json* priority = j.field("priority");
        const Json* maxDistance = j.field("max_distance");
        if (!blend || blend->type != Json::Type::Number || !priority || priority->type != Json::Type::Number ||
            !maxDistance || maxDistance->type != Json::Type::Number) {
            error = "blend_distance, priority and max_distance must be numbers";
            return false;
        }
        v.blendDistance = float(blend->number);
        v.priority = int32_t(priority->number);
        v.maxDistance = float(maxDistance->number);
        const Json* probes = j.field("probes");
        if (!probes || probes->type != Json::Type::Array ||
            probes->items.size() != size_t(v.counts.x) * v.counts.y * v.counts.z) {
            error = "probes must hold counts.x * counts.y * counts.z entries";
            return false;
        }
        for (const Json& pj : probes->items) {
            LightingProbe p;
            const Json* valid = pj.field("valid");
            if (!valid || valid->type != Json::Type::Bool) {
                error = "probe valid must be true or false";
                return false;
            }
            p.valid = valid->number != 0;
            if (!shFrom(pj.field("sh"), p.sh, error)) return false;
            if (!numbers(pj.field("distance"), n, kProbeVisibilityTexels * 2, error, "distance")) return false;
            for (size_t t = 0; t < kProbeVisibilityTexels; ++t) p.distance[t] = {float(n[2 * t]), float(n[2 * t + 1])};
            v.probes.push_back(p);
        }
        out.volumes.push_back(std::move(v));
    }
    set = std::move(out);
    return true;
}

std::string lightmapJson(uint32_t width, uint32_t height, std::span<const float> rgba) {
    std::string out = "{\n  \"format\": \"lightmap\",\n  \"convention\": \"linear_rec709_irradiance_over_pi\",\n"
                      "  \"width\": " +
                      std::to_string(width) + ",\n  \"height\": " + std::to_string(height) + ",\n  \"rgba\": [";
    for (size_t i = 0; i < rgba.size(); ++i) {
        if (i % 16 == 0)
            out += i ? ",\n    " : "\n    ";
        else
            out += ", ";
        out += number(rgba[i]);
    }
    return out + "\n  ]\n}\n";
}

bool lightmapFromJson(
    std::string_view text, uint32_t& width, uint32_t& height, std::vector<float>& rgba, std::string& error) {
    Json root;
    if (!parseJson(text, root, error)) return false;
    const Json* format = root.field("format");
    if (root.type != Json::Type::Object || !format || format->text != "lightmap") {
        error = "not an exported lightmap (format \"lightmap\")";
        return false;
    }
    uint32_t w = 0, h = 0;
    if (!uintField(root, "width", w, error) || !uintField(root, "height", h, error)) return false;
    if (w == 0 || h == 0 || w > kMaxLightmapSize || h > kMaxLightmapSize) {
        error = "lightmap size is out of range";
        return false;
    }
    std::vector<double> v;
    if (!numbers(root.field("rgba"), v, size_t(w) * h * 4, error, "rgba")) return false;
    rgba.assign(v.begin(), v.end());
    width = w;
    height = h;
    return true;
}

// ---- Schema ----

std::string lightingManifestSchema() {
    return R"(# Lighting schema

Generated by `player --lighting-schema`; don't edit by hand. The guide is
[Lighting.md](Lighting.md).

## Conventions

- Stored diffuse lighting is **B = E / pi**: linear Rec.709 scene RGB, before exposure
  and tone mapping. It is the light arriving at a surface, never the surface's colour
  times it. The environment's irradiance cube uses the same convention, so a lightmap
  texel or a probe replaces it one for one (`color linear_rec709_irradiance_over_pi`).
- Lightmaps hold the receiver's occluded environment light, light bounced off static
  surfaces, and static emission. They never hold the receiver's own direct light from
  lights: those stay real-time, with their shadows.
- Binding ids are 128 bits written as 32 lowercase hex digits. Objects inside a prefab
  copy use `deriveBindingId(copy, object)` once per enclosing copy, innermost first:
  two FNV-1a 64-bit hashes (offset bases `cbf29ce484222325` and `84222325cbf29ce4`) of
  `shockwave.bake.prefab.1` followed by the copy's then the object's id (hi, lo, each
  64-bit little-endian).
- Content hashes are FNV-1a 64-bit over little-endian values, floats by their bits.

## Manifest: `Content/Lighting/<Scene>.swlighting`

A text file. The first line is `swlighting 1`. Each further line is a key and its
values; `#` starts a comment; strings with spaces are "quoted" (`\"` and `\\` escape).
Unknown keys, repeated keys and bad values are errors with the line number.

| Key | Values | Meaning |
|---|---|---|
| `scene` | "path" | The scene baked, inside Content |
| `tool` | "text" | What wrote it (optional) |
| `color` | `linear_rec709_irradiance_over_pi` | Stored value convention |
| `algorithm` | 1.. | Estimator and unwrap version; another one is stale |
| `quality` | `preview` or `final` | Bake quality |
| `samples` | 1.. | Paths per lightmap texel |
| `bounces` | 0..16 | Diffuse reflections a path may make |
| `seed` | 0.. | Random seed; the same seed bakes the same bytes |
| `atlas_size` | 64..16384 | The largest atlas allowed; each atlas shrinks to the power of two that holds its charts (`width`/`height` on its resource) |
| `probe_rays` | 16..65536 | Rays per probe (optional, 256) |
| `lighting_hash` | 16 hex | Static lights, environment and settings. Different: the whole bake is stale and unused |
| `geometry_hash` | 16 hex | Every static occluder. Different: stale, but receivers whose own hash matches are still used |
| `resource <kind> "path" hash H bytes N [width W height H]` | | A generated file and its content hash; kinds `lightmap` (in atlas order), `receivers`, `probes` |
| `receiver <id> part P atlas A hash H` | | A lightmapped surface: its binding id, model part (0 for a MeshRenderer), atlas, and own dependency hash |
| `volume <id> hash H` | | A baked DiffuseProbeVolume and its dependency hash |

## Lightmap: `Lightmap_NNN.ktx2`

Standard KTX2: one level of `VK_FORMAT_R16G16B16A16_SFLOAT`, no supercompression, a
basic data format descriptor (RGBSDA, BT.709, linear). RGB is B; alpha is 1 where a
chart covers the texel (gutters are filled from their charts) and 0 in unused space.
The renderer samples level 0 only, so charts never blend in smaller mip levels.

## Receivers: `Receivers.swbake`

Little-endian, no padding.

| Field | Type |
|---|---|
| magic | 8 bytes `SWBAKE\0\0` |
| version | u32, 1 |
| receiver count | u32 |
| per receiver | u64 binding hi, u64 binding lo, u32 part, u32 atlas, u64 geometry hash, u32 vertex count, u32 index count, then per vertex u32 source vertex + f32 u + f32 v, then u32 indices |

A receiver's mesh is its source mesh re-indexed: vertex i copies source vertex
`source_vertex[i]` and gets lightmap UV i (0..1 in its atlas; texel centres at
(i + 0.5) / size). Indices must be whole triangles and in range.

## Probes: `Probes.swprobe`

Little-endian, no padding.

| Field | Type |
|---|---|
| magic | 8 bytes `SWPROBE\0` |
| version | u32, 1 |
| environment | 27 f32: the surroundings as irradiance / pi SH, coefficient-major (rgb) |
| volume count | u32 |
| per volume | u64 hi, u64 lo, f32 toWorld[16] column-major, f32 half extents[3], u32 counts[3], f32 blend distance, i32 priority, f32 max distance, then per probe (x fastest, then y, then z) u32 valid (0 or 1), f32 sh[27], f32 distance[128] |

Spherical harmonics: nine coefficients per channel in the order Y00, Y1-1 (y), Y10 (z),
Y11 (x), Y2-2 (xy), Y2-1 (yz), Y20 (3z^2 - 1), Y21 (xz), Y22 (x^2 - y^2), with
normalisation 0.282095, 0.488603, 1.092548, 0.315392 and 0.546274. They're coefficients
of irradiance / pi: radiance projections scaled by 1, 2/3 and 1/4 per band. A constant
radiance c has only coefficient 0, c * 4 pi * 0.282095 (then times 1), and evaluates to
c at every normal. Negative results are clamped to zero when evaluated, never stored.

The distance map is 8 x 8 octahedral texels (y up at the centre), each the mean
distance in metres to the first hit and the mean of its square, from the probe's rays;
misses count as the volume's max distance. A point farther than the mean is weighted
by Chebyshev's bound, cubed, with a variance floor of (0.02 x spacing)^2.

## Inspect, export and import

`player --lighting-info [--scene file] [--json out]` prints the bake's state and why it's
out of date (JSON schema `shockwave.lighting-info.1`). `player --lighting-export <payload>
--output <out.json>` writes a .swbake, .swprobe or lightmap .ktx2 as JSON (every float
with 9 significant digits, so it round-trips exactly), and `player --lighting-import
<in.json> --output <payload>` encodes it again, by the output's extension. An imported
payload's hash differs from the manifest's until the scene is rebaked.
)";
}

}   // namespace shockwave
