#pragma once

// Skeletal animation: characters and creatures that walk, run and wave, from the
// animation clips inside their model files (glTF, FBX, .blend...). ozz-animation samples
// and blends the clips; the renderer bends skinned meshes to the result on the GPU. See
// docs/Animation.md.
//
// A scene animates a model by giving the entity with its ModelRenderer an Animator.
// Game code switches clips through game.animation() (AnimationWorld.h), or by changing
// the Animator's fields.

#include <Engine/World.h>

#include <string>
#include <vector>

namespace shockwave {

class Assets;

// Plays one of the clips of the entity's ModelRenderer model.
struct Animator {
    std::string clip;   // the clip's name, as in the model; empty plays its first clip
    bool playing = true;   // off holds the current pose
    bool loop = true;   // starts again at the end; off stops on the last frame
    float speed = 1.0f;   // 1 is as authored, 2 twice as fast, negative plays backwards
    // Seconds into the clip to start from. The editor shows this pose while not playing;
    // changing it during Play jumps there.
    float time = 0.0f;
    float blendTime = 0.2f;   // seconds to cross-fade from the previous clip when clip changes
};
// Registers Animator for scenes, the Inspector and player --schema. The hosts call it at
// startup, like registerAudioComponents.
void registerAnimationComponents();

struct AnimationProblem {
    std::string message;
    bool warning = false;   // it still plays, perhaps not as intended
};
// What's wrong with the Animator on e: no ModelRenderer, a model without clips, a clip
// name the model doesn't have (the message lists the ones it has)...
std::vector<AnimationProblem> validateAnimator(const World& world, Entity e, const Assets& assets);

// The clips of a model, by name, in file order; empty if it has none or isn't loaded.
struct AnimationClipInfo {
    std::string name;
    float duration = 0;   // seconds
};
std::vector<AnimationClipInfo> animationClips(const Assets& assets, std::string_view model);

}   // namespace shockwave
