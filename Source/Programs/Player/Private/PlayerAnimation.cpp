// The player's skeletal animation tools: --animations lists what each model can play,
// and --check validates every Animator in a scene. See docs/Animation.md.
#include "Player.h"

#include <Animation/Animation.h>
#include <Assets/Assets.h>
#include <Engine/Components.h>

#include <algorithm>
#include <cstdio>

namespace player {

int Player::listAnimations() {
    const shockwave::Assets& assets = engine().assets();
    auto models = assets.models();
    std::sort(models.begin(), models.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    int shown = 0;
    for (const auto& [name, handle] : models) {
        const bool asked = std::find(m_tool.animationModels.begin(), m_tool.animationModels.end(), name) !=
                           m_tool.animationModels.end();
        if (!m_tool.animationModels.empty() && !asked) continue;
        const auto asset = assets.model(handle);
        if (!asset) continue;
        const auto& data = asset->data;
        size_t skinned = 0, joints = 0;
        for (const auto& mesh : data.meshes)
            if (mesh.skinned()) {
                ++skinned;
                joints = std::max(joints, mesh.bones.size());
            }
        // Without a request, only models that can animate.
        if (m_tool.animationModels.empty() && skinned == 0 && data.animations.empty()) continue;
        ++shown;
        std::printf(
            "%s: %zu nodes, %zu skinned mesh%s (up to %zu joints each), %zu clip%s\n", name.c_str(), data.nodes.size(),
            skinned, skinned == 1 ? "" : "es", joints, data.animations.size(), data.animations.size() == 1 ? "" : "s");
        for (const auto& clip : data.animations)
            std::printf(
                "  clip \"%s\" %.3f s, %zu animated nodes\n", clip.name.c_str(), static_cast<double>(clip.duration),
                clip.channels.size());
    }
    for (const std::string& name : m_tool.animationModels) {
        if (!assets.findModel(name).valid()) {
            std::fprintf(stderr, "no model \"%s\" in %s\n", name.c_str(), m_project.relative(m_project.assetFolder).c_str());
            return 1;
        }
    }
    if (shown == 0) std::printf("no animated models in %s\n", m_project.relative(m_project.assetFolder).c_str());
    return 0;
}

void Player::checkAnimators(shockwave::World& world, const std::string& scene, std::vector<std::string>& problems) {
    world.each<const shockwave::Animator>([&](shockwave::Entity e, const shockwave::Animator&) {
        const auto* name = world.get<shockwave::Name>(e);
        const std::string where = scene + ": entity '" + (name ? name->value : "?") + "': ";
        for (const auto& problem : shockwave::validateAnimator(world, e, engine().assets())) {
            if (problem.warning)
                std::printf("warning %s%s\n", where.c_str(), problem.message.c_str());
            else
                problems.push_back(where + problem.message);
        }
    });
}

}   // namespace player
