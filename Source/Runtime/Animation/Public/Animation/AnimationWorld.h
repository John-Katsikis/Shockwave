#pragma once

// AnimationWorld: plays every Animator in a World, and game code's way into animation
// during Play, as game.animation(). One per Play session, like AudioWorld; the editor
// keeps one more to show Animators while editing.
//
//     game.animation().play(hero, "Run");          // cross-fades over the Animator's blend_time
//     game.animation().play(hero, "Jump", 0.1f);   // ...or over 0.1 s
//     if (game.animation().finished(hero)) ...     // a clip that doesn't loop reached its end
//     glm::mat4 hand;
//     if (game.animation().jointTransform(hero, "hand_r", hand)) sword.setTransform(hand);
//
// Calls with an entity that has no Animator (or whose model has no such clip) do nothing
// and return false.
//
// Threading: main thread only (GameMode::beginPlay, frame, fixedUpdate, endPlay), never
// from systems. update() spreads the sampling across the job system.

#include <Animation/Animation.h>
#include <Engine/EntityRef.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class JobSystem;

// Per-frame cost and size, for the Stats window and tools.
struct AnimationStats {
    uint32_t animators = 0;   // Animators posed last update
    uint32_t joints = 0;   // joints (model nodes) posed
    uint32_t blending = 0;   // Animators mid cross-fade
    uint32_t rigs = 0;   // models with a skeleton built for animation
    float updateMs = 0;   // the last update, sampling on all cores included
};

class AnimationWorld final : public Subsystem {
public:
    // assets: where models come from; null in tools without any (nothing animates).
    AnimationWorld(World& world, const Assets* assets, JobSystem& jobs);
    ~AnimationWorld() override;
    AnimationWorld(const AnimationWorld&) = delete;
    AnimationWorld& operator=(const AnimationWorld&) = delete;

    // Once per frame, before rendering: moves every playing Animator on by dt seconds
    // (times its speed) and poses its model. dt 0 holds everything still, finishes
    // cross-fades at once, and shows each Animator at its time: the editor's view.
    void update(double dt);
    // Forgets where every Animator had got to: the next update starts each from its time.
    void restart();

    // ---- Game code ----
    // Cross-fades to clip over blendTime seconds (negative: the Animator's blend_time) and
    // plays it from the start. Already playing it: nothing changes. False if the model
    // has no such clip.
    bool play(Entity entity, std::string_view clip, float blendTime = -1.0f);
    bool play(const EntityRef& entity, std::string_view clip, float blendTime = -1.0f) {
        return play(entity.entity(), clip, blendTime);
    }
    // Holds the current pose (playing off).
    bool stop(Entity entity);
    bool stop(const EntityRef& entity) { return stop(entity.entity()); }
    // Seconds into the clip that's playing, and that clip's name and length.
    float time(Entity entity) const;
    std::string clip(Entity entity) const;
    float duration(Entity entity) const;
    // A clip that doesn't loop has reached its end (or start, playing backwards).
    bool finished(Entity entity) const;
    bool finished(const EntityRef& entity) const { return finished(entity.entity()); }
    // The clips of the entity's model, in file order.
    std::vector<std::string> clips(Entity entity) const;
    // Where a joint (any node of the model, by name) is in the world this frame. For
    // holding a sword in a hand, or a camera on a head. False if there's no such joint.
    bool jointTransform(Entity entity, std::string_view joint, glm::mat4& world) const;

    AnimationStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}   // namespace shockwave
