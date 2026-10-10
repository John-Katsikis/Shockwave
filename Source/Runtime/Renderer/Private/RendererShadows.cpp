// Shadow maps. The sun (the first shadow-casting directional light) gets cascaded shadow
// maps: the view is split by distance, each slice gets its own map in a 2 x 2 atlas, so
// nearby shadows stay sharp while distant ones still exist. Spot lights get one map and
// point lights six (one per cube face), packed as tiles in a second atlas, nearest light
// first while tiles last. Every map draws from the same draw table as the scene, culled
// to what can cast into it, and the mesh shader filters them with a rotated Poisson-style
// disk for soft edges.
#include "RendererImpl.h"

#include <Core/Profile.h>

#include <glm/gtc/matrix_transform.hpp>

namespace shockwave {

ShadowConfig detail::shadowConfig(ShadowQuality quality) {
    switch (quality) {
    case ShadowQuality::Off: return {};
    case ShadowQuality::Low: return {1024, 256, 2048, 4};
    case ShadowQuality::Medium: return {2048, 512, 4096, 8};
    case ShadowQuality::High: return {2048, 512, 4096, 16};
    case ShadowQuality::Ultra: return {4096, 1024, 4096, 32};
    }
    return {};
}

namespace {

// A view's six clip planes (Gribb & Hartmann), for testing bounding spheres. Works for any
// projection with 0..1 depth, reversed or not; a plane a projection doesn't have (the far
// plane of an infinite one) is left out.
struct Frustum {
    std::array<glm::vec4, 6> planes{};
    uint32_t count = 0;

    explicit Frustum(const glm::mat4& m) {
        auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
        const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        for (const glm::vec4& p : {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2}) {
            const float length = glm::length(glm::vec3(p));
            if (length > 1e-6f) planes[count++] = p / length;
        }
    }
    bool intersects(const glm::vec4& sphere) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (glm::dot(glm::vec3(planes[i]), glm::vec3(sphere)) + planes[i].w < -sphere.w) return false;
        }
        return true;
    }
};

// Any vector not parallel to dir, for lookAt.
glm::vec3 upFor(const glm::vec3& dir) {
    return std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
}

// Reverse-Z perspective with a far plane: near maps to 1, far to 0.
glm::mat4 perspectiveReverse(float fovY, float nearPlane, float farPlane) {
    return glm::perspectiveRH_ZO(fovY, 1.0f, farPlane, nearPlane);
}

// The cube faces in the order the mesh shader picks them: +X, -X, +Y, -Y, +Z, -Z.
constexpr std::array<glm::vec3, 6> kCubeFaces = {glm::vec3(1, 0, 0),  glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                                                 glm::vec3(0, -1, 0), glm::vec3(0, 0, 1),  glm::vec3(0, 0, -1)};

// Filter radius in texels for a light's softness, shared with the bias below so wide
// filters don't reach back into the surface.
float filterRadius(float softness) { return 0.75f + std::clamp(softness, 0.0f, 1.0f) * 5.25f; }

}   // namespace

void Renderer::Impl::createShadowMaps() {
    shadowSettings = shadowConfig(shadowQuality);
    if (shadowQuality == ShadowQuality::Off) return;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const uint32_t cascades = shadowSettings.cascadeSize * 2;
    cascadeAtlas = createImage({cascades, cascades}, kShadowFormat, usage, VK_IMAGE_ASPECT_DEPTH_BIT, 1, true);
    localAtlas = createImage(
        {shadowSettings.localAtlasSize, shadowSettings.localAtlasSize}, kShadowFormat, usage, VK_IMAGE_ASPECT_DEPTH_BIT,
        1, true);
    cascadeState = localState = {};

    std::lock_guard lock(descriptorMutex);
    VkDescriptorImageInfo infos[2]{};
    VkWriteDescriptorSet writes[2]{};
    const std::pair<uint32_t, VkImageView> slots[2] = {{cascadeSlot, cascadeAtlas.view}, {localSlot, localAtlas.view}};
    for (int i = 0; i < 2; ++i) {
        infos[i].imageView = slots[i].second;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = bindlessSet;
        writes[i].dstBinding = 0;
        writes[i].dstArrayElement = slots[i].first;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(context->device(), 2, writes, 0, nullptr);
}

void Renderer::Impl::destroyShadowMaps() {
    destroyImage(cascadeAtlas);
    destroyImage(localAtlas);
    cascadePasses.clear();
    localPasses.clear();
}

void Renderer::Impl::planShadows(
    const std::vector<LightDesc>& descs, LightData* lightData, uint32_t lightCount, ShadowView* views,
    FrameData& header) {
    SW_PROFILE_SCOPE("Plan shadows");
    for (ShadowPass& pass : cascadePasses) pass.draws.clear();
    cascadePasses.clear();
    localPasses.clear();
    shadowStats = {};

    const glm::mat4 inverseView = glm::inverse(view);
    const glm::vec3 forward = glm::normalize(-glm::vec3(inverseView[2]));
    header.cameraForward = forward;
    header.cascadeAtlas = cascadeSlot;
    header.localAtlas = localSlot;
    header.shadowTaps = shadowSettings.taps;
    if (shadowQuality == ShadowQuality::Off || !cascadeAtlas.image) return;

    // The draws that can cast at all, tested against each map's frustum.
    std::vector<uint32_t> casters;
    casters.reserve(gatheredDraws.size());
    for (uint32_t i = 0; i < gatheredDraws.size(); ++i) {
        if (gatheredDraws[i].castShadows) casters.push_back(i);
    }
    auto cull = [&](ShadowPass& pass) {
        const Frustum frustum(pass.viewProjection);
        for (uint32_t i : casters) {
            if (frustum.intersects(gatheredDraws[i].bounds)) pass.draws.push_back(i);
        }
        shadowStats.casterDraws += static_cast<uint32_t>(pass.draws.size());
    };

    // ---- The sun: cascaded shadow maps ----
    uint32_t sun = kNoShadow;
    for (uint32_t i = 0; i < lightCount; ++i) {
        if (descs[i].type == LightDesc::Type::Directional && descs[i].castShadows) {
            sun = i;
            break;
        }
    }
    if (sun != kNoShadow) {
        const uint32_t count = std::clamp(lighting.shadowCascades, 1u, kMaxCascades);
        const float distance = std::max(lighting.shadowDistance, 1.0f);
        const float nearest = std::min(0.1f, distance * 0.01f);
        // Split the view by distance: halfway between even and logarithmic spacing, so the
        // first cascade stays small (sharp) without starving the far ones.
        std::array<float, kMaxCascades> ends{};
        for (uint32_t c = 0; c < count; ++c) {
            const float t = static_cast<float>(c + 1) / static_cast<float>(count);
            const float even = nearest + (distance - nearest) * t;
            const float logarithmic = nearest * std::pow(distance / nearest, t);
            ends[c] = glm::mix(even, logarithmic, 0.75f);
        }
        ends[count - 1] = distance;
        header.cascadeCount = count;
        header.shadowDistance = distance;
        header.cascadeEnds = glm::vec4(ends[0], ends[1], ends[2], ends[3]);

        const glm::vec3 dir = glm::normalize(descs[sun].direction);
        const glm::vec3 up = upFor(dir);
        const glm::mat4 lightRotation = glm::lookAt(glm::vec3(0.0f), dir, up);
        const glm::mat4 inverseRotation = glm::inverse(lightRotation);
        const bool orthographic = std::abs(projection[2][3]) < 1e-6f;
        const float size = static_cast<float>(shadowSettings.cascadeSize);
        // How far behind each cascade casters can stand (tall buildings, mountains).
        const float casterReach = std::max(distance, 50.0f);
        for (uint32_t c = 0; c < count; ++c) {
            const float d0 = c == 0 ? 0.0f : ends[c - 1], d1 = ends[c];
            // The slice of the view frustum, in view space.
            std::array<glm::vec3, 8> corners;
            for (int k = 0; k < 8; ++k) {
                const float d = (k & 4) ? d1 : d0;
                const float x = orthographic ? 1.0f / std::abs(projection[0][0]) : d / std::abs(projection[0][0]);
                const float y = orthographic ? 1.0f / std::abs(projection[1][1]) : d / std::abs(projection[1][1]);
                corners[k] = glm::vec3((k & 1) ? x : -x, (k & 2) ? y : -y, -d);
            }
            // A bounding sphere, not a box: its size doesn't change as the camera turns, so
            // the map's texels stay put and shadow edges don't crawl.
            glm::vec3 center(0.0f);
            for (const glm::vec3& p : corners) center += p * 0.125f;
            float radius = 0.0f;
            for (const glm::vec3& p : corners) radius = std::max(radius, glm::length(p - center));
            radius = std::ceil(radius * 16.0f) / 16.0f;
            // Snap the center to whole texels in light space, so moving the camera doesn't
            // shift texel boundaries either.
            const float texel = 2.0f * radius / size;
            glm::vec4 lightSpace = lightRotation * (inverseView * glm::vec4(center, 1.0f));
            lightSpace.x = std::floor(lightSpace.x / texel) * texel;
            lightSpace.y = std::floor(lightSpace.y / texel) * texel;
            const glm::vec3 snapped = glm::vec3(inverseRotation * lightSpace);

            const float back = radius + casterReach;
            const glm::mat4 lightView = glm::lookAt(snapped - dir * back, snapped, up);
            // Reverse-Z: the light's end of the box is 1, the far end 0.
            const glm::mat4 lightProjection = glm::orthoRH_ZO(-radius, radius, -radius, radius, back + radius, 0.0f);

            ShadowView& v = views[c];
            v = {};
            v.viewProjection = lightProjection * lightView;
            v.rect = glm::vec4(static_cast<float>(c % 2) * 0.5f, static_cast<float>(c / 2) * 0.5f, 0.5f, 0.5f);
            v.texelSize = texel;
            v.perspective = 0;

            ShadowPass& pass = cascadePasses.emplace_back();
            pass.viewProjection = v.viewProjection;
            pass.rect = {
                {static_cast<int32_t>((c % 2) * shadowSettings.cascadeSize),
                 static_cast<int32_t>((c / 2) * shadowSettings.cascadeSize)},
                {shadowSettings.cascadeSize, shadowSettings.cascadeSize}};
            pass.depthBias = -1.0f;
            pass.slopeBias = -1.5f;
            cull(pass);
        }
        LightData& l = lightData[sun];
        l.shadowView = 0;
        l.shadowStrength = std::clamp(descs[sun].shadowStrength, 0.0f, 1.0f);
        l.shadowSoftness = filterRadius(descs[sun].shadowSoftness);
        l.shadowBias = std::max(descs[sun].shadowBias, 0.0f) * (1.0f + l.shadowSoftness * 0.25f);
        shadowStats.cascades = count;
    }

    // ---- Spot and point lights: tiles in the light atlas ----
    const uint32_t tile = shadowSettings.tileSize;
    const uint32_t perRow = shadowSettings.localAtlasSize / tile;
    const uint32_t capacity = std::min(perRow * perRow, kMaxLocalShadowViews);
    const Frustum camera(viewProjection);
    struct Candidate {
        uint32_t light;
        float priority;   // lower first: how close the camera is to the light's reach
    };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < lightCount; ++i) {
        const LightDesc& d = descs[i];
        if (d.type == LightDesc::Type::Directional || !d.castShadows) continue;
        const float range = std::max(d.range, 0.01f);
        if (!camera.intersects(glm::vec4(d.position, range))) continue;   // lights nothing on screen
        candidates.push_back({i, glm::length(d.position - cameraPosition) - range});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.priority < b.priority;
    });

    const float atlasSize = static_cast<float>(shadowSettings.localAtlasSize);
    uint32_t used = 0;
    for (const Candidate& candidate : candidates) {
        const LightDesc& d = descs[candidate.light];
        const bool point = d.type == LightDesc::Type::Point;
        const uint32_t faces = point ? 6 : 1;
        if (used + faces > capacity) {
            ++shadowStats.skippedLights;
            continue;
        }
        LightData& l = lightData[candidate.light];
        l.shadowView = kMaxCascades + used;
        l.shadowStrength = std::clamp(d.shadowStrength, 0.0f, 1.0f);
        l.shadowSoftness = filterRadius(d.shadowSoftness);
        l.shadowBias = std::max(d.shadowBias, 0.0f) * (1.0f + l.shadowSoftness * 0.25f);

        const float range = std::max(d.range, 0.01f);
        const float nearPlane = std::clamp(range * 0.002f, 0.02f, 0.5f);
        // Widen each map by the filter's reach, so samples near an edge stay in the tile.
        const float border = (l.shadowSoftness + 2.0f) * 2.0f / static_cast<float>(tile);
        for (uint32_t face = 0; face < faces; ++face) {
            glm::vec3 dir;
            float fov;
            if (point) {
                dir = kCubeFaces[face];
                fov = 2.0f * std::atan(1.0f + border);
            } else {
                dir = glm::normalize(d.direction);
                const float half = std::acos(std::clamp(d.cosOuter, -1.0f, 1.0f));
                fov = std::min(2.0f * std::atan(std::tan(half) * (1.0f + border)), glm::radians(170.0f));
            }
            const glm::mat4 lightView = glm::lookAt(d.position, d.position + dir, upFor(dir));
            const uint32_t index = used++;
            const uint32_t tx = index % perRow, ty = index / perRow;

            ShadowView& v = views[kMaxCascades + index];
            v = {};
            v.viewProjection = perspectiveReverse(fov, nearPlane, range) * lightView;
            v.rect = glm::vec4(
                static_cast<float>(tx * tile) / atlasSize, static_cast<float>(ty * tile) / atlasSize,
                static_cast<float>(tile) / atlasSize, static_cast<float>(tile) / atlasSize);
            v.texelSize = 2.0f * std::tan(fov * 0.5f) / static_cast<float>(tile);
            v.perspective = 1;

            ShadowPass& pass = localPasses.emplace_back();
            pass.viewProjection = v.viewProjection;
            pass.rect = {{static_cast<int32_t>(tx * tile), static_cast<int32_t>(ty * tile)}, {tile, tile}};
            pass.depthBias = -1.0f;
            pass.slopeBias = -1.5f;
            cull(pass);
        }
        ++shadowStats.shadowedLights;
    }
}

void Renderer::Impl::recordShadows(
    VkCommandBuffer cmd, Frame& f, const std::vector<ShadowPass>& passes, const vk::Image& atlas) {
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = atlas.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0f, 0};   // reverse-Z: nothing in the way

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, atlas.extent};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout, 0, 1, &bindlessSet, 0, nullptr);
    for (const ShadowPass& pass : passes) {
        if (pass.draws.empty()) continue;
        // No Y flip here (unlike the scene): the mesh shader reads these maps with the same
        // matrix, so any consistent convention works, and culling is off for single-sided
        // geometry anyway.
        const VkViewport viewport{
            static_cast<float>(pass.rect.offset.x),
            static_cast<float>(pass.rect.offset.y),
            static_cast<float>(pass.rect.extent.width),
            static_cast<float>(pass.rect.extent.height),
            0.0f,
            1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &pass.rect);
        vkCmdSetDepthBias(cmd, pass.depthBias, 0.0f, pass.slopeBias);
        ShadowPushConstants push{f.drawAddress, 0, pass.viewProjection};
        vkCmdPushConstants(
            cmd, shadowLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        VkBuffer boundIndices = VK_NULL_HANDLE;
        for (uint32_t i : pass.draws) {
            const DrawCommand& draw = gatheredDraws[i];
            if (draw.indexBuffer != boundIndices) {
                vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                boundIndices = draw.indexBuffer;
            }
            vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, draw.drawIndex);
        }
    }
    vkCmdEndRendering(cmd);
}

}   // namespace shockwave
