// player --check for particle effects: each ParticleEffect a scene has, then every
// .efkefc in Content that no scene uses (game code can still play it). Problems fail the
// check; warnings (parts of an effect that aren't drawn yet) are printed and don't.
// And --effect-info: one effect's support report (A04).
#include "Player.h"

#include <Engine/Components.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace player {

namespace {

std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

}   // namespace

int effectInfo(const fs::path& effect, const fs::path& json) {
    // No project needed: the effect's folder stands in for Content, which is where
    // Effekseer finds its textures and models from.
    shockwave::Effects effects(nullptr);
    effects.setContentFolder(effect.parent_path().empty() ? fs::path(".") : effect.parent_path());
    const shockwave::EffectInfo info = effects.info(effect.filename().generic_string());
    std::string out =
        "{\"effect\": " + quote(effect.generic_string()) + ", \"loaded\": " + (info.loaded ? "true" : "false");
    char duration[32];
    std::snprintf(duration, sizeof(duration), "%.6g", info.duration);
    out += ", \"duration\": " + std::string(duration) + ", \"nodes\": " + std::to_string(info.nodes) +
           ", \"textures\": " + std::to_string(info.textures) + ", \"models\": " + std::to_string(info.models) +
           ", \"materials\": " + std::to_string(info.materials) + ", \"sounds\": " + std::to_string(info.sounds);
    out += ",\n \"problems\": [";
    for (size_t i = 0; i < info.problems.size(); ++i)
        out += std::string(i ? "," : "") + "\n  {\"message\": " + quote(info.problems[i].message) +
               ", \"warning\": " + (info.problems[i].warning ? "true" : "false") + "}";
    out += info.problems.empty() ? "]" : "\n ]";
    out += ",\n \"features\": [";
    for (size_t i = 0; i < info.features.size(); ++i) {
        const shockwave::EffectFeature& f = info.features[i];
        out += std::string(i ? "," : "") + "\n  {\"node\": " + quote(f.node) + ", \"feature\": " + quote(f.feature) +
               ", \"status\": " + quote(f.status) + ", \"detail\": " + quote(f.detail) +
               ", \"setting\": " + quote(f.setting) + "}";
    }
    out += info.features.empty() ? "]}\n" : "\n ]}\n";
    if (json.empty()) {
        // Readable on the terminal; --json for the full report.
        std::printf("%s: %s\n", effect.generic_string().c_str(), info.loaded ? "loaded" : "didn't load");
        for (const auto& p : info.problems)
            std::printf("  %s %s\n", p.warning ? "warning" : "problem", p.message.c_str());
        for (const auto& f : info.features)
            std::printf(
                "  node %-6s %-13s %s%s%s%s\n", f.node.c_str(), f.status.c_str(), f.feature.c_str(),
                f.detail.empty() ? "" : ": ", f.detail.c_str(),
                f.setting.empty() ? "" : (" [" + f.setting + "]").c_str());
    } else {
        std::ofstream file(json);
        file << out;
        file.close();
        if (!file) throw std::runtime_error("can't write " + json.string());
    }
    const bool problem = !info.loaded || std::any_of(info.problems.begin(), info.problems.end(), [](const auto& p) {
        return !p.warning;
    });
    return problem ? 1 : 0;
}

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
