#include "BakeTracer.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace shockwave::bake {

glm::vec3 cosineDirection(const glm::vec3& n, float u1, float u2) {
    // Malley's method: uniform on the disk, projected up onto the hemisphere.
    const float r = std::sqrt(u1);
    const float phi = glm::two_pi<float>() * u2;
    const glm::vec3 local(r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u1)));
    // Frisvad / Duff et al. orthonormal basis around n.
    const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    const float b = n.x * n.y * a;
    const glm::vec3 t(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
    const glm::vec3 bt(b, sign + n.y * n.y * a, -n.y);
    return glm::normalize(t * local.x + bt * local.y + n * local.z);
}

glm::vec3 sphereDirection(uint32_t i, uint32_t count) {
    const float golden = glm::pi<float>() * (3.0f - std::sqrt(5.0f));
    const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
    const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float phi = golden * float(i);
    return {r * std::cos(phi), y, r * std::sin(phi)};
}

float radicalInverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

glm::vec3 BakeScene::offset(const glm::vec3& position, const glm::vec3& normal) {
    const float scale = std::max({1.0f, std::abs(position.x), std::abs(position.y), std::abs(position.z)});
    return position + normal * (2e-4f * scale);
}

BakeScene::BakeScene(const BakeSceneDescription& description) : m_description(description) {
    for (size_t s = 0; s < description.surfaces.size(); ++s) {
        const BakeSurface& surface = description.surfaces[s];
        const glm::mat3 linear(surface.toWorld);
        m_normalMatrices.push_back(glm::transpose(glm::inverse(linear)));
        m_handedness.push_back(glm::determinant(linear) < 0.0f ? -1.0f : 1.0f);
        if (!surface.occluder) continue;
        const MeshData& mesh = *surface.mesh;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
            if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size()) continue;
            const glm::vec3 p0 = glm::vec3(surface.toWorld * glm::vec4(mesh.vertices[a].position, 1.0f));
            const glm::vec3 p1 = glm::vec3(surface.toWorld * glm::vec4(mesh.vertices[b].position, 1.0f));
            const glm::vec3 p2 = glm::vec3(surface.toWorld * glm::vec4(mesh.vertices[c].position, 1.0f));
            const glm::vec3 e1 = p1 - p0, e2 = p2 - p0;
            const float area = glm::length(glm::cross(e1, e2));
            if (!std::isfinite(area) || area < 1e-12f) continue;   // degenerate: never hit anyway
            m_triangles.push_back({p0, e1, e2, uint32_t(s), uint32_t(i)});
        }
    }
    build();
}

void BakeScene::build() {
    m_nodes.clear();
    if (m_triangles.empty()) return;
    std::vector<glm::vec3> centres(m_triangles.size());
    for (size_t i = 0; i < m_triangles.size(); ++i) {
        const Triangle& t = m_triangles[i];
        centres[i] = t.v0 + (t.e1 + t.e2) / 3.0f;
    }
    auto bounds = [&](uint32_t start, uint32_t count, glm::vec3& lo, glm::vec3& hi) {
        lo = glm::vec3(std::numeric_limits<float>::max());
        hi = -lo;
        for (uint32_t i = start; i < start + count; ++i) {
            const Triangle& t = m_triangles[i];
            for (const glm::vec3& p : {t.v0, t.v0 + t.e1, t.v0 + t.e2}) {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        }
    };
    auto area = [](const glm::vec3& lo, const glm::vec3& hi) {
        const glm::vec3 d = glm::max(hi - lo, glm::vec3(0.0f));
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    };
    m_nodes.reserve(m_triangles.size() * 2);
    m_nodes.push_back({});
    m_nodes[0].start = 0;
    m_nodes[0].count = uint32_t(m_triangles.size());
    std::vector<uint32_t> stack{0};
    constexpr int kBins = 16;
    while (!stack.empty()) {
        const uint32_t index = stack.back();
        stack.pop_back();
        Node node = m_nodes[index];
        bounds(node.start, node.count, node.min, node.max);
        m_nodes[index].min = node.min;
        m_nodes[index].max = node.max;
        if (node.count <= 4) continue;
        glm::vec3 cmin(std::numeric_limits<float>::max()), cmax(-std::numeric_limits<float>::max());
        for (uint32_t i = node.start; i < node.start + node.count; ++i) {
            cmin = glm::min(cmin, centres[i]);
            cmax = glm::max(cmax, centres[i]);
        }
        // Binned SAH over the widest centroid axes; ties go to the lowest axis and bin, so
        // the tree is the same on every machine.
        float bestCost = std::numeric_limits<float>::max();
        int bestAxis = -1, bestSplit = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const float extent = cmax[axis] - cmin[axis];
            if (extent < 1e-9f) continue;
            struct Bin {
                glm::vec3 lo{std::numeric_limits<float>::max()}, hi{-std::numeric_limits<float>::max()};
                uint32_t count = 0;
            } bins[kBins];
            for (uint32_t i = node.start; i < node.start + node.count; ++i) {
                const int b = std::min(kBins - 1, int((centres[i][axis] - cmin[axis]) / extent * kBins));
                const Triangle& t = m_triangles[i];
                for (const glm::vec3& p : {t.v0, t.v0 + t.e1, t.v0 + t.e2}) {
                    bins[b].lo = glm::min(bins[b].lo, p);
                    bins[b].hi = glm::max(bins[b].hi, p);
                }
                ++bins[b].count;
            }
            for (int split = 1; split < kBins; ++split) {
                glm::vec3 llo(std::numeric_limits<float>::max()), lhi(-std::numeric_limits<float>::max());
                glm::vec3 rlo = llo, rhi = lhi;
                uint32_t lc = 0, rc = 0;
                for (int b = 0; b < split; ++b)
                    if (bins[b].count)
                        llo = glm::min(llo, bins[b].lo), lhi = glm::max(lhi, bins[b].hi), lc += bins[b].count;
                for (int b = split; b < kBins; ++b)
                    if (bins[b].count)
                        rlo = glm::min(rlo, bins[b].lo), rhi = glm::max(rhi, bins[b].hi), rc += bins[b].count;
                if (!lc || !rc) continue;
                const float cost = area(llo, lhi) * float(lc) + area(rlo, rhi) * float(rc);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = split;
                }
            }
        }
        if (bestAxis < 0 || bestCost >= area(node.min, node.max) * float(node.count)) continue;   // a leaf is cheaper
        const float extent = cmax[bestAxis] - cmin[bestAxis];
        uint32_t mid = node.start;
        for (uint32_t i = node.start; i < node.start + node.count; ++i) {
            const int b = std::min(kBins - 1, int((centres[i][bestAxis] - cmin[bestAxis]) / extent * kBins));
            if (b < bestSplit) {
                std::swap(m_triangles[i], m_triangles[mid]);
                std::swap(centres[i], centres[mid]);
                ++mid;
            }
        }
        const uint32_t left = uint32_t(m_nodes.size());
        m_nodes.push_back({{}, {}, node.start, mid - node.start});
        m_nodes.push_back({{}, {}, mid, node.start + node.count - mid});
        m_nodes[index].start = left;
        m_nodes[index].count = 0;
        stack.push_back(left + 1);
        stack.push_back(left);
    }
}

glm::vec4 BakeScene::baseColor(const BakeSurface& s, uint32_t first, float u, float v) const {
    const MeshData& mesh = *s.mesh;
    const uint32_t a = mesh.indices[first], b = mesh.indices[first + 1], c = mesh.indices[first + 2];
    const float w = 1.0f - u - v;
    glm::vec4 colour = mesh.vertices[a].color * w + mesh.vertices[b].color * u + mesh.vertices[c].color * v;
    const Image* image = s.material.baseColor.get();
    if (!image || image->width == 0 || image->height == 0) return colour;
    const bool set1 = s.material.binding.uvSet == 1 && mesh.uv1.size() == mesh.vertices.size();
    auto uvOf = [&](uint32_t i) { return set1 ? mesh.uv1[i] : mesh.vertices[i].uv; };
    glm::vec2 uv = (uvOf(a) * w + uvOf(b) * u + uvOf(c) * v) * s.material.uvScale;
    uv = s.material.binding.transform.apply(uv);
    // Bilinear with repeat addressing, as the default sampler.
    const float x = uv.x * float(image->width) - 0.5f, y = uv.y * float(image->height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    glm::vec4 texel(0.0f);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int px = (int(fx) + i) % int(image->width), py = (int(fy) + j) % int(image->height);
            if (px < 0) px += int(image->width);
            if (py < 0) py += int(image->height);
            const uint8_t* p = &image->rgba[(size_t(py) * image->width + size_t(px)) * 4];
            const float weight = (i ? x - fx : 1.0f - (x - fx)) * (j ? y - fy : 1.0f - (y - fy));
            auto linear = [](uint8_t value) {
                const float c = value / 255.0f;
                return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            };
            texel += glm::vec4(linear(p[0]), linear(p[1]), linear(p[2]), p[3] / 255.0f) * weight;
        }
    return colour * texel;
}

bool BakeScene::opaque(const Triangle& tri, float u, float v) const {
    const BakeSurface& s = m_description.surfaces[tri.surface];
    if (s.material.alphaCutoff < 0.0f) return true;
    return baseColor(s, tri.first, u, v).a * s.material.alpha >= s.material.alphaCutoff;
}

template <class Accept>
bool BakeScene::traverse(
    const glm::vec3& origin, const glm::vec3& direction, float tMax, bool anyHit, Hit& hit, Accept&& accept) const {
    if (m_nodes.empty()) return false;
    const glm::vec3 inv(1.0f / direction.x, 1.0f / direction.y, 1.0f / direction.z);
    float best = tMax;
    bool found = false;
    uint32_t stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& node = m_nodes[stack[--top]];
        const glm::vec3 t0 = (node.min - origin) * inv, t1 = (node.max - origin) * inv;
        const glm::vec3 tn = glm::min(t0, t1), tf = glm::max(t0, t1);
        const float enter = std::max({tn.x, tn.y, tn.z, 0.0f});
        const float exit = std::min({tf.x, tf.y, tf.z, best});
        if (enter > exit) continue;
        if (node.count == 0) {
            if (top + 2 > 128) continue;
            stack[top++] = node.start + 1;
            stack[top++] = node.start;
            continue;
        }
        for (uint32_t i = node.start; i < node.start + node.count; ++i) {
            const Triangle& tri = m_triangles[i];
            const glm::vec3 p = glm::cross(direction, tri.e2);
            const float det = glm::dot(tri.e1, p);
            if (std::abs(det) < 1e-14f) continue;
            const float invDet = 1.0f / det;
            const glm::vec3 s = origin - tri.v0;
            const float u = glm::dot(s, p) * invDet;
            if (u < 0.0f || u > 1.0f) continue;
            const glm::vec3 q = glm::cross(s, tri.e1);
            const float v = glm::dot(direction, q) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float t = glm::dot(tri.e2, q) * invDet;
            if (t <= 0.0f || t >= best) continue;
            if (!accept(tri, u, v)) continue;
            best = t;
            hit = {t, i, u, v};
            found = true;
            if (anyHit) return true;
        }
    }
    return found;
}

bool BakeScene::intersect(const glm::vec3& origin, const glm::vec3& direction, float tMax, Hit& hit) const {
    return traverse(
        origin, direction, tMax, false, hit, [&](const Triangle& t, float u, float v) { return opaque(t, u, v); });
}

bool BakeScene::occluded(const glm::vec3& origin, const glm::vec3& direction, float tMax) const {
    Hit hit;
    return traverse(
        origin, direction, tMax, true, hit, [&](const Triangle& t, float u, float v) { return opaque(t, u, v); });
}

SurfacePoint BakeScene::surface(const Hit& hit, const glm::vec3& direction) const {
    const Triangle& tri = m_triangles[hit.triangle];
    const BakeSurface& s = m_description.surfaces[tri.surface];
    const MeshData& mesh = *s.mesh;
    const uint32_t a = mesh.indices[tri.first], b = mesh.indices[tri.first + 1], c = mesh.indices[tri.first + 2];
    const float w = 1.0f - hit.u - hit.v;
    SurfacePoint p;
    p.position = tri.v0 + tri.e1 * hit.u + tri.e2 * hit.v;
    // Counter-clockwise triangles face out; a mirroring transform reverses the winding.
    glm::vec3 geometric = glm::normalize(glm::cross(tri.e1, tri.e2)) * m_handedness[tri.surface];
    glm::vec3 normal = m_normalMatrices[tri.surface] * (mesh.vertices[a].normal * w + mesh.vertices[b].normal * hit.u +
                                                        mesh.vertices[c].normal * hit.v);
    normal = glm::length(normal) > 1e-8f ? glm::normalize(normal) : geometric;
    const bool front = glm::dot(geometric, direction) < 0.0f;
    if (!front) {
        p.backface = !s.material.doubleSided;
        geometric = -geometric;
        normal = -normal;
    }
    if (glm::dot(normal, geometric) <= 0.0f) normal = geometric;
    p.geometric = geometric;
    p.normal = normal;
    if (p.backface) return p;   // the inside of a closed object: no light comes from there
    const glm::vec4 colour = baseColor(s, tri.first, hit.u, hit.v);
    p.reflectance = s.material.reflectance(glm::vec3(colour));
    p.emissive = s.material.emissive;
    return p;
}

glm::vec3 BakeScene::direct(const glm::vec3& position, const glm::vec3& normal) const {
    glm::vec3 sum(0.0f);
    const glm::vec3 origin = offset(position, normal);
    for (const BakeLightSource& source : m_description.lights) {
        const LightDesc& l = source.light;
        glm::vec3 toLight;
        float distance = std::numeric_limits<float>::max();
        float attenuation = 1.0f;
        if (l.type == LightDesc::Type::Directional) {
            toLight = -l.direction;
        } else {
            const glm::vec3 offsetToLight = l.position - position;
            distance = glm::length(offsetToLight);
            if (distance >= l.range || distance < 1e-5f) continue;
            toLight = offsetToLight / distance;
            // Exactly the shader's falloff (mesh.frag rangeFalloff) and cone.
            const float ratio = distance / l.range;
            const float window = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
            attenuation = window * window / (distance * distance + 1.0f);
            if (l.type == LightDesc::Type::Spot) {
                const float cosAngle = glm::dot(-toLight, l.direction);
                const float t =
                    std::clamp((cosAngle - l.cosOuter) / std::max(l.cosInner - l.cosOuter, 1e-6f), 0.0f, 1.0f);
                attenuation *= t * t * (3.0f - 2.0f * t);
            }
        }
        const float nDotL = glm::dot(normal, toLight);
        if (nDotL <= 0.0f || attenuation <= 0.0f) continue;
        if (occluded(origin, toLight, distance)) continue;
        sum += l.color * (nDotL * attenuation);
    }
    return sum;
}

glm::vec3 BakeScene::incoming(
    const glm::vec3& origin, glm::vec3 direction, uint32_t bounces, Random& random, float* firstHit,
    bool* backface) const {
    glm::vec3 result(0.0f), throughput(1.0f);
    glm::vec3 from = origin;
    for (uint32_t depth = 0;; ++depth) {
        Hit hit;
        if (!intersect(from, direction, std::numeric_limits<float>::max(), hit)) {
            if (depth == 0 && firstHit) *firstHit = std::numeric_limits<float>::infinity();
            result += throughput * m_description.environment.radiance(direction);
            return result;
        }
        const SurfacePoint p = surface(hit, direction);
        if (depth == 0) {
            if (firstHit) *firstHit = hit.t;
            if (backface) *backface = p.backface;
        }
        if (p.backface) return result;
        result += throughput * p.emissive;
        if (depth >= bounces) return result;
        // The hit's own direct light is indirect light for the receiver.
        result += throughput * p.reflectance * direct(p.position, p.normal);
        throughput *= p.reflectance;
        if (glm::all(glm::lessThan(throughput, glm::vec3(1e-4f)))) return result;
        direction = cosineDirection(p.normal, random.next(), random.next());
        from = offset(p.position, p.geometric);
    }
}

}   // namespace shockwave::bake
