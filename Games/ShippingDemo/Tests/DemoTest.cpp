// Reactor Run's game rules (S07) on its real scenes, with real Jolt and no window:
//   - every scene's content passes the game's own checks, and broken copies are refused
//     with a reason that names the problem (and "gameplay content");
//   - input injected at the device boundary moves the player through the bindings, a
//     rebinding is followed, and prompts name what is bound;
//   - shooting, damage, reload, dry fire, pickups (only when needed, gone after);
//   - objectives in order, a gate opening, the Hold timer counting fixed ticks the same
//     at 30, 60 and 120 ticks a second;
//   - a death on the tick the last objective would complete wins (no victory), and a
//     campaign completion is only ever granted once.
// The whole game, menus and travel included, is the flow tests' (Tests/flow/*.json).
#include "DemoComponents.h"
#include "DemoGame.h"
#include "DemoRules.h"

#include <Assets/Assets.h>
#include <Audio/Audio.h>
#include <Circuits/Circuit.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Levels.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <GameUI/GameUI.h>
#include <Input/Input.h>
#include <Physics/Physics.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace shockwave;
using namespace demo;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #x);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

fs::path game;
Assets* assets = nullptr;

bool load(const char* scene, World& out) {
    std::string error;
    std::vector<std::string> warnings;   // meshes and models: there's no renderer
    if (!loadScene(out, *assets, (game / "Scenes" / scene).string(), error, {}, &warnings)) {
        std::fprintf(stderr, "%s: %s\n", scene, error.c_str());
        return false;
    }
    return true;
}

Entity named(World& world, const std::string& name) {
    Entity found = kNullEntity;
    world.each<const Name>([&](Entity e, const Name& n) {
        if (n.value == name && !found.valid()) found = e;
    });
    return found;
}

bool mentions(const std::vector<std::string>& problems, const std::string& text) {
    for (const std::string& p : problems)
        if (p.find(text) != std::string::npos) return true;
    return false;
}

struct Instance final : GameInstance {
    std::unique_ptr<GameMode> createGameMode(const World&) override { return createGame(); }
};

// ---- Content ------------------------------------------------------------------------------

void content() {
    for (const char* scene : {"menu.swscene", "training.swscene", "factory.swscene", "ending.swscene"}) {
        World world;
        CHECK(load(scene, world));
        const auto problems = DemoRules::validate(world);
        for (const std::string& p : problems) std::fprintf(stderr, "%s: %s\n", scene, p.c_str());
        CHECK(problems.empty());
    }
    World world;
    CHECK(load("training.swscene", world));
    CHECK(DemoRules::isGameplayLevel(world));
    // Break it one way at a time.
    auto& targets = *world.get<ObjectiveProgress>(named(world, "Objective: targets"));
    targets.group = "nothing";
    world.get<InteractionTarget>(named(world, "Training ammo"))->item = "gold";
    world.get<DemoGate>(named(world, "Training gate"))->objective = "training.nowhere";
    world.get<ObjectiveProgress>(named(world, "Objective: ammo"))->id = "training.line";
    world.get<ObjectiveProgress>(named(world, "Objective: gate"))->target = named(world, "Floor");
    const auto problems = DemoRules::validate(world);
    CHECK(mentions(problems, "counts group \"nothing\""));
    CHECK(mentions(problems, "gives \"gold\""));
    CHECK(mentions(problems, "\"training.nowhere\", which isn't in the level"));
    CHECK(mentions(problems, "\"training.line\" is there twice"));
    CHECK(mentions(problems, "which isn't a Use InteractionTarget"));
    // The GameMode refuses to start such a level, and says whose problem it is.
    JobSystem jobs(2);
    Instance instance;
    PlaySession session(world, jobs, instance);
    bool threw = false;
    try {
        session.start();
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("gameplay content") != std::string::npos;
    }
    CHECK(threw);
    CHECK(DemoRules::validate(World{}).empty());   // the menu has nothing to check
}

// ---- Input --------------------------------------------------------------------------------

// One engine frame without an engine: the devices, then input, a tick, the frame.
void frame(PlaySession& session, Input& input) {
    input.beginFrame();
    input.update(1.0 / 60);
    session.collectInput(1.0 / 60);
    session.update(1.0 / 60);
    session.frame(1.0 / 60);
}

void input() {
    World world;
    CHECK(load("training.swscene", world));
    JobSystem jobs(2);
    Instance instance;
    PlaySession session(world, jobs, instance);
    session.start();
    Input& in = const_cast<Input&>(session.context().input());
    in.setBindings({});
    in.clearBindingsFile();
    InputBindings bindings;
    std::string error;
    CHECK(loadInputBindings(game / "Config" / "Input.swinput", bindings, error));
    in.setBindings(bindings);
    const Entity player = named(world, "Player");
    const float startZ = world.get<Transform>(player)->position.z;
    InputBinding w{.source = InputBinding::Source::Key, .code = uint16_t(Key::W)};
    CHECK(in.inject(w, 1));
    for (int i = 0; i < 30; ++i) frame(session, in);
    const float walkedZ = world.get<Transform>(player)->position.z;
    CHECK(walkedZ < startZ - 1.0f);   // forward is -Z
    in.inject(w, 0);
    for (int i = 0; i < 10; ++i) frame(session, in);
    // The prompt follows the bindings and the device.
    CHECK(bindingLabel(in, kUse, false) == "E");
    CHECK(bindingLabel(in, kFire, false) == "Left mouse");
    CHECK(bindingLabel(in, kFire, true) == "Right trigger");
    InputBindings rebound = bindings;
    for (auto& [action, binding] : rebound.binds)
        if (action == kForward && binding.source == InputBinding::Source::Key)
            binding.code = uint16_t(Key::Up);
        else if (action == kUse && binding.source == InputBinding::Source::Key)
            binding.code = uint16_t(Key::F);
    in.setBindings(rebound);
    CHECK(bindingLabel(in, kUse, false) == "F");
    const float beforeZ = world.get<Transform>(player)->position.z;
    in.inject(w, 1);   // W isn't forward any more
    for (int i = 0; i < 20; ++i) frame(session, in);
    in.inject(w, 0);
    CHECK(std::abs(world.get<Transform>(player)->position.z - beforeZ) < 0.05f);
    InputBinding up{.source = InputBinding::Source::Key, .code = uint16_t(Key::Up)};
    in.inject(up, 1);
    for (int i = 0; i < 20; ++i) frame(session, in);
    in.inject(up, 0);
    CHECK(world.get<Transform>(player)->position.z < beforeZ - 0.5f);
    // A virtual gamepad: the stick walks, and prompts show its buttons.
    CHECK(!in.hasVirtualGamepad());
    InputBinding stick{
        .source = InputBinding::Source::GamepadAxis,
        .code = uint16_t(GamepadAxis::LeftY),
        .range = InputBinding::Range::Positive};
    const float padZ = world.get<Transform>(player)->position.z;
    in.inject(stick, 1);
    for (int i = 0; i < 20; ++i) frame(session, in);
    CHECK(in.hasVirtualGamepad());
    CHECK(world.get<Transform>(player)->position.z < padZ - 0.5f);
    CHECK(bindingLabel(in, kUse, true) == "A");   // the virtual pad reports itself as an Xbox pad
    // Unplugged mid-push: it lets go, the player stops.
    in.removeVirtualGamepad();
    for (int i = 0; i < 20; ++i) frame(session, in);
    const float stoppedZ = world.get<Transform>(player)->position.z;
    for (int i = 0; i < 10; ++i) frame(session, in);
    CHECK(!in.hasVirtualGamepad());
    CHECK(std::abs(world.get<Transform>(player)->position.z - stoppedZ) < 0.05f);
    session.stop();
}

// ---- Rules --------------------------------------------------------------------------------

struct Rig {
    World world;
    JobSystem jobs{2};
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<DemoRules> rules;
    float rate = 60;
    explicit Rig(const char* scene, float rate = 60) {
        CHECK(load(scene, world));
        prepare(rate);
    }
    Rig() = default;
    void prepare(float tickRate = 60) {
        rate = tickRate;
        physics = std::make_unique<PhysicsWorld>(world, jobs);
        physics->synchronize();
        rules = std::make_unique<DemoRules>(world, physics.get(), rate);
        rules->scan();
    }
    TickEvents tick(const PlayerCommand& c = {}) {
        world.each<DemoEnemy, const Transform, const Health>(
            [&](Entity e, DemoEnemy& ai, const Transform& at, const Health& h) { rules->think(e, ai, at, h); });
        physics->synchronize();
        physics->step(1.0 / rate);
        return rules->tick(c);
    }
    void place(glm::vec3 at) {
        world.get<Transform>(rules->player())->position = at;
        physics->synchronize();
        CharacterMotionState state{at, glm::vec3(0), true};
        physics->correctCharacter(rules->player(), state);
    }
};

void combat() {
    Rig rig("training.swscene");
    const Entity player = rig.rules->player();
    rig.place({0, 0, -6});
    auto& gun = *rig.world.get<Loadout>(player);
    CHECK(gun.ammo == 6 && gun.reserve == 6);
    // The centre target is straight ahead: two hits destroy it.
    PlayerCommand fire;
    fire.fire = true;
    int shots = 0, hits = 0, ticks = 0;
    while (rig.rules->alive("targets") == 3 && ticks++ < 600) {
        const TickEvents e = rig.tick(fire);
        shots += e.shots;
        hits += e.hit;
    }
    CHECK(rig.rules->alive("targets") == 2);
    CHECK(!named(rig.world, "Target centre").valid());   // destroyed with its parts
    CHECK(!named(rig.world, "Plate").valid() || rig.world.alive(named(rig.world, "Plate")));
    CHECK(shots == 2 && hits == 2);
    CHECK(gun.ammo == 4 && gun.shots == 2 && gun.hits == 2);
    // Fire into the wall until the magazine is empty: it reloads by itself, in ticks.
    PlayerCommand away = fire;
    away.yaw = glm::radians(90.0f);
    for (int i = 0; i < 600 && gun.ammo > 0; ++i) rig.tick(away);
    CHECK(gun.ammo == 0);
    bool started = false, reloaded = false;
    int reloadTicks = 0;
    for (int i = 0; i < 200 && !reloaded; ++i) {
        const TickEvents e = rig.tick(away);
        started |= e.reloadStarted;
        if (started) ++reloadTicks;
        reloaded = e.reloaded;
    }
    CHECK(started && reloaded);
    CHECK(reloadTicks == 73);   // the tick it starts, then ceil(1.2 s * 60) more
    CHECK(gun.reserve == 0 && gun.ammo >= 5);   // still firing: the reloaded tick shoots again
    for (int i = 0; i < 600 && gun.ammo > 0; ++i) rig.tick(away);
    bool dry = false;
    for (int i = 0; i < 30; ++i) dry |= rig.tick(away).dryFire;
    CHECK(dry && gun.ammo == 0 && gun.reserve == 0);
    // Ammo: walking over it fills the reserve, once; the box is gone.
    const Entity box = named(rig.world, "Training ammo");
    rig.place({0, 0, -10});
    const TickEvents picked = rig.tick();
    CHECK(picked.pickups.size() == 1 && picked.pickups[0] == "ammo");
    CHECK(gun.reserve == 24);
    CHECK(!rig.world.alive(box));
    for (int i = 0; i < 5; ++i) CHECK(rig.tick().pickups.empty());
}

void pickupsWhenNeeded() {
    Rig rig("factory.swscene");
    const Entity player = rig.rules->player();
    const Entity medkit = named(rig.world, "Factory health");
    CHECK(medkit.valid());
    rig.world.each<DemoEnemy>([](Entity, DemoEnemy& ai) { ai.sight = 1; });   // no one shoots
    // At full health it stays where it is.
    rig.place({0, 0, -23});
    for (int i = 0; i < 5; ++i) CHECK(rig.tick().pickups.empty());
    CHECK(rig.world.alive(medkit));
    rig.world.get<Health>(player)->current = 30;
    const TickEvents e = rig.tick();
    CHECK(e.pickups.size() == 1);
    CHECK(rig.world.get<Health>(player)->current == 70);
    CHECK(!rig.world.alive(medkit));
}

// The sentries see the player, walk at them and shoot; the player takes them out; the
// security door opens; the reactor console only works once it's its turn.
void objectivesInOrder() {
    Rig rig("factory.swscene");
    const Entity player = rig.rules->player();
    CHECK(rig.world.get<ObjectiveProgress>(rig.rules->activeObjective())->id == "factory.enemies");
    // The console is out of turn: nothing to use even standing next to it.
    rig.place({0, 0, -34});
    CHECK(!rig.rules->usable().valid());
    rig.place({0, 0, 0});
    PlayerCommand fire;
    fire.fire = true;
    bool hurt = false;
    TickEvents done;
    for (int i = 0; i < 1200 && rig.rules->alive("enemies") > 0; ++i) {
        const TickEvents e = rig.tick(fire);
        hurt |= e.hurt;
        done.completed.insert(done.completed.end(), e.completed.begin(), e.completed.end());
        done.opened.insert(done.opened.end(), e.opened.begin(), e.opened.end());
    }
    CHECK(rig.rules->alive("enemies") == 0);
    CHECK(hurt);   // they fired back
    CHECK(rig.world.get<Health>(player)->current < 100);
    CHECK(rig.world.get<Health>(player)->current > 0);
    CHECK(rig.rules->progress()->kills == 2);
    // The tick the last one fell completed the objective and opened the door.
    CHECK(done.completed.size() == 1 && done.completed[0] == "factory.enemies");
    CHECK(done.opened.size() == 1);
    CHECK(rig.world.get<DemoGate>(named(rig.world, "Security door"))->open);
    CHECK(!rig.world.get<Collider>(named(rig.world, "Security door"))->enabled);
    // Now the console can be used, and Use completes its objective on the next tick.
    rig.place({0, 0, -33});
    const Entity console = named(rig.world, "Reactor console");
    CHECK(rig.rules->usable() == console);
    PlayerCommand use;
    use.use = true;
    rig.tick(use);
    CHECK(rig.world.get<InteractionTarget>(console)->used);
    done = rig.tick();
    CHECK(done.completed.size() == 1 && done.completed[0] == "factory.reactor");
    // The exit stays shut until everything is done.
    rig.place({0, 0, -53});
    CHECK(!rig.rules->usable().valid());
    rig.tick(use);
    CHECK(rig.rules->progress()->state == CampaignProgress::Playing);
}

// The Hold objective counts fixed ticks: 8 seconds is 8 seconds at any tick rate.
void holdTimer() {
    for (const float rate : {30.0f, 60.0f, 120.0f}) {
        Rig rig;
        CHECK(load("factory.swscene", rig.world));
        rig.world.each<ObjectiveProgress>([](Entity, ObjectiveProgress& o) {
            if (o.id == "factory.enemies" || o.id == "factory.reactor") o.complete = true;
        });
        rig.prepare(rate);
        rig.place({0, 0, -34});
        // Keep the guard out of it: this is about the clock.
        rig.world.each<DemoEnemy>([](Entity, DemoEnemy& ai) { ai.sight = 1; });
        int ticks = 0;
        bool complete = false;
        while (!complete && ticks < 10000) {
            ++ticks;
            for (const std::string& id : rig.tick().completed) complete |= id == "factory.hold";
        }
        CHECK(complete);
        CHECK(std::abs(float(ticks) / rate - 8.0f) <= 1.5f / rate);
    }
}

// Down beats a victory on the same tick: the Reach objective under the player's feet
// doesn't complete because a sentry's shot took them down first.
void deathBeatsVictory() {
    Rig rig("training.swscene");
    const Entity player = rig.rules->player();
    // Everything done but walking to the firing line, which the player is standing on.
    rig.world.each<ObjectiveProgress>([](Entity, ObjectiveProgress& o) { o.complete = o.id != "training.line"; });
    rig.place({0, 0, -6});
    rig.world.get<Health>(player)->current = 5;
    // A sentry in front, ready to fire.
    const Entity sentry = rig.world.create();
    rig.world.add<Transform>(sentry, Transform{{0, 0, -9}});
    DemoEnemy ai;
    ai.damage = 50;
    rig.world.add<DemoEnemy>(sentry, ai);
    rig.world.add<Health>(sentry, Health{100, 100, "enemies"});
    Collider body;
    body.halfExtents = {0.35f, 0.85f, 0.3f};
    body.offset = {0, 0.85f, 0};
    rig.world.add<Collider>(sentry, body);
    RigidBody rb;
    rb.motion = RigidBody::Motion::Kinematic;
    rb.gravityFactor = 0;
    rig.world.add<RigidBody>(sentry, rb);
    rig.physics->synchronize();
    const TickEvents e = rig.tick();
    CHECK(e.hurt && e.down);
    CHECK(e.completed.empty());
    CHECK(!rig.world.get<ObjectiveProgress>(named(rig.world, "Objective: firing line"))->complete);
    CHECK(rig.rules->progress()->state == CampaignProgress::Down);
    // Down stays down: nothing else happens, nothing completes.
    for (int i = 0; i < 10; ++i) CHECK(rig.tick().completed.empty());
}

// A campaign completion is granted once, however often the level is finished.
void rewardsOnce() {
    CampaignState campaign;
    CHECK(campaign.complete("factory"));
    CHECK(!campaign.complete("factory"));
    CHECK(campaign.completions().size() == 1 && campaign.completed("factory"));
}

}   // namespace

int main(int argc, char** argv) {
    game = argc > 1 ? fs::path(argv[1]) : fs::path("Games/ShippingDemo");
    registerTypes();
    registerAudioComponents();
    registerGameUIComponents();
    registerCircuitComponents();
    Assets ownAssets;
    assets = &ownAssets;
    prefabs().setup(game / "Content", &ownAssets);
    try {
        content();
        input();
        combat();
        pickupsWhenNeeded();
        objectivesInOrder();
        holdTimer();
        deathBeatsVictory();
        rewardsOnce();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        ++failures;
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("shipping_demo: all passed\n");
    return 0;
}
