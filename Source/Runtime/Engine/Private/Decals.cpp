#include <Engine/Decals.h>

#include <Assets/Assets.h>
#include <Engine/Hierarchy.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace shockwave {

namespace {

bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}   // namespace

DecalDesc
decalDesc(const Decal& decal, const glm::mat4& worldMatrix, const MaterialAsset* material, uint64_t order, float fade) {
    DecalDesc desc;
    desc.boxToWorld = glm::scale(worldMatrix, decal.size);
    desc.texture = material ? material->baseColorTexture : decal.texture;
    const glm::vec4 base = material ? material->baseColor : glm::vec4(1.0f);
    desc.color = glm::vec4(glm::vec3(base) * glm::vec3(decal.tint), base.a * decal.tint.a * decal.opacity * fade);
    desc.roughness = material ? material->draw.roughness : decal.roughness;
    desc.metallic = material ? material->draw.metallic : decal.metallic;
    desc.channels = (decal.albedo ? kDecalAlbedo : 0u) | (decal.roughnessMetallic ? kDecalRoughnessMetallic : 0u);
    desc.receiverMask = static_cast<uint32_t>(decal.receiverMask) & kDecalLayerMask;
    desc.angleFadeDegrees = decal.angleFade;
    desc.sortOrder = decal.sortOrder;
    desc.order = order;
    desc.essential = decal.essential;
    return desc;
}

uint64_t decalOrder(Entity e, const DecalLifetime* lifetime) {
    return lifetime ? (uint64_t(1) << 32) + lifetime->spawn : uint64_t(e.index);
}

float decalLifetimeFade(const DecalLifetime& lifetime) {
    if (!(lifetime.seconds > 0.0f)) return 1.0f;
    const float left = lifetime.seconds - lifetime.age;
    if (left <= 0.0f) return 0.0f;
    return lifetime.fade > 0.0f ? std::clamp(left / lifetime.fade, 0.0f, 1.0f) : 1.0f;
}

std::string decalProblem(const Decal& decal, const Assets* assets) {
    if (!finite(decal.size) || !(decal.size.x > 0.0f && decal.size.y > 0.0f && decal.size.z > 0.0f))
        return "size must be above zero on every axis";
    if (!std::isfinite(decal.opacity) || decal.opacity < 0.0f || decal.opacity > 1.0f)
        return "opacity must be between 0 and 1";
    if (!std::isfinite(decal.angleFade) || decal.angleFade < 0.0f || decal.angleFade > 89.0f)
        return "angle_fade must be between 0 and 89 degrees";
    if (!decal.albedo && !decal.roughnessMetallic) return "changes nothing: albedo and roughness_metallic are both off";
    if (decal.receiverMask < 0 || decal.receiverMask > 255) return "receiver_mask must be between 0 and 255";
    if (decal.material.valid() && assets) {
        const auto material = assets->material(decal.material);
        if (!material) return "its material isn't loaded";
        if (material->fallback) return "its material couldn't be resolved: " + material->error;
    }
    return {};
}

glm::quat impactDecalRotation(const glm::vec3& normal, uint32_t seed) {
    const float length = glm::length(normal);
    const glm::vec3 z = length > 0.0f ? normal / length : glm::vec3(0.0f, 0.0f, 1.0f);
    // Upright on walls; on floors and ceilings world +Y can't be "up", so -Z is.
    const glm::vec3 reference = std::abs(z.y) > 0.99f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 x = glm::normalize(glm::cross(reference, z));
    const glm::vec3 y = glm::cross(z, x);
    glm::quat q = glm::quat_cast(glm::mat3(x, y, z));
    if (seed != 0) {
        // A 32-bit integer hash (lowbias32) to an angle, so nearby seeds turn far apart.
        uint32_t h = seed;
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        const float angle = float(h) * (glm::two_pi<float>() / 4294967296.0f);
        q = q * glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
    }
    return glm::normalize(q);
}

Entity spawnImpactDecal(World& world, const ImpactDecal& impact, uint32_t maxImpactDecals) {
    if (!finite(impact.position) || !finite(impact.normal) || !(glm::length(impact.normal) > 1e-6f)) return kNullEntity;
    if (!decalProblem(impact.decal, nullptr).empty() || maxImpactDecals == 0) return kNullEntity;

    // Make room: the oldest marks go first, so the newest are always the ones shown.
    uint64_t next = 1;
    std::vector<std::pair<uint64_t, Entity>> marks;
    world.each<const DecalLifetime>([&](Entity e, const DecalLifetime& l) {
        marks.push_back({l.spawn, e});
        next = std::max(next, l.spawn + 1);
    });
    if (marks.size() >= maxImpactDecals) {
        std::sort(marks.begin(), marks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i + maxImpactDecals <= marks.size(); ++i) world.destroy(marks[i].second);
    }

    const Entity e = world.create();
    world.add<Transform>(e, {impact.position, impactDecalRotation(impact.normal, impact.seed), glm::vec3(1.0f)});
    world.add<Decal>(e, impact.decal);
    DecalLifetime lifetime;
    lifetime.seconds = std::max(impact.lifetime, 0.0f);
    lifetime.fade = std::max(impact.fade, 0.0f);
    lifetime.spawn = next;
    if (world.alive(impact.receiver) && world.has<Transform>(impact.receiver)) {
        // Stuck where it landed: its Transform becomes relative to the receiver.
        if (setParent(world, e, impact.receiver, true)) lifetime.receiver = impact.receiver;
    }
    world.add<DecalLifetime>(e, lifetime);
    return e;
}

void updateDecalLifetimes(World& world, float dt) {
    std::vector<Entity> gone;
    world.each<DecalLifetime>([&](Entity e, DecalLifetime& l) {
        l.age += dt;
        if ((l.seconds > 0.0f && l.age >= l.seconds) || (l.receiver.valid() && !world.alive(l.receiver)))
            gone.push_back(e);
    });
    for (Entity e : gone) world.destroy(e);
}

}   // namespace shockwave
