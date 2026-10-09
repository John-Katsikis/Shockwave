#include <shockwave/Components.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace shockwave {

glm::mat4 Transform::matrix() const {
    glm::mat4 m = glm::mat4_cast(rotation);
    m[0] *= scale.x;
    m[1] *= scale.y;
    m[2] *= scale.z;
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

Transform Transform::fromMatrix(const glm::mat4& m) {
    Transform t;
    t.position = glm::vec3(m[3]);
    t.scale = {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
    glm::mat3 r(1.0f);
    for (int i = 0; i < 3; ++i) {
        if (t.scale[i] > 1e-8f) r[i] = glm::vec3(m[i]) / t.scale[i];
    }
    t.rotation = glm::normalize(glm::quat_cast(r));
    return t;
}

Transform Transform::blend(const Transform& a, const Transform& b, float t) {
    return Transform{glm::mix(a.position, b.position, t), glm::slerp(a.rotation, b.rotation, t),
                     glm::mix(a.scale, b.scale, t)};
}

// glm's Euler convention: q = qz * qy * qx, so X is applied first, then Y, then Z.
glm::vec3 eulerDegrees(const glm::quat& q) {
    glm::vec3 d = glm::degrees(glm::eulerAngles(q));
    // Every rotation has two angle sets: (x, y, z) and (x+180, 180-y, z+180). glm keeps Y
    // within +-90, which turns a plain 120 degree turn about Y into (180, 60, 180). Pick
    // whichever set is smaller, which is the one people would type.
    auto wrap = [](float a) { return a > 180.0f ? a - 360.0f : (a <= -180.0f ? a + 360.0f : a); };
    const glm::vec3 alt(wrap(d.x + 180.0f), wrap(180.0f - d.y), wrap(d.z + 180.0f));
    if (std::abs(alt.x) + std::abs(alt.y) + std::abs(alt.z) + 1e-3f < std::abs(d.x) + std::abs(d.y) + std::abs(d.z)) {
        d = alt;
    }
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-4f) d[i] = 0.0f;   // no "-0" or 1e-7 noise in files
    }
    return d;
}

glm::quat fromEulerDegrees(const glm::vec3& degrees) {
    return glm::quat(glm::radians(degrees));
}

namespace {

std::vector<ComponentInfo>& registry() {
    static std::vector<ComponentInfo> components;
    return components;
}

// Startup-time, main thread (see registerComponent), so a plain flag will do. It's set
// before registering because registering calls back in here.
void registerBuiltins() {
    static bool started = false;
    if (started) return;
    started = true;
    registerComponent<Name>("Name", {field("value", &Name::value)});
    registerComponent<Transform>("Transform", {
        field("position", &Transform::position, 0.05f),
        field("rotation", &Transform::rotation, 0.5f),
        field("scale", &Transform::scale, 0.02f, 0.001f, 1000.0f),
    });
    registerComponent<Parent>("Parent", {field("entity", &Parent::entity)});
    registerComponent<MeshRenderer>("MeshRenderer", {
        field("mesh", &MeshRenderer::mesh),
        field("texture", &MeshRenderer::texture),
        field("tint", &MeshRenderer::tint),
        field("visible", &MeshRenderer::visible),
    });
    registerComponent<Environment>("Environment", {field("sky_color", &Environment::skyColor)});
    registerComponent<Spin>("Spin", {
        field("axis", &Spin::axis, 0.01f),
        field("speed", &Spin::speed, 1.0f),
    });
    registerComponent<Bob>("Bob", {
        field("amplitude", &Bob::amplitude, 0.01f, 0.0f, 100.0f),
        field("frequency", &Bob::frequency, 0.01f, 0.0f, 100.0f),
        field("phase", &Bob::phase, 0.01f),
    });
}

} // namespace

namespace detail {
void addComponentInfo(ComponentInfo info) {
    registerBuiltins();   // built-ins always come first
    for (ComponentInfo& existing : registry()) {
        if (existing.name == info.name) {
            existing = std::move(info);
            return;
        }
    }
    registry().push_back(std::move(info));
}
} // namespace detail

const std::vector<ComponentInfo>& registeredComponents() {
    registerBuiltins();
    return registry();
}

const ComponentInfo* findComponent(std::string_view name) {
    for (const ComponentInfo& info : registeredComponents()) {
        if (info.name == name) return &info;
    }
    return nullptr;
}

} // namespace shockwave
