// The lighting baker (G05, G06; docs/GraphicsImplementation.md 9.8 and 10.3) on small CPU
// scenes, no GPU: constant and black surroundings, no baked direct light on the receiver,
// coloured bounce, closed boxes, alpha-masked holes, bounce counts, determinism,
// cancellation, invalidation by moving lights and receivers, and probe validity.

#include <Assets/Assets.h>
#include <Engine/BakedLighting.h>
#include <Engine/Components.h>
#include <Lighting/LightingBaker.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

struct Fixture {
    Assets assets;
    World world;
    fs::path content;
    uint32_t nextMesh = 1;
    explicit Fixture(const std::string& name) {
        content = fs::temp_directory_path() / ("shockwave_lighting_bake_" + name);
        std::error_code ec;
        fs::remove_all(content, ec);
        fs::create_directories(content);
        assets.setContentRoot(content);
        Entity env = world.create();
        world.add<Transform>(env);
        world.add<Environment>(env);
        sky(glm::vec3(0.0f));
    }
    void sky(const glm::vec3& c) {
        world.pool<Environment>()->components()[0].skyColor = glm::vec4(c, 1.0f);
        world.pool<Environment>()->components()[0].groundColor = glm::vec4(c, 1.0f);
        world.pool<Environment>()->components()[0].ambientIntensity = 1.0f;
    }
    MeshHandle mesh(MeshData data) {
        const MeshHandle handle{nextMesh++};
        assets.addMesh("mesh" + std::to_string(handle.id), handle, std::make_shared<MeshData>(std::move(data)));
        return handle;
    }
    Entity object(
        MeshHandle m, const glm::vec3& position, const glm::vec3& scale, bool receive,
        const glm::vec4& tint = glm::vec4(1.0f), const glm::quat& rotation = glm::quat(1, 0, 0, 0)) {
        Entity e = world.create();
        world.add<Transform>(e, {position, rotation, scale});
        MeshRenderer r;
        r.mesh = m;
        r.tint = tint;
        world.add<MeshRenderer>(e, r);
        BakeGeometry g;
        g.bindingId = formatBindingId(newBindingId());
        g.mobility = BakeGeometry::Mobility::Static;
        g.receiveLightmap = receive;
        g.texelsPerMeter = 4.0f;
        world.add<BakeGeometry>(e, g);
        return e;
    }
    Entity light(const glm::vec3& position, const glm::vec3& forward, float intensity = 1.0f) {
        Entity e = world.create();
        Transform t;
        t.position = position;
        t.rotation = glm::quatLookAt(
            glm::normalize(forward), std::abs(forward.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
        world.add<Transform>(e, t);
        Light l;
        l.type = Light::Type::Directional;
        l.intensity = intensity;
        world.add<Light>(e, l);
        world.add<BakeLight>(e);
        return e;
    }
    bool bake(
        LightingBakeSettings settings, LightingBakeReport& report, std::string& error,
        LightingBakeProgress* progress = nullptr) {
        settings.atlasSize = 128;
        if (!settings.samples) settings.samples = 64;
        return bakeLighting(world, assets, content, "Scenes/Test.swscene", settings, report, error, progress);
    }
    // The average B over a receiver's covered texels.
    glm::vec3 average(Entity e) {
        LightingManifest manifest;
        std::string error;
        if (!loadLightingManifest(content / "Lighting/Test.swlighting", manifest, error)) return glm::vec3(-1.0f);
        std::vector<uint8_t> bytes;
        const LightingResource* r = manifest.resource(LightingResource::Kind::Receivers);
        std::vector<LightingReceiver> receivers;
        if (!r || !readLightingFile(content / r->path, bytes, error) || !readLightingReceivers(bytes, receivers, error))
            return glm::vec3(-1.0f);
        const BindingId binding = effectiveBindingId(world, e, world.get<BakeGeometry>(e)->bindingId);
        for (const LightingReceiver& rec : receivers) {
            if (rec.binding != binding) continue;
            const LightingResource* map = manifest.resource(LightingResource::Kind::Lightmap, rec.atlas);
            uint32_t w = 0, h = 0;
            std::vector<float> rgba;
            if (!readLightingFile(content / map->path, bytes, error) || !decodeLightmapKtx2(bytes, w, h, rgba, error))
                return glm::vec3(-1.0f);
            // Texels under the receiver's triangles, by their UVs' bounds (the charts don't overlap).
            glm::vec2 lo(1e9f), hi(-1e9f);
            for (const glm::vec2& uv : rec.lightmapUv) lo = glm::min(lo, uv), hi = glm::max(hi, uv);
            glm::vec3 sum(0.0f);
            float count = 0;
            for (uint32_t y = uint32_t(lo.y * h) + 1; y + 1 < uint32_t(hi.y * h); ++y)
                for (uint32_t x = uint32_t(lo.x * w) + 1; x + 1 < uint32_t(hi.x * w); ++x) {
                    const float* t = &rgba[(size_t(y) * w + x) * 4];
                    if (t[3] <= 0.0f) continue;
                    sum += glm::vec3(t[0], t[1], t[2]);
                    count += 1;
                }
            return count ? sum / count : glm::vec3(-1.0f);
        }
        return glm::vec3(-1.0f);
    }
};

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
}
std::string text(const glm::vec3& v) {
    char out[96];
    std::snprintf(out, sizeof(out), "(%.4f %.4f %.4f)", v.x, v.y, v.z);
    return out;
}

// A cube whose faces point inwards: a closed room.
MeshData insideCube(float size) {
    MeshData cube = makeCube(size);
    for (size_t i = 0; i + 2 < cube.indices.size(); i += 3) std::swap(cube.indices[i + 1], cube.indices[i + 2]);
    for (Vertex& v : cube.vertices) v.normal = -v.normal;
    return cube;
}
}   // namespace

int main() {
    LightingBakeSettings settings;
    std::string error;
    LightingBakeReport report;

    {
        // Constant surroundings and nothing in the way: B is the surroundings' radiance.
        Fixture f("constant");
        f.sky({0.25f, 0.5f, 0.75f});
        const Entity plane = f.object(f.mesh(makePlane(2.0f)), {0, 0, 0}, {1, 1, 1}, true);
        check(f.bake(settings, report, error), "constant: bakes: " + error);
        const glm::vec3 b = f.average(plane);
        check(near(b, {0.25f, 0.5f, 0.75f}, 0.02f), "constant: B equals the surroundings, got " + text(b));
        check(report.receivers == 1 && report.atlases == 1 && report.texels > 0, "constant: report counts");
    }
    {
        // Black surroundings, no lights, no emission: nothing.
        Fixture f("black");
        const Entity plane = f.object(f.mesh(makePlane(2.0f)), {0, 0, 0}, {1, 1, 1}, true);
        check(f.bake(settings, report, error), "black: bakes: " + error);
        check(near(f.average(plane), glm::vec3(0.0f), 1e-6f), "black: B is zero");
    }
    {
        // A plane lit directly with nothing to bounce off: its own direct light isn't baked.
        Fixture f("direct");
        const Entity plane = f.object(f.mesh(makePlane(4.0f)), {0, 0, 0}, {1, 1, 1}, true);
        f.light({0, 5, 0}, {0, -1, 0}, 3.0f);
        check(f.bake(settings, report, error), "direct: bakes: " + error);
        check(near(f.average(plane), glm::vec3(0.0f), 1e-6f), "direct: no baked direct light on the receiver");
    }
    {
        // A red wall lit by the sun bounces red onto the floor; with 0 bounces it doesn't.
        Fixture f("bounce");
        const MeshHandle plane = f.mesh(makePlane(4.0f));
        const Entity floor = f.object(plane, {0, 0, 0}, {1, 1, 1}, true);
        // The wall: a plane turned to face -X, standing at x = 1, red.
        f.object(
            plane, {1.0f, 2.0f, 0}, {1, 1, 1}, false, {1.0f, 0.05f, 0.05f, 1.0f},
            glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)));
        f.light({-5, 5, 0}, {1, -0.2f, 0}, 4.0f);
        LightingBakeSettings one = settings;
        one.bounces = 1;
        check(f.bake(one, report, error), "bounce: bakes: " + error);
        const glm::vec3 b1 = f.average(floor);
        check(b1.r > 0.01f && b1.r > 4.0f * b1.g, "bounce: the floor gets red light from the wall, got " + text(b1));
        LightingBakeSettings none = settings;
        none.bounces = 0;
        check(f.bake(none, report, error), "bounce 0: bakes: " + error);
        check(near(f.average(floor), glm::vec3(0.0f), 1e-6f), "bounce 0: only surroundings and emission");
        LightingBakeSettings two = settings;
        two.bounces = 2;
        check(f.bake(two, report, error), "bounce 2: bakes: " + error);
        const glm::vec3 b2 = f.average(floor);
        check(b2.r >= b1.r * 0.98f, "bounce 2: at least as much light as 1, got " + text(b2) + " vs " + text(b1));
    }
    {
        // A closed room blocks the surroundings entirely.
        Fixture f("box");
        f.sky(glm::vec3(1.0f));
        const Entity floor = f.object(f.mesh(makePlane(1.0f)), {0, -0.9f, 0}, {1, 1, 1}, true);
        f.object(f.mesh(insideCube(2.0f)), {0, 0, 0}, {1, 1, 1}, false, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        check(f.bake(settings, report, error), "box: bakes: " + error);
        const glm::vec3 b = f.average(floor);
        check(near(b, glm::vec3(0.0f), 1e-4f), "box: closed box blocks the surroundings, got " + text(b));
    }
    {
        // A lid with a fully transparent mask lets the sky through; an opaque one doesn't.
        Fixture f("mask");
        f.sky(glm::vec3(1.0f));
        const Entity floor = f.object(f.mesh(makePlane(1.0f)), {0, 0, 0}, {1, 1, 1}, true);
        MeshData lid = makePlane(20.0f);
        for (size_t i = 0; i + 2 < lid.indices.size(); i += 3) std::swap(lid.indices[i + 1], lid.indices[i + 2]);
        const MeshHandle lidMesh = f.mesh(lid);
        // Black, so light bouncing between it and the floor doesn't blur the comparison.
        const Entity cover = f.object(lidMesh, {0, 0.5f, 0}, {1, 1, 1}, false, glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
        check(f.bake(settings, report, error), "mask: opaque bakes: " + error);
        const glm::vec3 blocked = f.average(floor);
        // Turn the lid into a cut-out whose alpha (tint alpha 0) is below the cutoff.
        auto material = std::make_shared<MaterialAsset>();
        material->baseColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        material->draw.alphaCutoff = 0.5f;
        f.world.get<MeshRenderer>(cover)->material = f.assets.addMaterial("cutout.swmaterial", material);
        check(f.bake(settings, report, error), "mask: masked bakes: " + error);
        const glm::vec3 open = f.average(floor);
        check(
            blocked.g < 0.05f && open.g > 0.95f,
            "mask: alpha-masked holes let light through: blocked " + text(blocked) + " open " + text(open));
    }
    {
        // The same seed bakes the same bytes; another seed doesn't.
        Fixture f("seed");
        f.sky({0.3f, 0.6f, 0.9f});
        f.object(f.mesh(makePlane(2.0f)), {0, 0, 0}, {1, 1, 1}, true);
        f.object(f.mesh(makeCube(0.5f)), {0, 0.3f, 0}, {1, 1, 1}, false);
        auto bytes = [&] {
            std::vector<uint8_t> b;
            readLightingFile(f.content / "Lighting/Test/Lightmap_000.ktx2", b, error);
            return b;
        };
        check(f.bake(settings, report, error), "seed: bakes: " + error);
        const auto first = bytes();
        LightingBakeSettings fewerThreads = settings;
        fewerThreads.threads = 1;
        check(f.bake(fewerThreads, report, error), "seed: bakes again: " + error);
        check(!first.empty() && bytes() == first, "seed: same seed, same bytes, whatever the threads");
        LightingBakeSettings other = settings;
        other.seed = 2;
        check(f.bake(other, report, error), "seed: other seed bakes: " + error);
        check(bytes() != first, "seed: another seed differs");

        // Cancelled: fails and leaves the last bake as it was.
        const auto before = bytes();
        LightingBakeProgress progress;
        progress.cancel = true;
        check(!f.bake(settings, report, error, &progress), "cancel: a cancelled bake fails");
        check(error.find("cancelled") != std::string::npos, "cancel: says so");
        check(bytes() == before, "cancel: the last bake is still in place");
        check(!fs::exists(f.content / "Lighting/.Test.bake"), "cancel: no temporary folder left behind");
    }
    {
        // Invalidation, as the runtime sees it without a renderer: moving a light invalidates
        // the whole bake; moving a receiver only that receiver.
        Fixture f("stale");
        f.sky({0.2f, 0.2f, 0.2f});
        const MeshHandle plane = f.mesh(makePlane(2.0f));
        const Entity a = f.object(plane, {0, 0, 0}, {1, 1, 1}, true);
        f.object(plane, {5, 0, 0}, {1, 1, 1}, true);
        const Entity sun = f.light({0, 5, 0}, {0, -1, 0});
        check(f.bake(settings, report, error), "stale: bakes: " + error);
        LightingManifest manifest;
        check(loadLightingManifest(f.content / report.manifest, manifest, error), "stale: manifest loads");
        auto scene = describeBakeScene(f.world, f.assets, f.content);
        check(lightingManifestHash(scene.lightingHash, manifest) == manifest.lightingHash, "stale: fresh bake matches");
        check(scene.geometryHash == manifest.geometryHash, "stale: geometry matches");
        f.world.get<Transform>(a)->position.y = 1.0f;
        scene = describeBakeScene(f.world, f.assets, f.content);
        check(
            lightingManifestHash(scene.lightingHash, manifest) == manifest.lightingHash,
            "stale: moving a receiver keeps lighting");
        check(scene.geometryHash != manifest.geometryHash, "stale: moving a receiver is stale geometry");
        uint32_t matching = 0;
        for (const LightingBinding& b : manifest.receivers)
            if (const BakeSurface* s = scene.surface(b.binding, b.part); s && s->hash == b.hash) ++matching;
        check(matching == 1, "stale: only the moved receiver is invalid");
        f.world.get<Transform>(sun)->position.x = 1.0f;   // directional: position doesn't matter...
        f.world.get<Light>(sun)->intensity = 2.0f;   // ...but intensity does
        scene = describeBakeScene(f.world, f.assets, f.content);
        check(
            lightingManifestHash(scene.lightingHash, manifest) != manifest.lightingHash,
            "stale: a changed light invalidates");
    }
    {
        // Probes: constant surroundings give a constant at every normal; a probe inside a
        // closed box is invalid.
        Fixture f("probes");
        f.sky({0.4f, 0.4f, 0.4f});
        Entity volume = f.world.create();
        f.world.add<Transform>(volume);
        DiffuseProbeVolume v;
        v.halfExtents = {1, 1, 1};
        v.spacing = 1.0f;
        v.bindingId = formatBindingId(newBindingId());
        f.world.add<DiffuseProbeVolume>(volume, v);
        f.object(f.mesh(makeCube(0.6f)), {1, 1, 1}, {1, 1, 1}, false);   // a solid block around the corner probe
        check(f.bake(settings, report, error), "probes: bakes: " + error);
        check(report.volumes == 1 && report.probes == 27, "probes: 3 x 3 x 3 grid");
        check(
            report.invalidProbes == 1,
            "probes: the probe inside the box is invalid, got " + std::to_string(report.invalidProbes));
        LightingManifest manifest;
        loadLightingManifest(f.content / report.manifest, manifest, error);
        std::vector<uint8_t> bytes;
        LightingProbeSet set;
        const LightingResource* r = manifest.resource(LightingResource::Kind::Probes);
        check(
            r && readLightingFile(f.content / r->path, bytes, error) && readLightingProbes(bytes, set, error),
            "probes: payload reads: " + error);
        if (!set.volumes.empty()) {
            const LightingProbe& centre = set.volumes[0].probes[13];
            for (const glm::vec3& n :
                 {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1)}) {
                const glm::vec3 e = shEvaluate(centre.sh, n);
                check(
                    near(e, glm::vec3(0.4f), 0.03f), "probes: constant surroundings, constant result, got " + text(e));
            }
            const glm::vec3 env = shEvaluate(set.environment, glm::vec3(0.6f, 0.0f, 0.8f));
            check(near(env, glm::vec3(0.4f), 0.01f), "probes: environment SH constant, got " + text(env));
            check(!set.volumes[0].probes[26].valid, "probes: the corner probe at (1, 1, 1) is the invalid one");
        }
    }
    {
        // Nothing to bake is an error, and so is an atlas budget it would exceed.
        Fixture f("empty");
        check(!f.bake(settings, report, error), "empty: nothing to bake fails");
        f.object(f.mesh(makePlane(50.0f)), {0, 0, 0}, {1, 1, 1}, true);
        f.world.pool<BakeGeometry>()->components()[0].texelsPerMeter = 64.0f;
        check(
            !f.bake(settings, report, error) && error.find("texels_per_meter") != std::string::npos,
            "budget: a receiver bigger than the atlas is refused, not shrunk");
    }

    if (failures) {
        std::fprintf(stderr, "%d lighting bake check(s) failed\n", failures);
        return 1;
    }
    std::printf("lighting bake: all checks passed\n");
    return 0;
}
