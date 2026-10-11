// player --check for particle effects: each ParticleEffect a scene has, then every
// .efkefc in Content that no scene uses (game code can still play it). Problems fail the
// check; warnings (parts of an effect that aren't drawn yet) are printed and don't.
#include "Player.h"

#include <Engine/Components.h>

#include <algorithm>
#include <cstdio>

namespace player {

void Player::checkParticleEffects(
    shockwave::World& world, const std::string& scene, std::vector<std::string>& problems,
    std::vector<std::string>& checkedEffects) {
    const std::string shownScene = m_project.relative(scene);
    world.each<const shockwave::ParticleEffect>([&](shockwave::Entity e, const shockwave::ParticleEffect& effect) {
        const shockwave::Name* name = world.get<shockwave::Name>(e);
        const std::string where = shownScene + ": entity '" + (name ? name->value : "?") + "': ParticleEffect";
        if (!world.has<shockwave::Transform>(e)) problems.push_back(where + " needs a Transform to be placed");
        if (effect.scale <= 0.0f) problems.push_back(where + " scale must be above 0");
        if (effect.speed < 0.0f) problems.push_back(where + " speed can't be negative");
        if (effect.effect.empty()) {
            problems.push_back(where + " has no effect file");
            return;
        }
        checkedEffects.push_back(effect.effect);
        for (const auto& problem : m_effects->validate(effect.effect)) {
            if (problem.warning)
                std::printf("warning %s %s: %s\n", where.c_str(), effect.effect.c_str(), problem.message.c_str());
            else
                problems.push_back(where + " " + effect.effect + ": " + problem.message);
        }
    });
}

void Player::checkUnusedEffects(const std::vector<std::string>& checkedEffects, std::vector<std::string>& problems) {
    for (const std::string& effect : m_effects->effectFiles()) {
        if (std::find(checkedEffects.begin(), checkedEffects.end(), effect) != checkedEffects.end()) continue;
        const std::string shown = m_project.relative(m_project.assetFolder / effect);
        bool ok = true;
        for (const auto& problem : m_effects->validate(effect)) {
            if (problem.warning) {
                std::printf("warning %s: %s\n", shown.c_str(), problem.message.c_str());
            } else {
                problems.push_back(shown + ": " + problem.message);
                ok = false;
            }
        }
        if (ok) std::printf("ok %s\n", shown.c_str());
    }
}

}   // namespace player
