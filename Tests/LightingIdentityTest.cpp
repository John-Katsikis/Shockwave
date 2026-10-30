// Baked lighting identity (G05; docs/GraphicsImplementation.md 9.1): binding ids survive
// saving and reordering a scene, duplicates and missing ids are reported (never silently
// fixed), assigning fills exactly what's needed, prefab copies derive distinct ids, and
// mirrored or non-uniformly scaled receivers keep their own hashes.

#include <Assets/Assets.h>
#include <Engine/BakedLighting.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>

#include <cstdio>
#include <sstream>
#include <string>

using namespace shockwave;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
Entity wall(World& world, const std::string& name, const std::string& id, MeshHandle mesh) {
    Entity e = world.create();
    world.add<Name>(e, {name});
    world.add<Transform>(e);
    MeshRenderer r;
    r.mesh = mesh;
    world.add<MeshRenderer>(e, r);
    BakeGeometry g;
    g.bindingId = id;
    g.mobility = BakeGeometry::Mobility::Static;
    g.receiveLightmap = true;
    world.add<BakeGeometry>(e, g);
    return e;
}
BindingId idOf(const World& world, Entity e) {
    return effectiveBindingId(world, e, world.get<BakeGeometry>(e)->bindingId);
}
}   // namespace

int main() {
    Assets assets;
    const MeshHandle cube{7};
    assets.addMesh("cube", cube, std::make_shared<MeshData>(makeCube()));

    // Saved, reordered and loaded again: every wall keeps its id, and a bake finds it.
    {
        World world;
        const std::string a = formatBindingId(newBindingId()), b = formatBindingId(newBindingId());
        wall(world, "A", a, cube);
        wall(world, "B", b, cube);
        std::ostringstream out;
        writeScene(out, world, assets);
        const std::string text = out.str();
        check(text.find("binding_id \"" + a + "\"") != std::string::npos, "scene: ids are saved");
        check(text.find("bake_id") == std::string::npos, "scene: an unset prefab bake_id isn't written");
        // Swap the two entity blocks.
        const size_t first = text.find("\nentity \"A\""), second = text.find("\nentity \"B\"");
        const std::string reordered = text.substr(0, first) + text.substr(second) + text.substr(first, second - first);
        World loaded;
        std::istringstream in(reordered);
        std::string error;
        check(readScene(loaded, assets, in, "reordered.swscene", error), "scene: reordered loads: " + error);
        Entity la, lb;
        loaded.each<const Name>([&](Entity e, const Name& n) { (n.value == "A" ? la : lb) = e; });
        check(la.index != 0 || lb.index != 1, "scene: really reordered");
        check(
            formatBindingId(idOf(loaded, la)) == a && formatBindingId(idOf(loaded, lb)) == b,
            "scene: ids follow the walls");
        check(checkLightingIds(loaded).empty(), "scene: no problems");
    }
    // Missing, malformed and duplicate ids are reported; assign fixes them, once.
    {
        World world;
        const std::string same = formatBindingId(newBindingId());
        const Entity first = wall(world, "First", same, cube);
        const Entity copy = wall(world, "Copy", same, cube);   // duplicated without a fresh id
        const Entity missing = wall(world, "Missing", "", cube);
        const Entity bad = wall(world, "Bad", "XYZ", cube);
        Entity dynamic = wall(world, "Dynamic", "", cube);
        world.get<BakeGeometry>(dynamic)->mobility = BakeGeometry::Mobility::Dynamic;
        const auto problems = checkLightingIds(world);
        check(problems.size() == 3, "check: three problems, got " + std::to_string(problems.size()));
        std::string all;
        for (const std::string& p : problems) all += p + "\n";
        check(all.find("\"Copy\" has the same binding id as \"First\"") != std::string::npos, "check: duplicate named");
        check(all.find("\"Missing\" has no binding_id") != std::string::npos, "check: missing named");
        check(all.find("\"Bad\"") != std::string::npos, "check: malformed named");
        check(all.find("Dynamic") == std::string::npos, "check: dynamic objects need no id");
        const std::string before = world.get<BakeGeometry>(first)->bindingId;
        check(assignLightingIds(world) == 3, "assign: sets exactly three");
        check(world.get<BakeGeometry>(first)->bindingId == before, "assign: the first keeps its id");
        check(world.get<BakeGeometry>(copy)->bindingId != before, "assign: the duplicate gets a fresh one");
        check(
            !world.get<BakeGeometry>(missing)->bindingId.empty() && world.get<BakeGeometry>(bad)->bindingId != "XYZ",
            "assign: missing and malformed filled");
        check(world.get<BakeGeometry>(dynamic)->bindingId.empty(), "assign: dynamic objects left alone");
        check(checkLightingIds(world).empty(), "assign: then no problems");
        check(assignLightingIds(world) == 0, "assign: a second time changes nothing");
        // Duplicating in the editor refreshes the copy.
        const Entity duplicate = cloneWithChildren(world, first);
        check(!checkLightingIds(world).empty(), "duplicate: a plain copy collides");
        refreshLightingIds(world, duplicate);
        check(checkLightingIds(world).empty(), "duplicate: refreshed ids don't");
    }
    // Two copies of one prefab: the part has one id in the prefab file, but each copy's
    // bake_id makes them distinct; copies without one are reported, then assigned.
    {
        World world;
        const std::string local = formatBindingId(newBindingId());
        Entity roots[2], parts[2];
        for (int i = 0; i < 2; ++i) {
            roots[i] = world.create();
            world.add<Name>(roots[i], {"Lamp " + std::to_string(i)});
            world.add<Transform>(roots[i]);
            world.add<PrefabInstance>(roots[i], {"Prefabs/Lamp.swprefab", nullptr, {}});
            parts[i] = wall(world, "Shade", local, cube);
            world.add<PrefabPart>(parts[i]);
            setParent(world, parts[i], roots[i], false);
        }
        const auto problems = checkLightingIds(world);
        check(
            problems.size() == 2 && problems[0].find("bake_id") != std::string::npos,
            "prefab: copies without a bake_id are reported");
        check(assignLightingIds(world) == 2, "prefab: assign gives each copy a bake_id (not the parts)");
        check(world.get<BakeGeometry>(parts[0])->bindingId == local, "prefab: the part keeps the prefab's id");
        check(
            idOf(world, parts[0]).valid() && idOf(world, parts[0]) != idOf(world, parts[1]),
            "prefab: two copies, two effective ids");
        BindingId copy, localId;
        parseBindingId(world.get<PrefabInstance>(roots[0])->bakeId, copy);
        parseBindingId(local, localId);
        check(idOf(world, parts[0]) == deriveBindingId(copy, localId), "prefab: the documented derivation");
        check(checkLightingIds(world).empty(), "prefab: no problems after");
    }
    // What a receiver's hash covers: its placement (mirrored and stretched too), not
    // things that don't change its light.
    {
        World world;
        const Entity e = wall(world, "W", formatBindingId(newBindingId()), cube);
        auto hash = [&] { return describeBakeScene(world, assets, {}).surfaces.at(0).hash; };
        const uint64_t original = hash();
        world.get<Transform>(e)->scale = {2.0f, 1.0f, 0.5f};
        const uint64_t stretched = hash();
        world.get<Transform>(e)->scale = {-2.0f, 1.0f, 0.5f};
        const uint64_t mirrored = hash();
        check(original != stretched && stretched != mirrored, "hash: non-uniform and mirroring transforms count");
        world.get<Transform>(e)->scale = {1, 1, 1};
        check(hash() == original, "hash: back where it was, the same hash");
        world.add<Name>(e, {"Renamed"});
        check(hash() == original, "hash: renaming doesn't invalidate");
        world.get<MeshRenderer>(e)->tint = {1, 0, 0, 1};
        check(hash() != original, "hash: its colour does");
    }

    if (failures) {
        std::fprintf(stderr, "%d lighting identity check(s) failed\n", failures);
        return 1;
    }
    std::printf("lighting identity: all checks passed\n");
    return 0;
}
