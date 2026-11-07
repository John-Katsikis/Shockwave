#pragma once

// GameMode: the rules of one Play session of a scene, as in Unreal. It adds the game's
// systems to the schedule, and gets BeginPlay/EndPlay and per-tick hooks. A fresh
// GameMode is made for every Play, by the project's GameInstance.

#include <Engine/EntityRef.h>
#include <Engine/Schedule.h>
#include <Engine/Subsystem.h>
#include <Engine/Levels.h>
#include <GameFramework/GameFlow.h>
#include <GameFramework/LevelLifetime.h>

#include <glm/vec3.hpp>

#include <functional>
#include <string_view>

namespace shockwave {

class Engine;
class Graphics;
class SaveGames;
class CampaignState;
class Input;
class PhysicsWorld;
class AudioWorld;
class EffectsWorld;
class AnimationWorld;
class UIWorld;
class NavigationWorld;
namespace net {
class NetworkWorld;
class OnlineMatch;
}   // namespace net

// What game code can reach during Play.
struct GameContext {
    World& world;
    JobSystem& jobs;
    Engine* engine;   // null only in tools and tests that run without a window
    SubsystemCollection& subsystems;   // this session's: physics, and any the game adds

    template <class T>
    T* subsystem() const {
        return subsystems.find<T>();
    }
    Graphics& graphics() const;   // player quality and pacing; main thread, see docs/Graphics.md
    // This frame's camera jumped (a teleport, a respawn, a cut between shots): temporal
    // anti-aliasing starts afresh instead of smearing the old view into the new one
    // (docs/TemporalAntiAliasing.md). Main thread; does nothing without a window.
    void cutCamera() const;
    AudioWorld& audio() const;   // always there during Play
    // Particle effects (docs/Effects.md). Always there during Play; without a window it
    // does nothing, so game code doesn't need to check.
    EffectsWorld& effects() const;
    // Skeletal animation (docs/Animation.md): switch clips, find joints. Always there
    // during Play.
    AnimationWorld& animation() const;
    PhysicsWorld& physics() const;   // always there during Play
    // Menus and HUDs (docs/GameUI.md). Always there during Play; without a window it
    // does nothing, so game code doesn't need to check.
    UIWorld& ui() const;
    NavigationWorld& navigation() const;   // always there during Play; paths and NavAgents
    // The engine's input. In tools and tests without a window it's one where nothing is
    // ever pressed, so game code doesn't need to check.
    Input& input() const;
    // The first entity with this Name, or an empty EntityRef (false) when there's none.
    EntityRef find(std::string_view name) const { return findEntity(world, name); }
    // A new copy of a prefab ("Prefabs/Enemy.swprefab", see docs/Prefabs.md), where the
    // prefab puts it or at position. An empty EntityRef (false) if it can't be loaded.
    EntityRef spawn(std::string_view prefab) const;
    EntityRef spawn(std::string_view prefab, const glm::vec3& position) const;
    // Saving and loading the game (docs/SaveGames.md). Always there during Play.
    SaveGames& saves() const;
    // Progress that belongs to the whole game and survives travel: unlocked levels, keys,
    // the story so far (docs/SaveGames.md#campaign). Saved with every save. Always there
    // during Play.
    CampaignState& campaign() const { return *campaignState; }
    // Network play (docs/Networking.md). Always there during Play: offline it reports
    // local authority (role Offline) and does nothing.
    net::NetworkWorld& network() const;
    // Hosting, finding, joining, inviting and leaving matches (Multiplayer/OnlineMatch.h):
    // the application's, shared with its command line and editor. Null when the
    // application offers none (tools, tests, a project without a network scene); a game's
    // menus grey their online buttons then. Hosting or joining replaces the session, so
    // call it from menu handlers, never mid-tick.
    net::OnlineMatch* onlineMatch = nullptr;

    // ---- Levels (docs/LevelLifetime.md) ----
    // Why this session started, and which runtime level it is (0 outside level travel).
    StartReason startReason = StartReason::Fresh;
    uint64_t levelEpoch = 0;
    // Travel (S05, GameFramework/GameFlow.h): request, restart, menu, exits, tickets and
    // their progress. Never immediate: the level ends after this frame's callbacks have
    // returned, so it's safe from fixedUpdate, frame, UI handlers and beginPlay. Requests
    // carry this level's epoch, so one kept from an earlier level is refused.
    LevelFlow flow() const { return LevelFlow(flowHost, levelEpoch); }
    // The short form: asks to go to another level of the catalog, arriving at `spawn` (or
    // its default). False when it's refused (game.flow().request says why).
    bool requestLevel(std::string level, std::string spawn = {}) const {
        return bool(flow().request(std::move(level), std::move(spawn)));
    }
    // Leaves this level for good, to `next` (docs/SaveGames.md#finishing-a-level): travels
    // like requestLevel, and once accepted writes the "autosave" slot with the campaign and
    // the next level, so Continue after a crash resumes there without granting rewards
    // twice. Record completions (game.campaign().complete) before calling it.
    TravelTicket finishLevel(std::string next, std::string spawn = {}) const;
    static constexpr const char* kAutosaveSlot = "autosave";
    GameFlow* flowHost = nullptr;   // set by LevelHost; null: nothing can travel
    // Where this level was entered: the spawn asked for (or the catalog's default) and its
    // place. Place the player there in beginPlay; found is false when there's no spawn.
    LevelArrival arrival;
    CampaignState* campaignState = nullptr;   // set by PlaySession: LevelHost's, or its own
};

class GameMode {
public:
    virtual ~GameMode() = default;
    // Add the game's systems; they run in parallel by declared access (see Schedule).
    virtual void configureSystems(GameContext&, Schedule&) {}
    // The scene is loaded and physics exists; spawn things, add subsystems.
    virtual void beginPlay(GameContext&) {}
    // Main thread, once per frame before that frame's fixed ticks: read the devices and
    // latch this frame's local actions, so ticks consume them rather than polling input.
    // Runs while Play runs, whatever the window or menus are doing; during network play
    // check game.network().localInputSuppressed() and send neutral input instead.
    virtual void collectInput(GameContext&, double frameDt) { (void)frameDt; }
    // Main thread, every fixed tick after the systems and the physics step.
    virtual void fixedUpdate(GameContext&, double dt) { (void)dt; }
    // Main thread, once per rendered frame with the real frame time.
    virtual void frame(GameContext&, double frameDt) { (void)frameDt; }
    // Play is stopping, or the level is ending for another: the authored scene is
    // restored right after (editor Play), or the level's world is discarded (travel).
    virtual void endPlay(GameContext&) {}
    // Main thread, just before a save game snapshots the world: put state that isn't in
    // components into game.saves().set(...).
    virtual void beforeSave(GameContext&) {}
    // Main thread, right after a save game replaced the world (physics is rebuilt
    // already): read back game.saves() values, refresh UI.
    virtual void afterLoad(GameContext&) {}
};

}   // namespace shockwave
