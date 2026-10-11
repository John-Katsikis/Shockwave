// Skeletal animation without a GPU: importing a skinned glTF with its clips, playing,
// looping, cross-fading, stopping at the end, joints, and the Animator checks.
#include <Animation/Animation.h>
#include <Animation/AnimationWorld.h>

#include <Assets/Assets.h>
#include <Core/Jobs.h>
#include <Engine/Components.h>
#include <Engine/Pose.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
bool near(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }
float difference(const ModelPose& a, const ModelPose& b) {
    float most = 0;
    for (size_t i = 0; i < a.nodes.size(); ++i)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) most = std::max(most, std::abs(a.nodes[i][c][r] - b.nodes[i][c][r]));
    return most;
}
}   // namespace

int main(int argc, char** argv) {
    try {
        check(argc > 1, "usage: animation_test <source folder>");
        const fs::path fox = fs::path(argv[1]) / "Games/Sample/Content/models/Fox.glb";

        // ---- Import ----
        auto asset = std::make_shared<ModelAsset>();
        std::string error;
        check(loadModel(fox, asset->data, error), "Fox.glb doesn't load: " + error);
        const ModelData& data = asset->data;
        check(hasSkinnedMeshes(data), "Fox.glb should have a skinned mesh");
        check(data.animations.size() == 3, "Fox.glb should have 3 clips");
        check(findAnimation(data, "Survey") && findAnimation(data, "Walk") && findAnimation(data, "Run"),
              "Fox.glb's clips are Survey, Walk and Run");
        for (const auto& clip : data.animations)
            check(clip.duration > 0.1f && !clip.channels.empty(), "clip " + clip.name + " has keys and a length");
        for (const auto& mesh : data.meshes) {
            if (!mesh.skinned()) continue;
            check(mesh.skin.size() == mesh.data.vertices.size(), "one VertexSkin per vertex");
            for (const auto& skin : mesh.skin) {
                check(near(skin.weights.x + skin.weights.y + skin.weights.z + skin.weights.w, 1.0f, 1e-3f),
                      "skin weights add up to 1");
                for (int k = 0; k < 4; ++k) check(skin.joints[k] < mesh.bones.size(), "joint indices are in range");
            }
            for (const auto& bone : mesh.bones) check(bone.node < data.nodes.size(), "bones name model nodes");
        }
        std::cout << "import: " << data.nodes.size() << " nodes, " << data.animations.size() << " clips\n";

        // ---- Playing ----
        Assets assets;
        const ModelHandle handle = assets.addModel("models/Fox.glb", asset);
        const auto published = assets.model(handle);
        registerAnimationComponents();
        JobSystem jobs(2);
        World world;
        const Entity e = world.create();
        world.add<Transform>(e);
        world.add<ModelRenderer>(e, {handle});
        world.add<Animator>(e, {.clip = "Walk"});
        AnimationWorld animation(world, &assets, jobs);
        animation.update(0.0);
        const ModelPose* pose = world.get<ModelPose>(e);
        check(pose && pose->model == published && pose->nodes.size() == data.nodes.size(), "an Animator poses its model");
        const ModelPose start = *pose;
        check(animation.clip(e) == "Walk" && near(animation.time(e), 0), "starts Walk at 0");

        animation.update(0.1);
        check(near(animation.time(e), 0.1f), "time moves on with dt");
        check(difference(start, *world.get<ModelPose>(e)) > 1e-3f, "the pose changes as the clip plays");

        // Speed and looping.
        world.get<Animator>(e)->speed = 2.0f;
        const float walk = animation.duration(e);
        animation.update(walk);   // two clip lengths at speed 2
        check(near(animation.time(e), 0.1f, 1e-3f), "looping wraps around the clip");
        world.get<Animator>(e)->speed = 1.0f;

        // Editing view: dt 0 shows the Animator's time exactly.
        world.get<Animator>(e)->time = 0.0f;
        animation.update(0.0);
        check(near(animation.time(e), 0.0f) && difference(start, *world.get<ModelPose>(e)) < 1e-4f,
              "setting time jumps to that pose");

        // Cross-fade.
        check(animation.play(e, "Run", 0.5f), "play Run");
        check(!animation.play(e, "Fly"), "play refuses a clip the model doesn't have");
        animation.update(0.1);
        check(animation.clip(e) == "Run" && animation.stats().blending == 1, "cross-fading into Run");
        animation.update(0.5);
        check(animation.stats().blending == 0, "the cross-fade finishes after blend time");

        // A clip that doesn't loop stops on its last frame.
        world.get<Animator>(e)->loop = false;
        animation.update(10.0);
        check(animation.finished(e) && near(animation.time(e), animation.duration(e)), "non-looping clips end");

        // Holding.
        animation.stop(e);
        const ModelPose held = *world.get<ModelPose>(e);
        animation.update(0.3);
        check(difference(held, *world.get<ModelPose>(e)) < 1e-5f, "stop holds the pose");

        // Joints, by node name, in the world.
        world.get<Transform>(e)->position = {10, 0, 0};
        glm::mat4 joint;
        check(animation.jointTransform(e, data.nodes.back().name, joint), "find a joint by name");
        check(!animation.jointTransform(e, "no such joint", joint), "unknown joints are refused");
        check(joint[3].x > 5.0f, "joints are in world space");

        // Removing the Animator removes the pose: the model shows its rest pose again.
        world.remove<Animator>(e);
        animation.update(0.1);
        check(!world.has<ModelPose>(e), "no Animator, no pose");

        // ---- Checks ----
        world.add<Animator>(e, {.clip = "Rn"});
        auto problems = validateAnimator(world, e, assets);
        check(problems.size() == 1 && problems[0].message.find("Survey, Walk, Run") != std::string::npos,
              "an unknown clip lists the model's clips");
        world.get<Animator>(e)->clip = "Run";
        check(validateAnimator(world, e, assets).empty(), "a good Animator has no problems");
        world.get<Animator>(e)->time = 99;
        problems = validateAnimator(world, e, assets);
        check(problems.size() == 1 && problems[0].warning, "a time past the end is a warning");
        const Entity bare = world.create();
        world.add<Animator>(bare);
        check(!validateAnimator(world, bare, assets).empty(), "an Animator needs a ModelRenderer");
        check(animationClips(assets, "models/Fox.glb").size() == 3, "animationClips lists clips");

        // ---- Many at once, on every core ----
        for (int i = 0; i < 200; ++i) {
            const Entity f = world.create();
            world.add<Transform>(f);
            world.add<ModelRenderer>(f, {handle});
            world.add<Animator>(f, {.clip = i % 2 ? "Run" : "Survey", .time = 0.01f * float(i % 50)});
        }
        animation.update(1.0 / 60.0);
        check(animation.stats().animators == 201, "every Animator with a model is posed");
        std::cout << "200 foxes: " << animation.stats().updateMs << " ms\n";
        std::cout << "animation test passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "animation test failed: " << e.what() << "\n";
        return 1;
    }
}
