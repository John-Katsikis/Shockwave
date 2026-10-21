// Particle effects without a GPU: the sample effects load with their textures, simulate
// on the CPU, and build valid triangles for a camera; ParticleEffect starts, stops,
// loops and follows its entity; one-shots, problems and hot reload work; and
// EffectsWorld (game.effects()) drives it all.
#include <Effects/Effects.h>
#include <Effects/EffectsWorld.h>
#include <Engine/Camera.h>
#include <Engine/Components.h>
#include <Engine/World.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
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
        check(info && info->fields.size() == 6, "ParticleEffect isn't registered with its 6 fields");

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
        std::cout << "effects test passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "effects test failed: " << e.what() << "\n";
        return 1;
    }
}
