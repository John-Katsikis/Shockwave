// The sample project's game code: a ColorPulse component and a GameMode that gives the
// Crate one during Play, so you can see gameplay code running in the editor. Pressing
// "pulse" (Space, or South on a gamepad: Cross / A) flashes the Crate and rumbles the
// pad; the binding lives in Config/Input.swinput.

#include <Shockwave/Game.h>

using namespace shockwave;

// Scales an entity's tint up and down over time. speed and strength are saved in scenes
// and shown in the Inspector; the rest is runtime state.
struct ColorPulse {
    float speed = 1.5f;
    float strength = 0.15f;
    glm::vec4 base{1};   // the tint before pulsing
    float time = 0;
    float flash = 0;   // 1 right after "pulse" is pressed, fading to 0
};
SW_COMPONENT(ColorPulse, SW_FIELD(speed, 0, 10), SW_FIELD(strength, 0, 1))

class SampleGame final : public GameMode {
public:
    // Runs every tick for each entity with both components, in parallel with other systems.
    void configureSystems(GameContext&, Schedule& schedule) override {
        schedule.each<MeshRenderer, ColorPulse>(
            "ColorPulse", [](const SystemContext& ctx, MeshRenderer& mesh, ColorPulse& pulse) {
                pulse.time += ctx.dt;
                pulse.flash = std::max(0.0f, pulse.flash - ctx.dt * 3.0f);
                const float wave = 0.5f + 0.5f * std::sin(pulse.time * pulse.speed);
                const float value = 1.0f - pulse.strength * wave + pulse.flash;
                mesh.tint = glm::vec4(glm::vec3(pulse.base) * value, pulse.base.a);
            });
    }

    // Give the Crate a game-owned behavior only during Play.
    void beginPlay(GameContext& game) override {
        if (EntityRef crate = game.find("Crate"); crate.has<MeshRenderer>())
            crate.add(ColorPulse{.base = crate.get<MeshRenderer>().tint});
        SW_LOG(LogGame, Info, "Sample gameplay started ({} entities)", game.world.entityCount());
    }

    // Once per frame on the main thread.
    void frame(GameContext& game, double) override {
        if (!game.input().wasActionPressed("pulse")) return;
        game.world.each<ColorPulse>([](ColorPulse& pulse) { pulse.flash = 1.0f; });
        game.input().rumble(0.3f, 0.8f, 0.2);
        game.audio().playClip("Audio/pulse.wav", {.volume = 0.6f});
    }

    void endPlay(GameContext&) override { SW_LOG(LogGame, Info, "Sample gameplay stopped"); }
};

SW_GAME("sample", SampleGame)
