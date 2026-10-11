// The sample project's game code: a ColorPulse component and a GameMode that gives the
// Crate one during Play, so you can see gameplay code running in the editor. Pressing
// "pulse" (Space, or South on a gamepad: Cross / A) flashes the Crate and rumbles the
// pad; the binding lives in Config/Input.swinput. The HUD (Content/UI/hud.rml) counts the
// pulses and has a Pulse button that does the same. "menu" (P, or Start) opens a pause
// menu (Content/UI/menu.rml) you can drive with the keyboard or a gamepad, with a list of
// recent pulses filled from here. Patrol walks NavAgents between waypoints with
// game.navigation().

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

// Walks a NavAgent round up to four waypoints in order, pausing at each
// (Scenes/navigation.swscene). The waypoints are entities, so you can move them in the
// editor.
struct Patrol {
    Entity first, second, third, fourth;
    float wait = 1.0f;   // seconds at each waypoint
    int next = 0;   // runtime: the waypoint it's walking to
    float waited = 0;
};
SW_COMPONENT(Patrol, SW_FIELD(first), SW_FIELD(second), SW_FIELD(third), SW_FIELD(fourth), SW_FIELD(wait, 0, 60))

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
        // The HUD's {{pulses}}, and its Pulse button (onclick="pulse").
        m_pulses = 0;
        m_time = 0.0;
        m_log = UIValue::list();
        game.ui().set("pulses", m_pulses);
        game.ui().set("log", m_log);
        game.ui().on("pulse", [this, &game](const UIEvent&) { pulse(game); });
        // The pause menu's buttons, and "back" (Escape, or East on a gamepad), which sends
        // the menu's data-back="resume".
        game.ui().on("resume", [&game](const UIEvent&) { game.ui().hide("PauseMenu"); });
        game.ui().on("reset", [this, &game](const UIEvent&) {
            m_pulses = 0;
            m_log = UIValue::list();
            game.ui().set("pulses", m_pulses);
            game.ui().set("log", m_log);
        });
        SW_LOG(LogGame, Info, "Sample gameplay started ({} entities)", game.world.entityCount());
    }

    // Every fixed tick, after the agents have moved: send each patrolling agent on to its
    // next waypoint once it has arrived and waited.
    void fixedUpdate(GameContext& game, double dt) override {
        game.world.each<Patrol, NavAgent>([&](Entity e, Patrol& patrol, NavAgent& agent) {
            const Entity points[] = {patrol.first, patrol.second, patrol.third, patrol.fourth};
            if (agent.hasDestination && !agent.arrived) return;
            if (agent.arrived) {
                patrol.waited += static_cast<float>(dt);
                if (patrol.waited < patrol.wait) return;
                patrol.next = (patrol.next + 1) % 4;
            }
            patrol.waited = 0;
            for (int tries = 0; tries < 4; ++tries, patrol.next = (patrol.next + 1) % 4) {
                if (!points[patrol.next].valid() || !game.world.has<Transform>(points[patrol.next])) continue;
                game.navigation().setDestination(e, glm::vec3(worldMatrix(game.world, points[patrol.next])[3]));
                return;
            }
        });
    }

    // Once per frame on the main thread.
    void frame(GameContext& game, double dt) override {
        m_time += dt;
        if (game.input().wasActionPressed("menu")) game.ui().toggle("PauseMenu");
        if (game.input().wasActionPressed("pulse") && !game.ui().capturesInput()) pulse(game);
    }

    void endPlay(GameContext&) override { SW_LOG(LogGame, Info, "Sample gameplay stopped"); }

private:
    void pulse(GameContext& game) {
        game.world.each<ColorPulse>([](ColorPulse& pulse) { pulse.flash = 1.0f; });
        game.input().rumble(0.3f, 0.8f, 0.2);
        game.audio().playClip("Audio/pulse.wav", {.volume = 0.6f});
        game.ui().set("pulses", ++m_pulses);
        // The menu lists the last five pulses: data-for="entry : log" in menu.rml.
        m_log.push(UIValue::record({{"number", m_pulses}, {"time", m_time}}));
        if (m_log.size() > 5) {
            UIValue recent = UIValue::list();
            for (size_t i = m_log.size() - 5; i < m_log.size(); ++i) recent.push(m_log[i]);
            m_log = std::move(recent);
        }
        game.ui().set("log", m_log);
    }

    int m_pulses = 0;
    double m_time = 0.0;
    UIValue m_log;
};

SW_GAME("sample", SampleGame)
