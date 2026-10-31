#pragma once

// Particle effects: fire, smoke, sparks, magic, explosions, made in the free Effekseer
// editor and saved as .efkefc files in the project's Content folder. Effekseer
// simulates them; the result is plain triangles that any renderer plugin draws into the
// HDR scene (Renderer/Particles.h). See docs/Effects.md.
//
// A scene shows an effect with a ParticleEffect component on an entity with a Transform.
// Game code plays one-shots and controls running effects through game.effects()
// (EffectsWorld.h). The editor and the player each own one Effects and call update() and
// render() once per frame.
//
// Threading: everything here is main-thread only. The simulation itself runs on a job
// system worker between update() and render(), overlapping the rest of the frame.

#include <Engine/World.h>
#include <Renderer/Particles.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class Engine;
class Renderer;

// Plays an effect file at the entity's Transform, following it as it moves.
struct ParticleEffect {
    std::string effect;   // .efkefc path inside Content, like "Effects/laser.efkefc"
    bool playing = true;   // runs while on; turning it off lets live particles finish
    bool loop = true;   // starts again each time it ends
    float speed = 1.0f;   // 1 is the speed it was made at; 0 freezes it
    float scale = 1.0f;   // on top of the Transform's scale
    glm::vec4 color{1.0f};   // multiplies every particle's color and alpha
    // Soft particles (A04): how far, in metres, particles fade in behind them to the scene
    // surface they meet. Negative keeps what the effect was made with (Effekseer's
    // "soft particle" distance); 0 turns the fade off for this effect.
    float softDistance = -1.0f;
    // How it's ordered among other effects' see-through particles, back to front:
    // Whole draws the whole effect at the middle of its particles, keeping the order it
    // was made with; Parts sorts each part (each node) on its own, for big effects like a
    // smoke plume that other effects sit inside of.
    enum class Sorting { Whole, Parts };
    Sorting sorting = Sorting::Whole;
};
// Registers ParticleEffect for scenes, the Inspector and player --schema. The hosts call
// it at startup, like registerAudioComponents.
void registerEffectsComponents();

// What's wrong with an effect, or what it uses that Shockwave doesn't draw yet.
struct EffectProblem {
    std::string file;   // inside Content
    std::string message;
    bool warning = false;   // it still plays; something in it is skipped
};

// One thing an effect's node uses, and what Shockwave draws for it (A04): the effect's
// support report, for player --effect-info, --check and the editor.
struct EffectFeature {
    std::string node;   // where in the effect: child indices from the root, "1/2"
    std::string feature;   // "sprite", "soft particles", "distortion", "falloff", ...
    // "supported": drawn as made; "approximated": drawn, close but not exactly (detail
    // says how); "unsupported": that part isn't drawn, or the feature is left out.
    std::string status;
    std::string detail;
    std::string setting;   // the graphics setting that turns it off, if any ("soft_particles")
};
struct EffectInfo {
    std::string file;   // inside Content
    bool loaded = false;
    std::vector<EffectProblem> problems;   // as validate() reports them
    float duration = -1.0f;   // seconds; negative: never ends on its own
    uint32_t nodes = 0;   // nodes that draw something
    uint32_t textures = 0, models = 0, materials = 0, sounds = 0;
    std::vector<EffectFeature> features;
};

// Per-frame cost and size, for the Stats window and tools.
struct EffectsStats {
    uint32_t effectsLoaded = 0;   // effect files in memory
    uint32_t playing = 0;   // running effect instances (an entity's effect, or a one-shot)
    uint32_t particles = 0;   // live particles (Effekseer instances)
    uint32_t batches = 0;   // draws handed to the renderer last frame
    uint32_t vertices = 0;
    uint32_t skippedBatches = 0;   // custom materials, not drawn yet
    float updateMs = 0;   // simulation, on a worker
    float buildMs = 0;   // building the triangles, on the main thread
    float waitMs = 0;   // the main thread waiting for the simulation to finish, before the build
};

class Effects {
public:
    // The most particles alive at once, across every effect. More are not spawned.
    static constexpr int kMaxParticles = 20000;

    // engine: where to draw; null for tools and tests (effects still load, simulate and
    // build triangles; textures are all white).
    explicit Effects(Engine* engine);
    ~Effects();
    Effects(const Effects&) = delete;
    Effects& operator=(const Effects&) = delete;

    // The Effects an engine's host made, for game.effects(); null if there's none.
    static Effects* forEngine(const Engine* engine);

    // The folder effect files are read from: the project's Content. Changing it stops and
    // unloads everything.
    void setContentFolder(const std::filesystem::path& folder);
    const std::filesystem::path& contentFolder() const;

    // Once per frame: starts, moves and stops effects to match the world's
    // ParticleEffects, reloads changed files, and starts this frame's simulation (dt
    // seconds; 0 holds everything still).
    void update(World& world, double dt);
    // Builds this frame's triangles from the camera set on the renderer and draws them
    // (from Application::onRender, after setCamera).
    void render(Renderer& renderer);
    // Builds the triangles for a camera without drawing them. Valid until the next build
    // or render. For tests and tools.
    ParticleDrawList build(const glm::mat4& view, const glm::mat4& projection);

    // Stops every effect; the next update starts the world's ParticleEffects from their
    // first frame. The editor calls it when Play starts and stops.
    void restart();
    // Starts one entity's effect again from its first frame.
    void restart(Entity entity);
    // Live particles of one entity's effect; 0 when it isn't playing.
    uint32_t particleCount(Entity entity) const;
    // How long an effect runs, in seconds; negative when it never ends on its own.
    float duration(std::string_view effect);

    // ---- Files ----
    // Problems seen loading an effect: missing file, textures or models, and features
    // it uses that aren't drawn yet (warnings). Loads it if needed.
    std::vector<EffectProblem> validate(std::string_view effect);
    // What the effect is made of and how each part is drawn (EffectFeature). Loads it if
    // needed.
    EffectInfo info(std::string_view effect);
    // Loads an effect ahead of time, so its first play doesn't stall. False if it can't be.
    bool preload(std::string_view effect);
    // Every .efkefc in the Content folder, for the editor.
    std::vector<std::string> effectFiles() const;
    // Loads every effect again from disk.
    void reloadAll();

    // ---- Game code (EffectsWorld forwards here) ----
    using Handle = int32_t;   // -1: none
    // A one-shot at a place in the world; it's gone when it ends.
    Handle play(std::string_view effect, const glm::mat4& transform);
    bool exists(Handle handle) const;
    void stop(Handle handle, bool immediately = false);
    void stopAll();
    void setTransform(Handle handle, const glm::mat4& transform);
    void setSpeed(Handle handle, float speed);
    void setColor(Handle handle, const glm::vec4& color);
    void setPaused(Handle handle, bool paused);
    // Effekseer's dynamic inputs (0..3) and triggers (0..3), set up in the effect.
    void setInput(Handle handle, int index, float value);
    void sendTrigger(Handle handle, int index);
    // The handle of an entity's running effect, or -1.
    Handle handleOf(Entity entity) const;

    // Whether to simulate and draw at all. Off, nothing moves or shows (the editor's
    // View > Particle effects).
    void setEnabled(bool enabled);
    bool enabled() const;

    EffectsStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}   // namespace shockwave
