// Gradients baked into textures, and the CPU rasterizer behind box-shadow. See UIEffects.h.
#include "UIEffects.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace shockwave::ui {

namespace {

glm::vec4 unpack(uint32_t c) {
    return glm::vec4(float(c & 0xff), float((c >> 8) & 0xff), float((c >> 16) & 0xff), float(c >> 24)) / 255.0f;
}

uint8_t toByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

float smoothstep(float a, float b, float x) {
    if (b <= a) return x < a ? 0.0f : 1.0f;
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float edge(glm::vec2 a, glm::vec2 b, glm::vec2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

// An RGBA float image, premultiplied, plus the clip mask shared by every layer (like a
// stencil buffer).
struct Canvas {
    int x = 0, y = 0, width = 0, height = 0;
    std::vector<glm::vec4> pixels;
};

struct Rasterizer {
    int x0, y0, width, height;
    std::vector<uint8_t> mask;   // 1: drawing allowed
    bool maskEnabled = false;
    const ParallelFor* parallel = nullptr;

    // Runs fn(begin, end) over [0, count), split across worker threads when there are
    // enough rows to be worth it.
    template <class Fn>
    void rows(size_t count, Fn&& fn) const {
        if (parallel && *parallel && count >= 32)
            (*parallel)(count, fn);
        else
            fn(size_t(0), count);
    }

    // Calls fn(index, b0, b1, b2) for every pixel center inside the triangle, clipped to
    // the region and scissor. Each row works out its span first, so only covered pixels
    // are visited.
    template <class Fn>
    void triangle(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::ivec4 scissor, Fn&& fn) const {
        p0 -= glm::vec2(x0, y0);
        p1 -= glm::vec2(x0, y0);
        p2 -= glm::vec2(x0, y0);
        const float area = edge(p0, p1, p2);
        if (std::abs(area) < 1e-6f) return;
        int minX = std::max(0, int(std::floor(std::min({p0.x, p1.x, p2.x}))));
        int minY = std::max(0, int(std::floor(std::min({p0.y, p1.y, p2.y}))));
        int maxX = std::min(width - 1, int(std::ceil(std::max({p0.x, p1.x, p2.x}))));
        int maxY = std::min(height - 1, int(std::ceil(std::max({p0.y, p1.y, p2.y}))));
        if (scissor.z >= 0) {
            minX = std::max(minX, scissor.x - x0);
            minY = std::max(minY, scissor.y - y0);
            maxX = std::min(maxX, scissor.x + scissor.z - x0 - 1);
            maxY = std::min(maxY, scissor.y + scissor.w - y0 - 1);
        }
        // Barycentric weight k at (x, y) is a[k] * x + b[k] * y + c[k]; all three are
        // positive inside, whatever the winding.
        const float inv = 1.0f / area;
        const glm::vec2* q[3][2] = {{&p1, &p2}, {&p2, &p0}, {&p0, &p1}};
        float a[3], b[3], c[3];
        for (int k = 0; k < 3; ++k) {
            const glm::vec2 s = *q[k][0], e = *q[k][1];
            a[k] = -(e.y - s.y) * inv;
            b[k] = (e.x - s.x) * inv;
            c[k] = ((e.y - s.y) * s.x - (e.x - s.x) * s.y) * inv;
        }
        if (maxY < minY) return;
        rows(size_t(maxY - minY + 1), [&](size_t first, size_t last) {
            for (int py = minY + int(first); py < minY + int(last); ++py) {
                const float cy = float(py) + 0.5f;
                float lo = float(minX) + 0.5f, hi = float(maxX) + 0.5f;
                bool empty = false;
                for (int k = 0; k < 3 && !empty; ++k) {
                    const float rest = b[k] * cy + c[k];
                    if (a[k] > 0.0f)
                        lo = std::max(lo, -rest / a[k]);
                    else if (a[k] < 0.0f)
                        hi = std::min(hi, -rest / a[k]);
                    else if (rest < 0.0f)
                        empty = true;
                }
                if (empty || hi < lo) continue;
                const int xs = std::max(minX, int(std::ceil(lo - 0.5f))),
                          xe = std::min(maxX, int(std::floor(hi - 0.5f)));
                for (int px = xs; px <= xe; ++px) {
                    const float cx = float(px) + 0.5f;
                    const float b0 = a[0] * cx + b[0] * cy + c[0], b1 = a[1] * cx + b[1] * cy + c[1];
                    const float b2 = 1.0f - b0 - b1;
                    if (b0 < -1e-5f || b1 < -1e-5f || b2 < -1e-5f) continue;   // rounding at the span's ends
                    fn(size_t(py) * size_t(width) + size_t(px), b0, b1, b2);
                }
            }
        });
    }

    bool allowed(size_t i) const { return !maskEnabled || mask[i]; }

    void draw(Canvas& canvas, const LayerOp& op) const {
        if (op.texture) return;   // textured drawing isn't needed for shadows
        for (size_t t = 0; t + 2 < op.indices.size(); t += 3) {
            const uint32_t i0 = op.indices[t], i1 = op.indices[t + 1], i2 = op.indices[t + 2];
            if (i0 >= op.vertices.size() || i1 >= op.vertices.size() || i2 >= op.vertices.size()) continue;
            const UiVertex* v[3] = {&op.vertices[i0], &op.vertices[i1], &op.vertices[i2]};
            const glm::vec4 c[3] = {unpack(v[0]->color), unpack(v[1]->color), unpack(v[2]->color)};
            const bool gradient = op.gradient && op.gradientPoints.size() == op.vertices.size();
            glm::vec2 g[3]{};
            if (gradient) g[0] = op.gradientPoints[i0], g[1] = op.gradientPoints[i1], g[2] = op.gradientPoints[i2];
            triangle(
                v[0]->position, v[1]->position, v[2]->position, op.scissor,
                [&](size_t i, float b0, float b1, float b2) {
                    if (!allowed(i)) return;
                    glm::vec4 src = c[0] * b0 + c[1] * b1 + c[2] * b2;
                    if (gradient) src *= op.gradient->color(op.gradient->param(g[0] * b0 + g[1] * b1 + g[2] * b2));
                    glm::vec4& dst = canvas.pixels[i];
                    dst = src + dst * (1.0f - src.a);
                });
        }
    }

    void clip(const LayerOp& op) {
        std::vector<uint8_t> cover(mask.size(), 0);
        for (size_t t = 0; t + 2 < op.indices.size(); t += 3) {
            const uint32_t i0 = op.indices[t], i1 = op.indices[t + 1], i2 = op.indices[t + 2];
            if (i0 >= op.vertices.size() || i1 >= op.vertices.size() || i2 >= op.vertices.size()) continue;
            triangle(
                op.vertices[i0].position, op.vertices[i1].position, op.vertices[i2].position, {0, 0, -1, -1},
                [&](size_t i, float, float, float) { cover[i] = 1; });
        }
        rows(mask.size(), [&](size_t first, size_t last) {
            for (size_t i = first; i < last; ++i) {
                switch (op.clip) {
                case LayerOp::ClipOp::Set: mask[i] = cover[i]; break;
                case LayerOp::ClipOp::SetInverse: mask[i] = !cover[i]; break;
                case LayerOp::ClipOp::Intersect: mask[i] = mask[i] && cover[i]; break;
                }
            }
        });
    }

    void run(Canvas& canvas, std::span<const LayerOp> ops) {
        for (const LayerOp& op : ops) {
            switch (op.kind) {
            case LayerOp::Kind::Draw: draw(canvas, op); break;
            case LayerOp::Kind::ClipMask: clip(op); break;
            case LayerOp::Kind::ClipEnable: maskEnabled = op.enable; break;
            case LayerOp::Kind::Composite: {
                Canvas layer{x0, y0, width, height, std::vector<glm::vec4>(canvas.pixels.size(), glm::vec4(0.0f))};
                run(layer, op.children);
                for (const LayerFilter& f : op.filters) applyFilter(layer, f);
                rows(layer.pixels.size(), [&](size_t first, size_t last) {
                    for (size_t i = first; i < last; ++i) {
                        if (!allowed(i)) continue;
                        const glm::vec4 src = layer.pixels[i];
                        canvas.pixels[i] = op.replace ? src : src + canvas.pixels[i] * (1.0f - src.a);
                    }
                });
                break;
            }
            }
        }
    }

    // A Gaussian blur as three box blurs, which costs the same whatever the radius.
    void boxBlur(std::vector<glm::vec4>& data, int w, int h, int r, bool horizontal) const {
        const int length = horizontal ? w : h, lines = horizontal ? h : w;
        const float scale = 1.0f / float(2 * r + 1);
        rows(size_t(lines), [&](size_t first, size_t last) {
            std::vector<glm::vec4> scratch(static_cast<size_t>(length));
            for (int line = int(first); line < int(last); ++line) {
                auto at = [&](int k) -> glm::vec4& {
                    return horizontal ? data[size_t(line) * size_t(w) + size_t(k)]
                                      : data[size_t(k) * size_t(w) + size_t(line)];
                };
                for (int k = 0; k < length; ++k) scratch[size_t(k)] = at(k);
                glm::vec4 sum(0.0f);
                for (int k = -r; k <= r; ++k)
                    if (k >= 0 && k < length) sum += scratch[size_t(k)];
                for (int k = 0; k < length; ++k) {
                    at(k) = sum * scale;
                    const int add = k + r + 1, remove = k - r;
                    if (add < length) sum += scratch[size_t(add)];
                    if (remove >= 0) sum -= scratch[size_t(remove)];
                }
            }
        });
    }

    void applyFilter(Canvas& layer, const LayerFilter& filter) const {
        if (filter.kind == LayerFilter::Kind::Opacity) {
            for (glm::vec4& p : layer.pixels) p *= std::clamp(filter.value, 0.0f, 1.0f);
            return;
        }
        if (filter.value < 0.25f) return;
        // A wide blur loses nothing at lower resolution: blur a copy shrunk by up to 4x,
        // then scale it back up. This keeps a large soft shadow to a few milliseconds.
        const int factor = std::clamp(int(filter.value / 3.0f), 1, 4);
        if (factor == 1) {
            gaussian(layer.pixels, width, height, filter.value);
            return;
        }
        const int w = (width + factor - 1) / factor, h = (height + factor - 1) / factor;
        std::vector<glm::vec4> small(size_t(w) * size_t(h), glm::vec4(0.0f));
        const float share = 1.0f / float(factor * factor);
        rows(size_t(h), [&](size_t first, size_t last) {
            for (int sy = int(first); sy < int(last); ++sy)
                for (int y = sy * factor; y < std::min(height, (sy + 1) * factor); ++y)
                    for (int x = 0; x < width; ++x)
                        small[size_t(sy) * size_t(w) + size_t(x / factor)] +=
                            layer.pixels[size_t(y) * size_t(width) + size_t(x)] * share;
        });
        gaussian(small, w, h, filter.value / float(factor));
        rows(size_t(height), [&](size_t first, size_t last) {
            for (int y = int(first); y < int(last); ++y) {
                const float sy = std::clamp((float(y) + 0.5f) / float(factor) - 0.5f, 0.0f, float(h - 1));
                const int y0 = int(sy), y1 = std::min(y0 + 1, h - 1);
                const float fy = sy - float(y0);
                for (int x = 0; x < width; ++x) {
                    const float sx = std::clamp((float(x) + 0.5f) / float(factor) - 0.5f, 0.0f, float(w - 1));
                    const int x0 = int(sx), x1 = std::min(x0 + 1, w - 1);
                    const float fx = sx - float(x0);
                    const auto at = [&](int px, int py) { return small[size_t(py) * size_t(w) + size_t(px)]; };
                    layer.pixels[size_t(y) * size_t(width) + size_t(x)] =
                        glm::mix(glm::mix(at(x0, y0), at(x1, y0), fx), glm::mix(at(x0, y1), at(x1, y1), fx), fy);
                }
            }
        });
    }

    // A Gaussian blur of the given sigma as three box blurs (Kutskir's "boxes for Gauss").
    void gaussian(std::vector<glm::vec4>& data, int w, int h, float sigma) const {
        if (sigma < 0.25f) return;
        const float ideal = std::sqrt(12.0f * sigma * sigma / 3.0f + 1.0f);
        int lower = int(std::floor(ideal));
        if (lower % 2 == 0) --lower;
        const int upper = lower + 2;
        const float m = std::round(
            (12.0f * sigma * sigma - 3.0f * float(lower * lower) - 12.0f * float(lower) - 9.0f) /
            (-4.0f * float(lower) - 4.0f));
        for (int pass = 0; pass < 3; ++pass) {
            const int size = float(pass) < m ? lower : upper;
            const int r = std::max(0, (size - 1) / 2);
            if (r == 0) continue;
            boxBlur(data, w, h, r, true);
            boxBlur(data, w, h, r, false);
        }
    }
};

}   // namespace

float Gradient::param(glm::vec2 point) const {
    float t = 0.0f;
    const glm::vec2 d = point - p;
    switch (kind) {
    case Kind::Linear: {
        const float len2 = glm::dot(v, v);
        t = len2 > 0.0f ? glm::dot(v, d) / len2 : 0.0f;
        break;
    }
    case Kind::Radial: t = glm::length(v * d); break;
    case Kind::Conic: {
        // As RmlUi's reference renderer: rotate by the start angle, 0 at the top, clockwise.
        const glm::vec2 r(v.x * d.x + v.y * d.y, -v.y * d.x + v.x * d.y);
        t = 0.5f + std::atan2(-r.x, r.y) / (2.0f * std::numbers::pi_v<float>);
        break;
    }
    }
    if (repeating && positions.size() >= 2) {
        const float t0 = positions.front(), t1 = positions.back();
        if (t1 > t0) t = t0 + (t - t0) - (t1 - t0) * std::floor((t - t0) / (t1 - t0));
    }
    return t;
}

glm::vec4 Gradient::color(float t) const {
    if (colors.empty()) return glm::vec4(0.0f);
    glm::vec4 c = colors[0];
    for (size_t i = 1; i < colors.size() && i < positions.size(); ++i)
        c = glm::mix(c, colors[i], smoothstep(positions[i - 1], positions[i], t));
    return c;
}

BakedGradient bakeGradient(const Gradient& gradient, std::span<const glm::vec2> points) {
    BakedGradient out;
    if (points.empty()) return out;
    // Texel centers sit exactly on the geometry's extent, so the sampler (which repeats)
    // never blends in the opposite edge.
    if (gradient.kind == Gradient::Kind::Linear) {
        // Unrepeated t at each vertex; t is linear in position, so it interpolates exactly.
        std::vector<float> ts(points.size());
        for (size_t i = 0; i < points.size(); ++i)
            ts[i] = glm::dot(gradient.v, points[i] - gradient.p) / std::max(glm::dot(gradient.v, gradient.v), 1e-12f);
        const float lo = *std::min_element(ts.begin(), ts.end()), hi = *std::max_element(ts.begin(), ts.end());
        const float pixels = (hi - lo) * glm::length(gradient.v);
        out.width = static_cast<uint32_t>(std::clamp(std::ceil(pixels) + 1.0f, 2.0f, 1024.0f));
        out.height = 1;
        out.rgba.resize(size_t(out.width) * 4);
        for (uint32_t i = 0; i < out.width; ++i) {
            float t = lo + (hi - lo) * float(i) / float(out.width - 1);
            if (gradient.repeating && gradient.positions.size() >= 2) {
                const float t0 = gradient.positions.front(), t1 = gradient.positions.back();
                if (t1 > t0) t = t0 + (t - t0) - (t1 - t0) * std::floor((t - t0) / (t1 - t0));
            }
            const glm::vec4 c = gradient.color(t);
            for (int k = 0; k < 4; ++k) out.rgba[i * 4 + uint32_t(k)] = toByte(c[k]);
        }
        out.uvs.resize(points.size());
        for (size_t i = 0; i < points.size(); ++i) {
            const float f = hi > lo ? (ts[i] - lo) / (hi - lo) : 0.0f;
            out.uvs[i] = glm::vec2((0.5f + f * float(out.width - 1)) / float(out.width), 0.5f);
        }
        return out;
    }
    glm::vec2 lo = points[0], hi = points[0];
    for (glm::vec2 p : points) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec2 extent = glm::max(hi - lo, glm::vec2(1e-3f));
    // Radial and conic gradients are smooth, so a quarter-megapixel bake is plenty.
    out.width = static_cast<uint32_t>(std::clamp(std::ceil(extent.x) + 1.0f, 2.0f, 256.0f));
    out.height = static_cast<uint32_t>(std::clamp(std::ceil(extent.y) + 1.0f, 2.0f, 256.0f));
    out.rgba.resize(size_t(out.width) * out.height * 4);
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            const glm::vec2 p =
                lo + extent * glm::vec2(float(x) / float(out.width - 1), float(y) / float(out.height - 1));
            const glm::vec4 c = gradient.color(gradient.param(p));
            const size_t at = (size_t(y) * out.width + x) * 4;
            for (int k = 0; k < 4; ++k) out.rgba[at + size_t(k)] = toByte(c[k]);
        }
    }
    out.uvs.resize(points.size());
    const glm::vec2 size(float(out.width), float(out.height));
    for (size_t i = 0; i < points.size(); ++i)
        out.uvs[i] = (glm::vec2(0.5f) + (points[i] - lo) / extent * (size - 1.0f)) / size;
    return out;
}

std::vector<uint8_t> rasterizeLayer(std::span<const LayerOp> ops, glm::ivec4 region, const ParallelFor& parallel) {
    const int w = std::max(region.z, 0), h = std::max(region.w, 0);
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4, 0);
    if (!w || !h) return out;
    Rasterizer r{region.x, region.y, w, h, std::vector<uint8_t>(size_t(w) * size_t(h), 1), false, &parallel};
    Canvas canvas{region.x, region.y, w, h, std::vector<glm::vec4>(size_t(w) * size_t(h), glm::vec4(0.0f))};
    r.run(canvas, ops);
    r.rows(canvas.pixels.size(), [&](size_t first, size_t last) {
        for (size_t i = first; i < last; ++i)
            for (int k = 0; k < 4; ++k) out[i * 4 + size_t(k)] = toByte(canvas.pixels[i][k]);
    });
    return out;
}

}   // namespace shockwave::ui
