#include <Engine/Components.h>

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
    registerComponent<Name>("Name", {field("value", &Name::value)}, "Core");
    registerComponent<Transform>("Transform", {
        field("position", &Transform::position, 0.05f),
        field("rotation", &Transform::rotation, 0.5f),
        field("scale", &Transform::scale, 0.02f, 0.001f, 1000.0f),
    }, "Core");
    registerComponent<Parent>("Parent", {field("entity", &Parent::entity)}, "Core");
    registerComponent<MeshRenderer>("MeshRenderer", {
        field("mesh", &MeshRenderer::mesh),
        field("texture", &MeshRenderer::texture),
        field("tint", &MeshRenderer::tint),
        field("tiling", &MeshRenderer::tiling, 0.05f, 0.001f, 10000.0f),
        field("visible", &MeshRenderer::visible),
        field("roughness", &MeshRenderer::roughness, 0.01f, 0.0f, 1.0f),
        field("metallic", &MeshRenderer::metallic, 0.01f, 0.0f, 1.0f),
        field("emissive", &MeshRenderer::emissive),
        field("emissive_intensity", &MeshRenderer::emissiveIntensity, 0.05f, 0.0f, 1000.0f),
    }, "Rendering");
    registerComponent<ModelRenderer>("ModelRenderer", {
        field("model", &ModelRenderer::model), field("tint", &ModelRenderer::tint), field("visible", &ModelRenderer::visible),
    }, "Rendering");
    registerComponent<Camera>("Camera", {
        field("fov", &Camera::fov, 0.5f, 1.0f, 170.0f),
        field("near_plane", &Camera::nearPlane, 0.005f, 0.001f, 100.0f),
        field("primary", &Camera::primary),
    }, "Rendering");
    registerComponent<Collider>("Collider", {
        enumField("shape", &Collider::shape, {"Box", "Sphere", "Capsule", "Convex model", "Triangle model"}),
        field("half_extents", &Collider::halfExtents, 0.02f, 0.001f, 10000.0f),
        field("radius", &Collider::radius, 0.02f, 0.001f, 10000.0f),
        field("half_height", &Collider::halfHeight, 0.02f, 0.001f, 10000.0f),
        field("offset", &Collider::offset, 0.02f), field("enabled", &Collider::enabled),
    }, "Physics");
    registerComponent<RigidBody>("RigidBody", {
        enumField("motion", &RigidBody::motion, {"Static", "Dynamic", "Kinematic"}),
        field("mass", &RigidBody::mass, 0.1f, 0.001f, 100000.0f),
        field("friction", &RigidBody::friction, 0.01f, 0.0f, 10.0f),
        field("restitution", &RigidBody::restitution, 0.01f, 0.0f, 1.0f),
        field("linear_damping", &RigidBody::linearDamping, 0.01f, 0.0f, 1.0f),
        field("angular_damping", &RigidBody::angularDamping, 0.01f, 0.0f, 1.0f),
        field("gravity_factor", &RigidBody::gravityFactor, 0.01f, 0.0f, 10.0f),
        field("linear_velocity", &RigidBody::linearVelocity, 0.1f), field("angular_velocity", &RigidBody::angularVelocity, 0.1f),
        field("collision_layer", &RigidBody::collisionLayer, 1.0f, 0.0f, 15.0f),
        field("collision_mask", &RigidBody::collisionMask, 1.0f, 0.0f, 65535.0f),
        field("sensor", &RigidBody::sensor), field("continuous", &RigidBody::continuous), field("enabled", &RigidBody::enabled),
    }, "Physics");
    registerComponent<PhysicsSettings>("PhysicsSettings", {
        field("gravity", &PhysicsSettings::gravity, 0.1f), field("collision_steps", &PhysicsSettings::collisionSteps, 1.0f, 1.0f, 8.0f),
    }, "Physics");
    registerComponent<CharacterController>("CharacterController", {
        field("radius", &CharacterController::radius, 0.02f, 0.01f, 10.0f),
        field("half_height", &CharacterController::halfHeight, 0.02f, 0.01f, 10.0f),
        field("mass", &CharacterController::mass, 0.1f, 0.01f, 10000.0f),
        field("max_slope", &CharacterController::maxSlope, 0.5f, 0.0f, 89.0f),
        field("step_height", &CharacterController::stepHeight, 0.02f, 0.0f, 2.0f),
        field("jump_speed", &CharacterController::jumpSpeed, 0.1f, 0.0f, 100.0f),
        field("max_push_force", &CharacterController::maxPushForce, 1.0f, 0.0f, 100000.0f),
        field("collision_layer", &CharacterController::collisionLayer, 1.0f, 0.0f, 15.0f),
        field("collision_mask", &CharacterController::collisionMask, 1.0f, 0.0f, 65535.0f),
        field("enabled", &CharacterController::enabled),
    }, "Physics");
    registerComponent<Environment>("Environment", {
        field("sky_color", &Environment::skyColor),
        field("ground_color", &Environment::groundColor),
        field("ambient_intensity", &Environment::ambientIntensity, 0.01f, 0.0f, 100.0f),
        field("exposure", &Environment::exposure, 0.05f, -16.0f, 16.0f),
        enumField("tonemapper", &Environment::tonemapper, {"ACES", "AgX", "Reinhard", "None"}),
    }, "Rendering");
    registerComponent<Light>("Light", {
        enumField("type", &Light::type, {"Directional", "Point", "Spot"}),
        field("color", &Light::color),
        field("intensity", &Light::intensity, 0.05f, 0.0f, 100000.0f),
        field("range", &Light::range, 0.1f, 0.01f, 100000.0f),
        field("spot_angle", &Light::spotAngle, 0.5f, 1.0f, 89.0f),
        field("spot_softness", &Light::spotSoftness, 0.01f, 0.0f, 1.0f),
        field("enabled", &Light::enabled),
    }, "Rendering");
    registerComponent<Spin>("Spin", {
        field("axis", &Spin::axis, 0.01f),
        field("speed", &Spin::speed, 1.0f),
    }, "Motion");
    registerComponent<Bob>("Bob", {
        field("amplitude", &Bob::amplitude, 0.01f, 0.0f, 100.0f),
        field("frequency", &Bob::frequency, 0.01f, 0.0f, 100.0f),
        field("phase", &Bob::phase, 0.01f),
    }, "Motion");
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
