// Particle effects without a GPU: the sample effects load with their textures, simulate
// on the CPU, and build valid triangles for a camera; ParticleEffect starts, stops,
// loops and follows its entity; one-shots, problems and hot reload work; and
// EffectsWorld (game.effects()) drives it all. A04: the graphics fixture's effects carry
// soft particle distances, distortion and lighting into their batches, the support report
// reads them (and Effekseer's own distortion example), and effects sort back to front,
// whole or in parts.
#include <Effects/Effects.h>
#include <Effects/EffectsWorld.h>
#include <Engine/Camera.h>
#include <Engine/Components.h>
#include <Engine/World.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void touch(const fs::path& file, int seconds) {
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(seconds));
}

void run(Effects& effects, World& world, int frames) {
    for (int i = 0; i < frames; ++i) effects.update(world, 1.0 / 60.0);
}

// Every index points at a vertex, every batch covers whole triangles inside the list.
void checkList(const ParticleDrawList& list, const std::string& what) {
    check(list.indices.size() % 3 == 0, what + ": indices aren't whole triangles");
    for (uint32_t index : list.indices) check(index < list.vertices.size(), what + ": index out of range");
    for (const ParticleBatch& batch : list.batches)
        check(
            batch.indexCount > 0 && batch.indexCount % 3 == 0 &&
                batch.firstIndex + batch.indexCount <= list.indices.size(),
            what + ": batch out of range");
}

const ParticleBatch* firstBatch(const ParticleDrawList& list, uint32_t flag) {
    for (const ParticleBatch& b : list.batches)
        if ((b.flags & flag) == flag) return &b;
    return nullptr;
}

// A04 (docs/Effects.md): soft particles, distortion, lighting, sorting, the support report.
void testParticleFeatures(const fs::path& content) {
    Effects effects(nullptr);
    effects.setContentFolder(content);
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 1, 8), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
    const glm::mat4 projection = perspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f);

    // The support report, from the decoded nodes.
    const EffectInfo haze = effects.info("effects/heat-haze.efkefc");
    check(haze.loaded && haze.nodes == 1 && haze.features.size() == 1, "heat-haze report");
    check(
        haze.features[0].feature == "sprite (distortion)" && haze.features[0].status == "supported" &&
            haze.features[0].setting == "particle_distortion",
        "heat-haze's distortion isn't reported supported");
    const EffectInfo soft = effects.info("effects/soft-alpha.efkefc");
    check(
        std::count_if(
            soft.features.begin(), soft.features.end(),
            [](const EffectFeature& f) { return f.feature == "soft particles" && f.setting == "soft_particles"; }) == 2,
        "soft-alpha's two soft nodes aren't reported");
    check(effects.validate("effects/soft-alpha.efkefc").empty(), "soft-alpha has problems");
    // Effekseer's own distortion example (made in its editor; no textures here): its third
    // node, a ring, is a distortion one.
    const EffectInfo upstream = effects.info("effects/upstream/distortion.efk");
    check(upstream.loaded && upstream.nodes >= 4, "Effekseer's distortion example didn't load");
    check(
        std::any_of(
            upstream.features.begin(), upstream.features.end(),
            [](const EffectFeature& f) {
                return f.node == "2" && f.feature == "ring (distortion)" && f.status == "supported";
            }),
        "Effekseer's distortion ring isn't reported");

    World world;
    auto show = [&](const char* file, glm::vec3 at, ParticleEffect pe = {}) {
        const Entity e = world.create();
        world.add<Transform>(e, {at});
        pe.effect = file;
        world.add<ParticleEffect>(e, pe);
        return e;
    };
    auto frame = [&] {
        run(effects, world, 3);
        return effects.build(view, projection);
    };

    // Distortion batches carry their flag, strength and a tangent; lit ones their flag.
    Entity e = show("effects/heat-haze.efkefc", {0, 1, 0});
    ParticleDrawList list = frame();
    checkList(list, "heat-haze");
    const ParticleBatch* d = firstBatch(list, kParticleDistortion);
    check(d && std::abs(d->distortion - 0.25f) < 1e-6f && list.batches.size() == 1, "heat-haze's distortion batch");
    for (const ParticleVertex& v : list.vertices) check(v.tangent != 0, "a distortion vertex has no tangent");
    world.destroy(e);
    e = show("effects/lit-smoke.efkefc", {0, 1, 0});
    list = frame();
    check(firstBatch(list, kParticleLit) && list.batches.size() == 1, "lit-smoke isn't lit");
    world.destroy(e);

    // Soft distances: the effect's own, or ParticleEffect's soft_distance (0 turns it off).
    e = show("effects/soft-alpha.efkefc", {0, 0, 0});
    list = frame();
    check(
        !list.batches.empty() && list.batches[0].softDistance == 1.0f && list.batches[0].softNear == 0.0f,
        "soft-alpha's soft distance isn't 1 m");
    world.get<ParticleEffect>(e)->softDistance = 0.0f;
    list = frame();
    check(!list.batches.empty() && list.batches[0].softDistance == 0.0f, "soft_distance 0 didn't turn the fade off");
    world.get<ParticleEffect>(e)->softDistance = 0.4f;
    list = frame();
    check(!list.batches.empty() && list.batches[0].softDistance == 0.4f, "soft_distance 0.4 didn't replace it");
    world.destroy(e);
    e = show("effects/soft-near.efkefc", {0, 0, 0});
    list = frame();
    check(
        !list.batches.empty() && list.batches[0].softNear == 2.0f && list.batches[0].softNearOffset == 0.5f,
        "soft-near's near fade isn't 2 m from 0.5 m");
    world.destroy(e);

    // Sorting: two effects come back to front whatever order they're in.
    const Entity nearFx = show("effects/soft-add.efkefc", {0, 0, 4});
    const Entity farFx = show("effects/soft-alpha.efkefc", {0, 0, -6});
    list = frame();
    check(
        list.batches.size() >= 2 && list.batches.front().blend == ParticleBlend::Alpha &&
            list.batches.back().blend == ParticleBlend::Add && list.batches.front().depth > list.batches.back().depth,
        "effects aren't drawn back to front");
    world.destroy(nearFx);
    world.destroy(farFx);
    // Sorted whole, an effect keeps the order its parts were made in (the near alpha puff,
    // then the far glow); sorted in parts, the far glow goes first.
    e = show("effects/two-layers.efkefc", {0, 0, 0});
    list = frame();
    check(
        list.batches.size() == 2 && list.batches[0].blend == ParticleBlend::Alpha &&
            list.batches[1].blend == ParticleBlend::Add && list.batches[0].depth == list.batches[1].depth,
        "two-layers sorted whole lost its order");
    world.get<ParticleEffect>(e)->sorting = ParticleEffect::Sorting::Parts;
    list = frame();
    checkList(list, "two-layers in parts");
    check(
        list.batches.size() == 2 && list.batches[0].blend == ParticleBlend::Add &&
            list.batches[1].blend == ParticleBlend::Alpha && list.batches[0].depth > list.batches[1].depth,
        "two-layers sorted in parts didn't put the far glow first");
}

}   // namespace

int main(int argc, char** argv) {
    try {
        check(argc > 1, "usage: effects_test <source dir>");
        const fs::path source = argv[1];
        const fs::path content = fs::temp_directory_path() / "shockwave_effects_test";
        fs::remove_all(content);
        fs::create_directories(content);
        fs::copy(source / "Games/Sample/Content/Effects", content / "Effects", fs::copy_options::recursive);

        registerEffectsComponents();
        const ComponentInfo* info = findComponent("ParticleEffect");
        check(info && info->fields.size() == 8, "ParticleEffect isn't registered with its 8 fields");

        Effects effects(nullptr);
        effects.setContentFolder(content);
        const std::vector<std::string> firefight = {
            "Effects/firefight-grenade.efkefc", "Effects/firefight-impact.efkefc", "Effects/firefight-rifle.efkefc",
            "Effects/firefight-smoke.efkefc"};
        std::vector<std::string> files = firefight;
        files.push_back("Effects/laser.efkefc");
        check(effects.effectFiles() == files, "effectFiles");

        // Loading: the sample is clean; mistakes are reported, not thrown.
        for (const EffectProblem& p : effects.validate("Effects/laser.efkefc"))
            check(p.warning, "laser.efkefc: " + p.message);
        check(!effects.validate("Effects/missing.efkefc").empty(), "a missing file isn't reported");
        check(!effects.validate("").empty(), "an empty path isn't reported");
        check(!effects.validate("Effects/old.efkproj").empty(), "an .efkproj isn't reported");
        const float duration = effects.duration("Effects/laser.efkefc");
        check(duration > 0.5f && duration < 30.0f, "laser duration " + std::to_string(duration));

        // The particle stress test's effects (written by Tools/MakeParticleStress.py, not the
        // Effekseer editor) load cleanly, run until stopped and build valid triangles.
        {
            World fight;
            for (const std::string& file : firefight) {
                check(effects.validate(file).empty(), file + " has problems");
                check(effects.duration(file) < 0.0f, file + " should run until stopped");
                const Entity rifle = fight.create();
                fight.add<Transform>(rifle, {glm::vec3(0, 1, 0)});
                fight.add<ParticleEffect>(rifle, {file});
            }
            run(effects, fight, 300);   // past every first-shot delay
            const ParticleDrawList list = effects.build(
                glm::lookAt(glm::vec3(0, 3, 10), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)),
                perspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f));
            checkList(list, "firefight");
            check(effects.stats().particles > 100 && !list.batches.empty(), "the firefight made too few particles");
            std::cout << "firefight: " << effects.stats().particles << " particles, " << list.batches.size()
                      << " batches\n";
            fight.each<ParticleEffect>([&](Entity entity, ParticleEffect&) {
                check(effects.particleCount(entity) > 0, "a firefight effect made no particles");
            });
            effects.restart();   // forget this world's effects
        }

        // A ParticleEffect plays at its entity and follows it.
        World world;
        const Entity e = world.create();
        world.add<Transform>(e, {glm::vec3(0, 1, 0)});
        world.add<ParticleEffect>(e, {"Effects/laser.efkefc"});
        run(effects, world, 30);
        check(effects.handleOf(e) >= 0, "the entity's effect isn't running");
        check(effects.particleCount(e) > 0, "no particles after half a second");

        const glm::mat4 view = glm::lookAt(glm::vec3(0, 2, 8), glm::vec3(0, 1, 0), glm::vec3(0, 1, 0));
        const glm::mat4 projection = perspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f);
        const ParticleDrawList list = effects.build(view, projection);
        checkList(list, "laser");
        check(!list.batches.empty() && !list.vertices.empty(), "the laser built no triangles");
        glm::vec3 centre(0.0f);
        for (const ParticleVertex& v : list.vertices) centre += v.position;
        centre /= static_cast<float>(list.vertices.size());
        const EffectsStats stats = effects.stats();
        std::cout << "laser: " << stats.particles << " particles, " << stats.batches << " batches, " << stats.vertices
                  << " vertices, " << stats.updateMs << " ms update, " << stats.buildMs << " ms build\n";

        // Moving the entity moves the effect (particles already let go of stay put).
        world.get<Transform>(e)->position = glm::vec3(50, 1, 0);
        run(effects, world, 2);
        glm::vec3 moved(0.0f);
        const ParticleDrawList list2 = effects.build(view, projection);
        for (const ParticleVertex& v : list2.vertices) moved += v.position;
        moved /= static_cast<float>(std::max<size_t>(list2.vertices.size(), 1));
        check(moved.x > centre.x + 5.0f, "the effect didn't follow its entity");

        // Turning playing off lets it finish; a looping effect restarts, a one-time one doesn't.
        world.get<ParticleEffect>(e)->playing = false;
        run(effects, world, static_cast<int>(duration * 60.0f) + 120);
        check(effects.handleOf(e) < 0 && effects.particleCount(e) == 0, "stopping didn't finish the effect");
        world.get<ParticleEffect>(e)->playing = true;
        world.get<ParticleEffect>(e)->loop = false;
        run(effects, world, 2);
        check(effects.handleOf(e) >= 0, "turning playing on didn't start it again");
        run(effects, world, static_cast<int>(duration * 60.0f) + 120);
        check(effects.handleOf(e) < 0, "a non-looping effect started again");
        effects.restart(e);
        run(effects, world, 2);
        check(effects.handleOf(e) >= 0, "restart(entity) didn't start it");
        world.get<ParticleEffect>(e)->loop = true;
        run(effects, world, static_cast<int>(duration * 60.0f) + 120);
        check(effects.handleOf(e) >= 0, "a looping effect didn't start again");

        // Removing the component removes the effect.
        world.remove<ParticleEffect>(e);
        run(effects, world, 1);
        check(effects.handleOf(e) < 0, "the effect outlived its component");

        // One-shots and EffectsWorld.
        {
            EffectsWorld game(world, &effects);
            const auto shot = game.play("Effects/laser.efkefc", glm::vec3(0, 0, -3));
            check(shot >= 0 && game.playing(shot), "one-shot didn't play");
            check(game.move(shot, glm::vec3(1, 0, -3)) && game.setSpeed(shot, 2.0f), "one-shot control failed");
            check(game.play("Effects/missing.efkefc", glm::vec3(0)) < 0, "a missing effect played");
            check(!game.stop(-1) && !game.trigger(-1, 0), "an unknown handle did something");
            run(effects, world, 10);
            check(game.particleCount() > 0, "one-shot made no particles");
            checkList(effects.build(view, projection), "one-shot");

            const Entity ship = world.create();
            world.add<Transform>(ship, {});
            world.add<ParticleEffect>(ship, {"Effects/laser.efkefc", false});
            run(effects, world, 2);
            check(effects.handleOf(ship) < 0, "a ParticleEffect with playing off started");
            check(game.play(ship) && world.get<ParticleEffect>(ship)->playing, "game.effects().play(entity)");
            run(effects, world, 2);
            check(effects.handleOf(ship) >= 0, "play(entity) didn't start it");
            check(game.stop(ship, true), "stop(entity)");
            run(effects, world, 2);
            check(effects.handleOf(ship) < 0, "stop(entity, immediately) left it running");
        }
        run(effects, world, 2);
        effects.build(view, projection);
        check(effects.stats().playing == 0, "one-shots outlived their EffectsWorld");

        // Hot reload: a changed texture reloads the effect and running ones start again.
        const Entity again = world.create();
        world.add<Transform>(again, {});
        world.add<ParticleEffect>(again, {"Effects/laser.efkefc"});
        run(effects, world, 5);
        const auto before = effects.handleOf(again);
        touch(content / "Effects/Texture/Particle01.png", 5);
        run(effects, world, 40);
        check(effects.handleOf(again) >= 0 && effects.handleOf(again) != before, "a texture change didn't reload");

        // Off: nothing simulates or draws.
        effects.setEnabled(false);
        run(effects, world, 2);
        check(effects.build(view, projection).batches.empty(), "disabled effects still draw");

        fs::remove_all(content);
        testParticleFeatures(source / "Tests/fixtures/graphics/Content");
        std::cout << "effects test passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "effects test failed: " << e.what() << "\n";
        return 1;
    }
}
