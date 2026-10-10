#include <GameFramework/Gameplay.h>
#include <Engine/Components.h>
#include <cmath>
#include <cstdio>

namespace shockwave {
namespace {
struct ColorPulse {
    float speed = 1.5f, strength = 0.15f;
    float time = 0;
    glm::vec4 base{1};
    bool initialized = false;
};
void registerTypes() {
    registerComponent<ColorPulse>("ColorPulse", {
        field("speed", &ColorPulse::speed, 0.1f, 0.0f, 10.0f),
        field("strength", &ColorPulse::strength, 0.01f, 0.0f, 1.0f)});
}
class SampleGameplay final : public Gameplay {
public:
    void onStart(GameplayContext& ctx) override {
        // Give the sample Crate a game-owned behavior only during Play.
        ctx.world.each<const Name>([&](Entity e, const Name& name) {
            if (name.value == "Crate" && ctx.world.has<MeshRenderer>(e) && !ctx.world.has<ColorPulse>(e))
                ctx.world.add<ColorPulse>(e);
        });
        std::printf("Sample gameplay started (%zu entities)\n", ctx.world.entityCount());
    }
    void configureSystems(GameplayContext&, Schedule& schedule) override {
        schedule.each<MeshRenderer, ColorPulse>("ColorPulse", [](const SystemContext& ctx, MeshRenderer& mesh, ColorPulse& pulse) {
            if (!pulse.initialized) { pulse.base = mesh.tint; pulse.initialized = true; }
            pulse.time += ctx.dt;
            const float value = 1.0f - pulse.strength * (0.5f + 0.5f * std::sin(pulse.time * pulse.speed));
            mesh.tint = glm::vec4(glm::vec3(pulse.base) * value, pulse.base.a);
        });
    }
    void onStop(GameplayContext&) override { std::puts("Sample gameplay stopped"); }
};
}
const GameModule& projectGameModule() {
    static const GameModule module{"sample", [] { return std::make_unique<SampleGameplay>(); }, registerTypes, {}};
    return module;
}
}
