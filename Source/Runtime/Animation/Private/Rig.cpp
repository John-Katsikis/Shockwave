#include "Rig.h"

#include <Engine/Components.h>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace shockwave::animation {
namespace {

namespace offline = ozz::animation::offline;

ozz::math::Float3 float3(const glm::vec3& v) { return {v.x, v.y, v.z}; }
ozz::math::Quaternion quaternion(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

// Each node's place relative to its parent, as translation, rotation and scale.
std::vector<Transform> restPose(const ModelData& model) {
    std::vector<Transform> rest;
    rest.reserve(model.nodes.size());
    for (const auto& node : model.nodes) rest.push_back(Transform::fromMatrix(node.local));
    return rest;
}

ozz::unique_ptr<ozz::animation::Skeleton> buildSkeleton(
    const ModelData& model, const std::vector<Transform>& rest, std::string& error) {
    const size_t count = model.nodes.size();
    if (count == 0) {
        error = "the model has no nodes";
        return nullptr;
    }
    if (count > ozz::animation::Skeleton::kMaxJoints) {
        error = "the model has " + std::to_string(count) + " nodes; animation handles up to " +
                std::to_string(int(ozz::animation::Skeleton::kMaxJoints));
        return nullptr;
    }
    std::vector<std::vector<uint32_t>> children(count);
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < count; ++i) {
        const int parent = model.nodes[i].parent;
        if (parent < 0)
            roots.push_back(i);
        else
            children[size_t(parent)].push_back(i);
    }
    std::function<void(offline::RawSkeleton::Joint&, uint32_t)> fill = [&](offline::RawSkeleton::Joint& joint,
                                                                           uint32_t node) {
        joint.name = model.nodes[node].name.c_str();
        joint.transform.translation = float3(rest[node].position);
        joint.transform.rotation = quaternion(glm::normalize(rest[node].rotation));
        joint.transform.scale = float3(rest[node].scale);
        joint.children.resize(children[node].size());
        for (size_t c = 0; c < children[node].size(); ++c) fill(joint.children[c], children[node][c]);
    };
    offline::RawSkeleton raw;
    raw.roots.resize(roots.size());
    for (size_t r = 0; r < roots.size(); ++r) fill(raw.roots[r], roots[r]);
    auto skeleton = offline::SkeletonBuilder()(raw);
    if (!skeleton) {
        error = "ozz-animation couldn't build a skeleton from the model's nodes";
        return nullptr;
    }
    // ozz lists joints depth first, which is the order the model's nodes are in. Check, so
    // joint i really is node i.
    const auto parents = skeleton->joint_parents();
    for (size_t i = 0; i < count; ++i) {
        if (parents[i] != model.nodes[i].parent) {
            error = "the model's nodes aren't in depth-first order";
            return nullptr;
        }
    }
    return skeleton;
}

// Keys must rise strictly and lie within the clip; drops the ones that don't.
template <class Raw, class Key, class Convert>
void copyKeys(ozz::vector<Raw>& out, const std::vector<Key>& keys, float duration, Convert convert) {
    for (const Key& key : keys) {
        if (!std::isfinite(key.time)) continue;
        const float time = std::clamp(key.time, 0.0f, duration);
        if (!out.empty() && time <= out.back().time) continue;
        out.push_back({time, convert(key.value)});
    }
}

ozz::unique_ptr<ozz::animation::Animation> buildClip(
    const ModelData& model, const ModelAnimation& clip, const std::vector<Transform>& rest, std::string& error) {
    offline::RawAnimation raw;
    raw.name = clip.name.c_str();
    raw.duration = std::max(clip.duration, 1e-3f);
    raw.tracks.resize(model.nodes.size());
    for (const ModelChannel& channel : clip.channels) {
        if (channel.node >= raw.tracks.size()) continue;
        auto& track = raw.tracks[channel.node];
        copyKeys(track.translations, channel.positions, raw.duration, float3);
        copyKeys(track.rotations, channel.rotations, raw.duration, [](const glm::quat& q) {
            return quaternion(glm::normalize(q));
        });
        copyKeys(track.scales, channel.scales, raw.duration, float3);
    }
    // A node the clip doesn't move stays where the model puts it (ozz would use identity).
    for (size_t i = 0; i < raw.tracks.size(); ++i) {
        auto& track = raw.tracks[i];
        if (track.translations.empty()) track.translations.push_back({0.0f, float3(rest[i].position)});
        if (track.rotations.empty()) track.rotations.push_back({0.0f, quaternion(glm::normalize(rest[i].rotation))});
        if (track.scales.empty()) track.scales.push_back({0.0f, float3(rest[i].scale)});
    }
    auto built = offline::AnimationBuilder()(raw);
    if (!built) error = "ozz-animation couldn't build clip '" + clip.name + "'";
    return built;
}

std::shared_ptr<const Rig> build(const ModelData& model) {
    const auto start = std::chrono::steady_clock::now();
    auto rig = std::make_shared<Rig>();
    const auto rest = restPose(model);
    rig->skeleton = buildSkeleton(model, rest, rig->error);
    if (rig->skeleton) {
        rig->clips.resize(model.animations.size());
        rig->clipErrors.resize(model.animations.size());
        for (size_t i = 0; i < model.animations.size(); ++i)
            rig->clips[i] = buildClip(model, model.animations[i], rest, rig->clipErrors[i]);
    }
    rig->buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    return rig;
}

struct Cache {
    std::mutex mutex;
    std::unordered_map<const ModelAsset*, std::pair<std::weak_ptr<const ModelAsset>, std::shared_ptr<const Rig>>>
        rigs;
};
Cache& cache() {
    static Cache instance;
    return instance;
}

}   // namespace

std::shared_ptr<const Rig> rigFor(const std::shared_ptr<const ModelAsset>& model) {
    if (!model) return nullptr;
    Cache& c = cache();
    {
        std::lock_guard lock(c.mutex);
        std::erase_if(c.rigs, [](const auto& entry) { return entry.second.first.expired(); });
        if (auto found = c.rigs.find(model.get()); found != c.rigs.end()) return found->second.second;
    }
    auto rig = build(model->data);   // outside the lock: big models take a moment
    if (!rig->error.empty()) SW_LOG(LogAnimation, Warning, "Animation: {}", rig->error);
    std::lock_guard lock(c.mutex);
    auto& entry = c.rigs[model.get()];
    if (!entry.second) entry = {model, rig};
    return entry.second;
}

uint32_t rigCount() {
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    return static_cast<uint32_t>(c.rigs.size());
}

}   // namespace shockwave::animation
