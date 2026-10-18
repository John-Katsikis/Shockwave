#include "ArenaGame.h"

#include <Engine/Components.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/PlaySession.h>
#include <Physics/Physics.h>

#include <cstdio>
#include <sstream>
#include <stdexcept>

using namespace shockwave;
using namespace arena;
namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
struct Instance final : GameInstance {
    std::unique_ptr<GameMode> createGameMode(const World&) override { return arena::createGame(); }
};
struct Fixture {
    World world;
    Entity rules, player, target;
    Fixture(bool cover = false, int score = 20) {
        rules = world.create();
        ArenaRules settings;
        settings.autoPlay = true;
        settings.scoreLimit = score;
        world.add<ArenaRules>(rules, settings);
        auto floor = world.create();
        world.add<Transform>(floor, {{0, -.5f, 0}});
        Collider ground;
        ground.halfExtents = {20, .5f, 20};
        world.add<Collider>(floor, ground);
        player = world.create();
        world.add<Transform>(player, {{0, .05f, 0}});
        world.add<CharacterController>(player);
        ArenaFighter human;
        human.player = true;
        human.team = 0;
        world.add<ArenaFighter>(player, human);
        ArenaWeapon gun;
        gun.magazine = 2;   // A kill takes three hits: exercises automatic reload.
        gun.reloadSeconds = .2f;
        world.add<ArenaWeapon>(player, gun);
        target = world.create();
        world.add<Transform>(target, {{0, .05f, -8}});
        ArenaFighter bot;
        bot.speed = 0;
        bot.fireInterval = 10;
        bot.damage = 1;
        world.add<ArenaFighter>(target, bot);
        Collider body;
        body.halfExtents = {.34f, .85f, .28f};
        body.offset = {0, .85f, 0};
        world.add<Collider>(target, body);
        auto camera = world.create();
        world.add<Name>(camera, {"Arena Camera"});
        world.add<Transform>(camera);
        world.add<Camera>(camera);
        if (cover) {
            auto wall = world.create();
            world.add<Transform>(wall, {{0, 2, -4}});
            Collider obstacle;
            obstacle.halfExtents = {3, 2, .5f};
            world.add<Collider>(wall, obstacle);
        }
    }
};
void ticks(PlaySession& session, int count) {
    for (int i = 0; i < count; ++i) {
        session.update(1.0 / 60);
        session.frame(1.0 / 60);
    }
}
void combat(JobSystem& jobs) {
    Fixture f;
    Instance instance;
    PlaySession session(f.world, jobs, instance);
    session.setAudioDevice(false);
    session.start();
    ticks(session, 30);
    check(f.world.get<ArenaRules>(f.rules)->blue == 0, "spawn protection prevents immediate kill");
    ticks(session, 480);
    const auto& rules = *f.world.get<ArenaRules>(f.rules);
    check(rules.shots > 0 && rules.hits >= 6, "autoplay hits enemies through Jolt raycasts");
    check(rules.blue >= 2, "enemies respawn and can be eliminated again");
    check(rules.reloads > 0, "empty magazines reload");
    check(f.world.get<ArenaFighter>(f.target)->deaths == rules.blue, "deaths and score agree");
    session.stop();
    check(f.world.get<ArenaRules>(f.rules)->blue == 0, "Stop restores authored match state");
    session.start();
    check(f.world.get<ArenaWeapon>(f.player)->ammo == 2, "new Play starts with a full magazine");
    session.stop();
}
void occlusionAndTeams(JobSystem& jobs) {
    for (bool ally : {false, true}) {
        Fixture f(!ally);
        if (ally) f.world.get<ArenaFighter>(f.target)->team = 0;
        Instance instance;
        PlaySession session(f.world, jobs, instance);
        session.setAudioDevice(false);
        session.start();
        ticks(session, 240);
        check(f.world.get<ArenaRules>(f.rules)->hits == 0, ally ? "no friendly fire" : "cover blocks hits");
        check(f.world.get<ArenaFighter>(f.target)->health == 100, "protected target remains healthy");
    }
}
void finishAndInvalidScenes(JobSystem& jobs) {
    Fixture f(false, 1);
    Instance instance;
    PlaySession session(f.world, jobs, instance);
    session.setAudioDevice(false);
    session.start();
    ticks(session, 240);
    check(f.world.get<ArenaRules>(f.rules)->finished, "score limit ends match");
    const auto shots = f.world.get<ArenaRules>(f.rules)->shots;
    ticks(session, 120);
    check(f.world.get<ArenaRules>(f.rules)->shots == shots, "finished match stops firing");
    session.stop();
    f.world.remove<CharacterController>(f.player);
    bool rejected = false;
    try {
        session.start();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected && session.state() == PlaySession::State::Editing, "invalid player rejected cleanly");
    f.world.remove<ArenaRules>(f.rules);
    rejected = false;
    try {
        session.start();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "incomplete scene cleanup tolerates missing rules");
}
void roundTrip() {
    Fixture f;
    Assets assets;
    std::stringstream text;
    writeScene(text, f.world, assets);
    World loaded;
    std::string error;
    check(readScene(loaded, assets, text, "arena-test.swscene", error), error.c_str());
    check(loaded.count<ArenaRules>() == 1 && loaded.count<ArenaFighter>() == 2, "arena components round-trip");
    loaded.each<const ArenaWeapon>([&](const ArenaWeapon& w) { check(w.magazine == 2, "weapon settings round-trip"); });
}
void firstPersonPresentation(JobSystem& jobs) {
    Fixture f;
    const Entity camera = primaryCamera(f.world);
    const Entity gun = f.world.create();
    f.world.add<Transform>(gun);
    f.world.add<Parent>(gun, {camera});
    storePreviousTransforms(f.world, jobs);
    auto& body = *f.world.get<Transform>(f.player);
    const auto initial = body.position;
    int lastTick = 0;
    // Simulate a 60 Hz body at 1,300 render frames/sec while turning the mouse.
    // The viewmodel must remain fixed in camera space across every alpha wrap.
    for (int frame = 0; frame < 130; ++frame) {
        const double ticks = double(frame) * 60 / 1300;
        const int tick = int(ticks);
        if (tick != lastTick) {
            storePreviousTransforms(f.world, jobs);
            lastTick = tick;
        }
        body.position.x = initial.x + float(tick + 1) * .1f;
        const float alpha = float(ticks - tick);
        updateView(f.world, f.player, camera, gun, frame * .004f, .2f, 0, 0, alpha);
        const auto& pose = *f.world.get<Transform>(camera);
        check(
            std::abs(pose.position.x - (initial.x + float(ticks) * .1f)) < .0001f,
            "camera follows continuous interpolated feet across tick boundaries");
        glm::mat4 view, projection;
        check(cameraMatrices(f.world, camera, 16.f / 9, view, projection), "presentation camera is valid");
        // The exact local interpolation used by renderWorld, including its parent.
        const auto interpolated = [&](Entity e) {
            return Transform::blend(f.world.get<PreviousTransform>(e)->value, *f.world.get<Transform>(e), alpha)
                .matrix();
        };
        const glm::mat4 relative = view * interpolated(camera) * interpolated(gun);
        const glm::mat4 expected = f.world.get<Transform>(gun)->matrix();
        float error = 0;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                error = std::max(error, std::abs(relative[col][row] - expected[col][row]));
        check(error < .0001f, "rifle stays locked to current camera while moving and aiming");
        check(body.position.x == initial.x + float(tick + 1) * .1f, "presentation never moves the physics body");
    }
}
}   // namespace
int main() {
    arena::registerTypes();
    JobSystem jobs(2);
    try {
        roundTrip();
        firstPersonPresentation(jobs);
        combat(jobs);
        occlusionAndTeams(jobs);
        finishAndInvalidScenes(jobs);
    } catch (const std::exception& e) {
        check(false, e.what());
    }
    std::printf("Arena checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
