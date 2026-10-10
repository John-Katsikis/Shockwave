// The sample project's game module: a ColorPulse component and a GameMode that gives
// the Crate one during Play, so you can see gameplay code running in the editor.
// Pressing "pulse" (Space, or South on a gamepad: Cross / A) flashes the Crate and
// rumbles the pad; the binding lives in Config/Input.swinput.

#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <GameFramework/GameModule.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace shockwave {
namespace {

// Scales an entity's tint up and down over time.
struct ColorPulse {
    float speed = 1.5f, strength = 0.15f;
    float time = 0;
    float flash = 0;   // 1 right after "pulse" is pressed, fading to 0
    glm::vec4 base{1};
    bool initialized = false;
};

void registerTypes() {
    registerComponent<ColorPulse>(
        "ColorPulse", {
                          field("speed", &ColorPulse::speed, 0.1f, 0.0f, 10.0f),
                          field("strength", &ColorPulse::strength, 0.01f, 0.0f, 1.0f),
                      });
}

class SampleGameMode final : public GameMode {
public:
    void configureSystems(GameContext&, Schedule& schedule) override {
        schedule.each<MeshRenderer, ColorPulse>(
            "ColorPulse", [](const SystemContext& ctx, MeshRenderer& mesh, ColorPulse& pulse) {
                if (!pulse.initialized) {
                    pulse.base = mesh.tint;
                    pulse.initialized = true;
                }
                pulse.time += ctx.dt;
                pulse.flash = std::max(0.0f, pulse.flash - ctx.dt * 3.0f);
                const float value =
                    1.0f - pulse.strength * (0.5f + 0.5f * std::sin(pulse.time * pulse.speed)) + pulse.flash;
                mesh.tint = glm::vec4(glm::vec3(pulse.base) * value, pulse.base.a);
            });
    }

    // Give the sample Crate a game-owned behavior only during Play.
    void beginPlay(GameContext& ctx) override {
        ctx.world.each<const Name>([&](Entity e, const Name& name) {
            if (name.value == "Crate" && ctx.world.has<MeshRenderer>(e) && !ctx.world.has<ColorPulse>(e))
                ctx.world.add<ColorPulse>(e);
        });
        SW_LOG(LogGame, Info, "Sample gameplay started ({} entities)", ctx.world.entityCount());
    }

    // Input is read once per frame; rumble is safe from any thread.
    void frame(GameContext& ctx, double) override {
        if (!ctx.engine) return;
        Input& input = ctx.engine->input();
        if (!input.wasActionPressed("pulse")) return;
        ctx.world.each<ColorPulse>([](Entity, ColorPulse& pulse) { pulse.flash = 1.0f; });
        input.rumble(0.3f, 0.8f, 0.2);
    }

    void endPlay(GameContext&) override { SW_LOG(LogGame, Info, "Sample gameplay stopped"); }
};

}   // namespace

const GameModule& projectGameModule() {
    static const GameModule module{
        .id = "sample",
        .registerTypes = registerTypes,
        .createAssets = {},
        .createGameInstance = [] { return std::make_unique<SimpleGameInstance<SampleGameMode>>(); },
    };
    return module;
}

}   // namespace shockwave
