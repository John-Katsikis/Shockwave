#include <Animation/AnimationWorld.h>

#include "Rig.h"

#include <Assets/Assets.h>
#include <Core/Jobs.h>
#include <Core/Profile.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Pose.h>

#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/span.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace shockwave {
namespace {

using ozz::animation::SamplingJob;

// One clip being played: which, where in it, and ozz's sampling cache for it.
struct Track {
    int clip = -1;   // index into ModelData::animations; -1: the rest pose
    float time = 0;
    std::unique_ptr<SamplingJob::Context> context = std::make_unique<SamplingJob::Context>();
};

// What an Animator is doing. Kept here, not in the component, so the Animator stays
// plain data that scenes save and Play restores exactly.
struct State {
    Entity entity;
    std::shared_ptr<const ModelAsset> model;
    std::shared_ptr<const animation::Rig> rig;
    bool started = false;
    std::string clipName;   // the Animator's clip as last seen, to notice changes
    float seenTime = 0;   // its time as last seen, to notice jumps
    Track current, previous;
    float fade = 1, fadeDuration = 0;   // cross-fade progress from previous to current, 0..1
    float nextBlend = -1;   // play()'s blend time for the next clip change
    bool finished = false;
    ozz::vector<ozz::math::SoaTransform> locals, previousLocals, blended;
    ozz::vector<ozz::math::Float4x4> models;
};

int clipIndex(const ModelData& model, const std::string& name) {
    if (name.empty()) return model.animations.empty() ? -1 : 0;
    for (size_t i = 0; i < model.animations.size(); ++i)
        if (model.animations[i].name == name) return static_cast<int>(i);
    return -1;
}

float clipDuration(const State& s, int clip) {
    return clip >= 0 ? std::max(s.model->data.animations[size_t(clip)].duration, 1e-3f) : 0.0f;
}

// Moves a track on by dt seconds. Returns true when a clip that doesn't loop hit its end.
bool advance(const State& s, Track& track, float dt, bool loop) {
    if (track.clip < 0) return false;
    const float duration = clipDuration(s, track.clip);
    track.time += dt;
    if (loop) {
        track.time = std::fmod(track.time, duration);
        if (track.time < 0) track.time += duration;
        return false;
    }
    const float clamped = std::clamp(track.time, 0.0f, duration);
    const bool ended = clamped != track.time || (dt > 0 && clamped >= duration) || (dt < 0 && clamped <= 0);
    track.time = clamped;
    return ended;
}

void sample(const State& s, Track& track, ozz::vector<ozz::math::SoaTransform>& output) {
    const auto& skeleton = *s.rig->skeleton;
    const auto* clip = track.clip >= 0 && size_t(track.clip) < s.rig->clips.size() ? s.rig->clips[size_t(track.clip)].get()
                                                                                       : nullptr;
    if (!clip) {   // no clip (or it couldn't be built): the model as it was made
        const auto rest = skeleton.joint_rest_poses();
        output.assign(rest.begin(), rest.end());
        return;
    }
    if (track.context->max_tracks() < clip->num_tracks()) track.context->Resize(clip->num_tracks());
    SamplingJob job;
    job.animation = clip;
    job.context = track.context.get();
    job.ratio = std::clamp(track.time / clipDuration(s, track.clip), 0.0f, 1.0f);
    job.output = ozz::make_span(output);
    if (!job.Run()) {
        const auto rest = skeleton.joint_rest_poses();
        output.assign(rest.begin(), rest.end());
    }
}

}   // namespace

struct AnimationWorld::Impl {
    World& world;
    const Assets* assets;
    JobSystem& jobs;
    std::vector<std::unique_ptr<State>> states;   // by entity index
    std::vector<State*> work;   // this update's
    AnimationStats stats;

    Impl(World& w, const Assets* a, JobSystem& j) : world(w), assets(a), jobs(j) {}

    State* find(Entity e) const {
        if (e.index >= states.size() || !states[e.index]) return nullptr;
        State* s = states[e.index].get();
        return s->entity == e && world.alive(e) ? s : nullptr;
    }
    State& stateFor(Entity e) {
        if (e.index >= states.size()) states.resize(size_t(e.index) + 1);
        auto& slot = states[e.index];
        if (!slot || slot->entity != e) {
            slot = std::make_unique<State>();
            slot->entity = e;
        }
        return *slot;
    }

    void animate(State& s, const Animator& animator, ModelPose& pose, float dt);
};

AnimationWorld::AnimationWorld(World& world, const Assets* assets, JobSystem& jobs)
    : m(std::make_unique<Impl>(world, assets, jobs)) {}
AnimationWorld::~AnimationWorld() = default;

void AnimationWorld::restart() { m->states.clear(); }

void AnimationWorld::update(double dt) {
    SW_PROFILE_SCOPE("Animation");
    const auto start = std::chrono::steady_clock::now();
    World& world = m->world;
    m->work.clear();
    AnimationStats stats;
    // On the main thread: who animates which model. Structural changes (adding poses)
    // happen here, before the parallel part.
    std::vector<Entity> animated;
    if (m->assets) {
        world.each<const Animator, const ModelRenderer>([&](Entity e, const Animator&, const ModelRenderer& renderer) {
            auto asset = m->assets->model(renderer.model);
            State& s = m->stateFor(e);
            if (s.model != asset) {
                const Entity entity = s.entity;
                s = State{};
                s.entity = entity;
                s.model = asset;
                s.rig = animation::rigFor(asset);
            }
            if (!asset || !s.rig || !s.rig->skeleton) return;
            animated.push_back(e);
        });
    }
    // Poses of entities that stopped animating go, so they show their rest pose again.
    if (const Pool<ModelPose>* poses = world.pool<ModelPose>()) {
        std::vector<uint8_t> keep;
        for (Entity e : animated) {
            if (e.index >= keep.size()) keep.resize(size_t(e.index) + 1, 0);
            keep[e.index] = 1;
        }
        std::vector<Entity> stale;
        for (Entity e : poses->entities())
            if (e.index >= keep.size() || !keep[e.index]) stale.push_back(e);
        for (Entity e : stale) world.remove<ModelPose>(e);
    }
    for (Entity e : animated) {
        if (!world.has<ModelPose>(e)) world.add<ModelPose>(e);
        m->work.push_back(&m->stateFor(e));
    }
    // On every core: advance, sample, blend and pose each Animator.
    const float step = std::isfinite(dt) ? static_cast<float>(dt) : 0.0f;
    m->jobs.parallelFor(m->work.size(), 4, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            State& s = *m->work[i];
            m->animate(s, *world.get<Animator>(s.entity), *world.get<ModelPose>(s.entity), step);
        }
    });
    for (const State* s : m->work) {
        ++stats.animators;
        stats.joints += static_cast<uint32_t>(s->model->data.nodes.size());
        if (s->fade < 1) ++stats.blending;
    }
    stats.rigs = animation::rigCount();
    stats.updateMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    m->stats = stats;
}

void AnimationWorld::Impl::animate(State& s, const Animator& animator, ModelPose& pose, float dt) {
    const ModelData& model = s.model->data;
    const bool editing = dt == 0.0f;   // the editor's still view: no fades, show time as set
    if (!s.started) {
        s.current.clip = clipIndex(model, animator.clip);
        s.current.time = animator.time;
        s.clipName = animator.clip;
        s.seenTime = animator.time;
        s.started = true;
    } else if (animator.clip != s.clipName) {
        s.clipName = animator.clip;
        const int wanted = clipIndex(model, animator.clip);
        if (wanted != s.current.clip) {
            std::swap(s.previous, s.current);
            s.current.clip = wanted;
            s.current.time = 0;
            s.finished = false;
            const float blend = s.nextBlend >= 0 ? s.nextBlend : animator.blendTime;
            s.fadeDuration = std::max(blend, 0.0f);
            s.fade = s.fadeDuration > 0 && s.previous.clip >= 0 && !editing ? 0.0f : 1.0f;
            if (editing) s.current.time = animator.time;
        }
        s.nextBlend = -1;
    }
    if (animator.time != s.seenTime || editing) {   // jumped (game code), or the editor's view
        s.seenTime = animator.time;
        s.current.time = animator.time;
        s.finished = false;
    }
    if (animator.playing && !editing) {
        if (advance(s, s.current, dt * animator.speed, animator.loop)) s.finished = true;
        if (s.fade < 1) advance(s, s.previous, dt * animator.speed, true);
    } else if (s.current.clip >= 0) {
        s.current.time = std::clamp(s.current.time, 0.0f, clipDuration(s, s.current.clip));
    }
    if (editing)
        s.fade = 1;
    else if (s.fade < 1)
        s.fade = std::min(1.0f, s.fade + std::abs(dt) / std::max(s.fadeDuration, 1e-4f));

    const auto& skeleton = *s.rig->skeleton;
    const size_t soa = size_t(skeleton.num_soa_joints());
    s.locals.resize(soa);
    sample(s, s.current, s.locals);
    const ozz::vector<ozz::math::SoaTransform>* result = &s.locals;
    if (s.fade < 1) {
        s.previousLocals.resize(soa);
        s.blended.resize(soa);
        sample(s, s.previous, s.previousLocals);
        ozz::animation::BlendingJob::Layer layers[2];
        layers[0].transform = ozz::make_span(s.previousLocals);
        layers[0].weight = 1.0f - s.fade;
        layers[1].transform = ozz::make_span(s.locals);
        layers[1].weight = s.fade;
        ozz::animation::BlendingJob blend;
        blend.layers = layers;
        blend.rest_pose = skeleton.joint_rest_poses();
        blend.output = ozz::make_span(s.blended);
        if (blend.Run()) result = &s.blended;
    }
    s.models.resize(size_t(skeleton.num_joints()));
    ozz::animation::LocalToModelJob toModel;
    toModel.skeleton = &skeleton;
    toModel.input = ozz::make_span(*result);
    toModel.output = ozz::make_span(s.models);
    pose.model = s.model;
    pose.nodes.resize(s.models.size());
    if (!toModel.Run()) {
        for (size_t i = 0; i < pose.nodes.size(); ++i) pose.nodes[i] = model.nodes[i].world;
        return;
    }
    for (size_t i = 0; i < s.models.size(); ++i)
        for (int c = 0; c < 4; ++c) ozz::math::StorePtrU(s.models[i].cols[c], &pose.nodes[i][c][0]);
}

// ---- Game code ----

bool AnimationWorld::play(Entity entity, std::string_view clip, float blendTime) {
    Animator* animator = m->world.get<Animator>(entity);
    const ModelRenderer* renderer = m->world.get<ModelRenderer>(entity);
    if (!animator || !renderer || !m->assets) return false;
    const auto asset = m->assets->model(renderer->model);
    if (!asset || !findAnimation(asset->data, clip)) return false;
    animator->playing = true;
    if (animator->clip == clip) return true;
    animator->clip = std::string(clip);
    if (State* s = m->find(entity)) s->nextBlend = blendTime;
    return true;
}

bool AnimationWorld::stop(Entity entity) {
    Animator* animator = m->world.get<Animator>(entity);
    if (!animator) return false;
    animator->playing = false;
    return true;
}

float AnimationWorld::time(Entity entity) const {
    const State* s = m->find(entity);
    return s ? s->current.time : 0.0f;
}

std::string AnimationWorld::clip(Entity entity) const {
    const State* s = m->find(entity);
    if (!s || !s->model || s->current.clip < 0) return {};
    return s->model->data.animations[size_t(s->current.clip)].name;
}

float AnimationWorld::duration(Entity entity) const {
    const State* s = m->find(entity);
    return s && s->model ? clipDuration(*s, s->current.clip) : 0.0f;
}

bool AnimationWorld::finished(Entity entity) const {
    const State* s = m->find(entity);
    return s && s->finished;
}

std::vector<std::string> AnimationWorld::clips(Entity entity) const {
    std::vector<std::string> names;
    const ModelRenderer* renderer = m->world.get<ModelRenderer>(entity);
    if (!renderer || !m->assets) return names;
    if (const auto asset = m->assets->model(renderer->model))
        for (const auto& clip : asset->data.animations) names.push_back(clip.name);
    return names;
}

bool AnimationWorld::jointTransform(Entity entity, std::string_view joint, glm::mat4& result) const {
    const ModelRenderer* renderer = m->world.get<ModelRenderer>(entity);
    if (!renderer || !m->assets || !m->world.has<Transform>(entity)) return false;
    const auto asset = m->assets->model(renderer->model);
    if (!asset) return false;
    const ModelPose* pose = m->world.get<ModelPose>(entity);
    if (pose && pose->model != asset) pose = nullptr;
    const auto& nodes = asset->data.nodes;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].name != joint) continue;
        result = worldMatrix(m->world, entity) * (pose ? pose->nodes[i] : nodes[i].world);
        return true;
    }
    return false;
}

AnimationStats AnimationWorld::stats() const { return m->stats; }

}   // namespace shockwave
