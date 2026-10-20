// Physics prediction and presentation (plan Batch 7, docs/Networking.md "Prediction"):
// PhysicsWorld's network API and collision policy, the engine InputSampler at frame
// rates from 24 to 1000 fps, and the --net-test prediction scenario (real NetworkWorlds
// with Jolt over the loopback hub) on every fault preset at 60 and 120 Hz: predictions
// converge on the host, replays run no game code and make no events, the correction
// offset is gone within 150 ms, and every character is drawn where the host has it.
#include <Core/Jobs.h>
#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/SimulationClock.h>
#include <GameFramework/InputSampler.h>
#include <Multiplayer/NetTest.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkValidation.h>
#include <Multiplayer/NetworkWorld.h>
#include <Physics/Physics.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace shockwave;
using namespace shockwave::net;

namespace {

int failures = 0, checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

constexpr double kDt = 1.0 / 60;

// A floor, a character at the origin and, optionally, a dynamic box in its way and a
// trigger it walks through.
struct Scene {
    World world;
    JobSystem jobs{1};
    std::unique_ptr<PhysicsWorld> physics;
    Entity character, box, trigger;

    Scene(PhysicsNetworkMode mode, bool withBox, bool withTrigger = false) {
        auto block = [&](glm::vec3 position, glm::vec3 half, RigidBody::Motion motion, bool sensor = false) {
            const Entity e = world.create();
            Transform t;
            t.position = position;
            world.add<Transform>(e, t);
            Collider c;
            c.halfExtents = half;
            world.add<Collider>(e, c);
            RigidBody body;
            body.motion = motion;
            body.sensor = sensor;
            body.mass = 80;
            world.add<RigidBody>(e, body);
            return e;
        };
        block({0, -0.5f, 0}, {40, 0.5f, 40}, RigidBody::Motion::Static);
        if (withBox) box = block({2, 0.5f, 0}, {0.5f, 0.5f, 0.5f}, RigidBody::Motion::Dynamic);
        if (withTrigger) trigger = block({2, 1, 0}, {0.5f, 1, 1}, RigidBody::Motion::Static, true);
        character = world.create();
        Transform at;
        at.position = {0, 0.05f, 0};
        world.add<Transform>(character, at);
        world.add<CharacterController>(character, {});
        physics = std::make_unique<PhysicsWorld>(world, jobs);
        physics->setNetworkMode(mode);
        if (mode != PhysicsNetworkMode::Offline) physics->setPredictedCharacters(std::span(&character, 1));
        physics->synchronize();
    }
    glm::vec3 position() const { return world.get<Transform>(character)->position; }
};

// What the scripted player does on tick i: walk +x, jump twice, turn back.
glm::vec3 walk(int i) { return i < 120 ? glm::vec3(4, 0, 0) : glm::vec3(-2, 0, 1); }
bool jump(int i) { return i == 20 || i == 150; }

void physicsApi() {
    // The host's step and a client's predictCharacterStep are the same movement, and a
    // predicted character goes through the dynamic box on both.
    Scene host(PhysicsNetworkMode::Authority, true), client(PhysicsNetworkMode::Client, true);
    check(
        host.physics->predicted(host.character) && client.physics->predicted(client.character),
        "both sides know the character is predicted");
    float worst = 0;
    bool jumped = false;
    for (int i = 0; i < 240; ++i) {
        auto& c = *host.world.get<CharacterController>(host.character);
        c.desiredVelocity = walk(i);
        c.jumpRequested = jump(i);
        host.physics->step(kDt);
        check(
            client.physics->predictCharacterStep(client.character, walk(i), jump(i), kDt) || i == 0,
            "a prediction step works");
        worst = std::max(worst, glm::length(host.position() - client.position()));
        jumped = jumped || host.position().y > 0.5f;
    }
    check(worst < 1e-4f, "host steps and client predictions agree (" + std::to_string(worst) + " m)");
    check(jumped, "the scripted jump left the ground");
    check(host.position().x > 3, "the predicted character walked through the dynamic box on the host");
    // Offline, the same walk runs into the box.
    Scene offline(PhysicsNetworkMode::Offline, true);
    for (int i = 0; i < 120; ++i) {
        auto& c = *offline.world.get<CharacterController>(offline.character);
        c.desiredVelocity = walk(i);
        offline.physics->step(kDt);
    }
    const glm::vec3 boxNow = offline.world.get<Transform>(offline.box)->position;
    check(
        glm::length(boxNow - glm::vec3(2, 0.5f, 0)) > 0.05f || offline.position().x < 1.6f,
        "offline the character still meets the box");

    // characterState and correctCharacter.
    const auto state = client.physics->characterState(client.character);
    check(state && glm::length(state->position - client.position()) < 1e-5f, "characterState is where it stands");
    const CharacterMotionState put{{1, 0.02f, -1}, {0.5f, 0, 0}, true};
    check(client.physics->correctCharacter(client.character, put), "correctCharacter works");
    const auto after = client.physics->characterState(client.character);
    check(
        after && glm::length(after->position - put.position) < 1e-5f &&
            glm::length(after->velocity - put.velocity) < 1e-5f,
        "the character takes the corrected state");
    check(glm::length(client.position() - put.position) < 1e-5f, "and so does its Transform");
    check(!client.physics->correctCharacter(client.box, put), "a box isn't a character");
    check(!client.physics->characterState(client.box), "nor does it have a character state");
    check(!client.physics->predictCharacterStep(client.box, {}, false, kDt), "nor can it be predicted");
    check(!client.physics->correctCharacter(client.character, {{NAN, 0, 0}, {}, false}), "a NaN state is refused");
    check(!client.physics->predictCharacterStep(client.character, {}, false, 0), "a zero step is refused");

    // A client's dynamic body is a proxy: it doesn't fall, and its component stays as
    // authored (Stop restores the scene unchanged).
    Scene proxy(PhysicsNetworkMode::Client, true);
    proxy.world.get<Transform>(proxy.box)->position.y = 3;
    for (int i = 0; i < 60; ++i) proxy.physics->step(kDt);
    check(std::abs(proxy.world.get<Transform>(proxy.box)->position.y - 3) < 1e-5f, "a client's box doesn't fall");
    check(
        proxy.world.get<RigidBody>(proxy.box)->motion == RigidBody::Motion::Dynamic,
        "and its RigidBody still says Dynamic");
    check(
        proxy.physics->applyReplicaBodyPose(proxy.box, {1, 2, 1}, glm::quat(1, 0, 0, 0), {0, -1, 0}, {}),
        "applyReplicaBodyPose works");
    check(
        glm::length(proxy.world.get<Transform>(proxy.box)->position - glm::vec3(1, 2, 1)) < 1e-5f,
        "the proxy is where the host says");
    check(
        !proxy.physics->applyReplicaBodyPose(proxy.box, {NAN, 0, 0}, glm::quat(1, 0, 0, 0), {}, {}),
        "a NaN pose is refused");
    const uint32_t bodies = proxy.physics->bodyCount();
    proxy.physics->setNetworkMode(PhysicsNetworkMode::Offline);
    proxy.physics->synchronize();
    check(proxy.physics->bodyCount() == bodies, "changing the mode rebuilds the same bodies");

    // Prediction makes no contact or sensor events; the host's step does.
    Scene sensorHost(PhysicsNetworkMode::Authority, false, true), sensorClient(PhysicsNetworkMode::Client, false, true);
    size_t hostEvents = 0, clientEvents = 0;
    for (int i = 0; i < 120; ++i) {
        sensorHost.world.get<CharacterController>(sensorHost.character)->desiredVelocity = walk(i);
        sensorHost.physics->step(kDt);
        hostEvents += sensorHost.physics->contacts().size();
        sensorClient.physics->predictCharacterStep(sensorClient.character, walk(i), false, kDt);
        sensorClient.physics->step(kDt);
        clientEvents += sensorClient.physics->contacts().size();
    }
    check(hostEvents > 0, "the host's character sets off the trigger");
    check(clientEvents == 0, "a client's prediction sets off nothing");
}

// ---- The --check rule ------------------------------------------------------------------------

void checkRule() {
    auto body = [](World& world, const char* name, RigidBody::Motion motion, bool sensor = false) {
        const Entity e = world.create();
        world.add<Name>(e, {name});
        world.add<Transform>(e, {});
        world.add<Collider>(e, {});
        RigidBody b;
        b.motion = motion;
        b.sensor = sensor;
        world.add<RigidBody>(e, b);
    };
    World world;
    const Entity player = world.create();
    world.add<Name>(player, {"Player"});
    world.add<Transform>(player, {});
    world.add<CharacterController>(player, {});
    NetworkObject n;
    n.key = "player";
    n.profile = "engine.pose.v1";
    n.mode = NetworkObject::Mode::PredictedCharacter;
    world.add<NetworkObject>(player, n);
    body(world, "Floor", RigidBody::Motion::Static);
    body(world, "Zone", RigidBody::Motion::Kinematic, true);
    std::vector<NetProblem> problems;
    validatePredictionWorld(world, "a.swscene", false, problems);
    check(problems.empty(), "static bodies and sensors are fine next to predicted characters");
    body(world, "Lift", RigidBody::Motion::Kinematic);
    body(world, "Crate", RigidBody::Motion::Dynamic);
    validatePredictionWorld(world, "a.swscene", false, problems);
    check(problems.size() == 2, "a moving kinematic and a dynamic body are reported");
    check(
        problems.size() == 2 && !problems[0].warning && problems[0].text.find("'Lift'") != std::string::npos &&
            problems[0].text.find("static bodies only") != std::string::npos,
        "the kinematic body is an error that states the rule: " + (problems.empty() ? "" : problems[0].text));
    check(
        problems.size() == 2 && problems[1].warning && problems[1].text.find("pass through") != std::string::npos,
        "the dynamic body is a warning");
    // A scene without predicted characters, or an offline scene, is left alone.
    world.get<NetworkObject>(player)->mode = NetworkObject::Mode::Interpolated;
    problems.clear();
    validatePredictionWorld(world, "a.swscene", false, problems);
    check(problems.empty(), "no predicted characters, no rule");
    validatePredictionWorld(world, "a.swscene", true, problems);
    check(problems.size() == 2, "a network scene the manifest's predicted prefab spawns into is checked");
    World offline;
    body(offline, "Bot", RigidBody::Motion::Kinematic);
    problems.clear();
    validatePredictionWorld(offline, "b.swscene", true, problems);
    check(problems.empty(), "an offline scene (no network objects) isn't");
}

// ---- The input sampler at any frame rate --------------------------------------------------

struct Sampled {
    int ticks = 0, jumps = 0, reloads = 0, fireTicks = 0, moveTicks = 0;
    float yaw = 0;   // the sampler's aim at the end
    float commandYaw = 0;   // the last command's
};

Sampled sampleAt(double fps, double tickRate) {
    InputSampler sampler;
    SimulationClock clock;
    Sampled out;
    const double frame = 1.0 / fps, tick = 1.0 / tickRate, length = 3.0;
    bool jumped = false, reloaded = false;
    const int frames = int(std::lround(length * fps));
    for (int f = 1; f <= frames; ++f) {
        const double t = f * frame;
        InputFrame in;
        in.look = {float(300 * frame), 0};   // 300 pixels a second, whatever the frame rate
        in.moveZ = t > 0.5 && t <= 2.5 ? -1.0f : 0.0f;
        in.fire = t > 1.0 && t <= 2.0;
        if (!jumped && t >= 0.7) in.jumpPressed = jumped = true;
        if (!reloaded && t >= 1.3) in.reloadPressed = reloaded = true;
        sampler.collect(in, 0.1f);
        const int run = clock.advance(frame, frame, tick).ticks;
        for (int i = 0; i < run; ++i) {
            const InputCommand c = sampler.next(1);
            check(!c.invalid(), "a sampled command is valid");
            ++out.ticks;
            out.jumps += (c.pressedBits & InputCommand::kPressedJump) != 0;
            out.reloads += (c.pressedBits & InputCommand::kPressedReload) != 0;
            out.fireTicks += (c.heldBits & InputCommand::kHeldFire) != 0;
            out.moveTicks += c.moveZ != 0;
            out.commandYaw = commandYaw(c);
        }
    }
    out.yaw = sampler.yaw();
    return out;
}

void samplerFrameRates() {
    for (double rate : {60.0, 120.0}) {
        const Sampled base = sampleAt(rate, rate);
        for (double fps : {24.0, 30.0, 60.0, 144.0, 240.0, 1000.0}) {
            const Sampled s = sampleAt(fps, rate);
            const std::string at = " at " + std::to_string(int(fps)) + " fps, " + std::to_string(int(rate)) + " Hz";
            check(s.jumps == 1 && s.reloads == 1, "each press reaches exactly one command" + at);
            // Held controls change on frame boundaries, so they may differ by a frame.
            const int slack = int(std::ceil(rate / fps)) + 1;
            check(std::abs(s.fireTicks - base.fireTicks) <= slack, "fire is held for the same ticks" + at);
            check(std::abs(s.moveTicks - base.moveTicks) <= slack, "movement is held for the same ticks" + at);
            check(std::abs(s.yaw - base.yaw) < 1e-4f, "the aim turns by the same amount" + at);
            // The last command samples the aim at its tick: at most a tick and a frame's
            // turn behind the end.
            check(
                std::abs(s.commandYaw - s.yaw) <= glm::radians(float(300 * (1 / fps + 1 / rate)) * 0.1f) + 2e-4f,
                "commands carry the aim as of their tick" + at);
        }
    }
}

// ---- Sessions -----------------------------------------------------------------------------

void sessions() {
    for (uint32_t rate : {60u, 120u}) {
        for (const char* preset : {"clean", "cafe", "poor", "hostile"}) {
            NetTestOptions options;
            options.scenario = "prediction";
            options.preset = preset;
            options.clients = 4;
            options.seed = 7;
            options.settings.tickRate = rate;
            options.settings.snapshotRate = rate / 2;
            NetTestReport report;
            const std::string at = std::string(" (") + preset + ", " + std::to_string(rate) + " Hz)";
            check(runNetTest(options, report), "the prediction scenario runs" + at);
            check(report.ok, "the prediction scenario passes" + at);
            if (!report.ok) std::fprintf(stderr, "%s", netTestText(report).c_str());
            const NetTestPrediction& p = report.prediction;
            check(p.predictedTicks > 0 && p.checks > 0, "clients predict and check" + at);
            check(p.finalError <= NetworkWorld::kPositionTolerance, "every client ends where the host has it" + at);
            check(p.finalPresentationError <= 0.01, "and draws every character there" + at);
            check(p.hostContacts > 0, "the host's characters set off the trigger" + at);
            if (std::string(preset) == "clean") check(p.resets == 0, "a clean network never snaps" + at);
            if (std::string(preset) == "hostile")
                check(p.corrections > 0 && p.replayedTicks > 0, "a hostile network corrects and replays" + at);
            std::printf(
                "%-7s %3u Hz: %llu predicted, %llu checked, %llu matched, %llu corrections, %llu replayed, max %.3f "
                "m, offset %.3f m\n",
                preset, rate, (unsigned long long)p.predictedTicks, (unsigned long long)p.checks,
                (unsigned long long)p.matches, (unsigned long long)p.corrections, (unsigned long long)p.replayedTicks,
                p.maxCorrection, p.maxOffset);
        }
    }
}

}   // namespace

int main() {
    physicsApi();
    checkRule();
    samplerFrameRates();
    sessions();
    if (failures) {
        std::fprintf(stderr, "network_prediction: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    std::printf("network_prediction: %d checks passed\n", checks);
    return 0;
}
