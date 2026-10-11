#include <Animation/Animation.h>

#include "Rig.h"

#include <Assets/Assets.h>
#include <Engine/Components.h>

#include <cmath>

namespace shockwave {

void registerAnimationComponents() {
    registerComponent<Animator>(
        "Animator",
        {field("clip", &Animator::clip), field("playing", &Animator::playing), field("loop", &Animator::loop),
         field("speed", &Animator::speed, .01f, -10, 10), field("time", &Animator::time, .01f, 0, 3600),
         field("blend_time", &Animator::blendTime, .01f, 0, 10)},
        "Animation");
}

std::vector<AnimationClipInfo> animationClips(const Assets& assets, std::string_view model) {
    std::vector<AnimationClipInfo> clips;
    if (const auto asset = assets.model(assets.findModel(model)))
        for (const auto& clip : asset->data.animations) clips.push_back({clip.name, clip.duration});
    return clips;
}

std::vector<AnimationProblem> validateAnimator(const World& world, Entity e, const Assets& assets) {
    std::vector<AnimationProblem> problems;
    const Animator* animator = world.get<Animator>(e);
    if (!animator) return problems;
    if (!std::isfinite(animator->speed) || !std::isfinite(animator->time) || !std::isfinite(animator->blendTime))
        problems.push_back({"Animator speed, time and blend_time must be numbers"});
    if (animator->blendTime < 0) problems.push_back({"Animator blend_time can't be negative"});
    if (animator->time < 0) problems.push_back({"Animator time can't be negative"});
    const ModelRenderer* renderer = world.get<ModelRenderer>(e);
    if (!renderer) {
        problems.push_back({"Animator needs a ModelRenderer on the same entity: it animates that model"});
        return problems;
    }
    const auto asset = assets.model(renderer->model);
    if (!asset) return problems;   // the ModelRenderer's own problem, reported elsewhere
    const std::string name = assets.modelName(renderer->model);
    const auto& clips = asset->data.animations;
    if (clips.empty()) {
        problems.push_back({"Model '" + name + "' has no animation clips, so the Animator has nothing to play"});
        return problems;
    }
    const ModelAnimation* clip = animator->clip.empty() ? &clips.front() : findAnimation(asset->data, animator->clip);
    if (!clip) {
        std::string list;
        for (const auto& each : clips) list += (list.empty() ? "" : ", ") + each.name;
        problems.push_back({"Model '" + name + "' has no clip '" + animator->clip + "'; it has: " + list});
    } else if (animator->time > clip->duration + 1e-3f) {
        problems.push_back(
            {"Animator time " + std::to_string(animator->time) + " is past the end of '" + clip->name + "' (" +
                 std::to_string(clip->duration) + " s)",
             true});
    }
    const auto rig = animation::rigFor(asset);
    if (rig && !rig->error.empty()) problems.push_back({"Model '" + name + "' can't be animated: " + rig->error});
    if (rig && clip)
        for (size_t i = 0; i < clips.size(); ++i)
            if (&clips[i] == clip && i < rig->clipErrors.size() && !rig->clipErrors[i].empty()) problems.push_back({rig->clipErrors[i]});
    return problems;
}

}   // namespace shockwave
