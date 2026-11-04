// Asset dependencies (S04, docs/AssetResidency.md): the graph over a project made on disk.
// Each provider finds its edges (scenes, prefabs, materials, RML, RCSS, circuits, post
// process, navigation sidecars); missing files, wrong case, paths leaving the project,
// kind conflicts and cycles are reported with their codes; closure hashes change with any
// file a root reaches and with nothing else; unchanged files aren't read again.
#include <Assets/AssetGraph.h>
#include <Circuits/Circuit.h>
#include <Engine/AssetReferences.h>
#include <Engine/Components.h>
#include <GameUI/GameUI.h>
#include <Navigation/NavComponents.h>
#include <Audio/Audio.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace shockwave;
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

fs::path root;

void write(const std::string& file, const std::string& text) {
    const fs::path path = root / file;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
    // Some file systems keep modification times to the second: make each write visible.
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
}

bool has(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

const Diagnostic* findCode(const std::vector<Diagnostic>& list, const std::string& code, const std::string& text = {}) {
    for (const Diagnostic& d : list)
        if (d.code == code && (text.empty() || d.text().find(text) != std::string::npos)) return &d;
    return nullptr;
}

void dump(const std::vector<Diagnostic>& list) {
    for (const Diagnostic& d : list) std::fprintf(stderr, "  %s %s\n", d.code.c_str(), d.text().c_str());
}

void makeProject() {
    root = fs::temp_directory_path() /
           ("asset_dependencies_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    write("Content/textures/bricks.png", "png bytes");
    write("Content/textures/other.png", "unrelated");
    write("Content/textures/sprites.png", "sprite sheet");
    write(
        "Content/materials/brick.swmaterial",
        "swmaterial 1\nparent \"materials/base.swmaterial\"\ntexture base_color \"textures/bricks.png\"\n");
    write("Content/materials/base.swmaterial", "swmaterial 1\nroughness 0.5\n");
    write("Content/Audio/door.wav", "RIFF");
    write(
        "Content/UI/hud.rml",
        "<rml><head><link type=\"text/rcss\" href=\"hud.rcss\"/>\n<!-- <img src=\"gone.png\"/> -->\n"
        "<style>body { decorator: image(../textures/sprites.png); }</style></head>\n"
        "<body><img src=\"../textures/bricks.png\"/><img src=\"{{icon}}\"/><img src=\"sprite-name\"/></body></rml>\n");
    write(
        "Content/UI/hud.rcss",
        "/* image(commented.png) */\n@spritesheet theme { src: ../textures/sprites.png; icon: 0px 0px 8px 8px; }\n"
        "button:hover { color: #fff; }\n");
    write(
        "Content/Circuits/Door.swcircuit",
        "shockwave_circuit 1\n\nnode open Event.BeginPlay\n\nnode sound Audio.PlaySound\n  exec open.then\n"
        "  in clip = \"Audio/door.wav\"\n");
    write(
        "Content/Prefabs/Crate.swprefab",
        "shockwave_prefab 1\n\nentity \"Crate\"\n  MeshRenderer\n    mesh cube\n    material "
        "\"materials/brick.swmaterial\"\nend\n");
    write("Content/post/warm.swpostprocess", "swpostprocess 1\nlut \"post/warm.cube\"\n");
    write("Content/post/warm.cube", "LUT_3D_SIZE 2\n");
    write(
        "Scenes/a.swscene", "shockwave_scene 2\n\nentity \"Floor\"\n  MeshRenderer\n    mesh floor\n    texture "
                            "\"textures/bricks.png\"\nend\n\n"
                            "entity \"Crate\"\n  Prefab\n    path \"Prefabs/Crate.swprefab\"\nend\n\n"
                            "entity \"Door\"\n  Circuit\n    circuit \"Circuits/Door.swcircuit\"\n  AudioSource\n    "
                            "clip \"Audio/door.wav\"\nend\n\n"
                            "entity \"HUD\"\n  UIDocument\n    document \"UI/hud.rml\"\nend\n\n"
                            "entity \"Grade\"\n  PostProcessVolume\n    profile \"post/warm.swpostprocess\"\nend\n\n"
                            "entity \"Lit\"\n  BakedLighting\n    asset \"Lighting/a.swlighting\"\nend\n");
    write("Scenes/a.swnavmesh", "nav");
    write("Scenes/b.swscene", "shockwave_scene 2\n\nentity \"Floor\"\n  MeshRenderer\n    mesh floor\nend\n");
}

void testClosure() {
    AssetGraph graph(root, root / "Content");
    graph.knownName = [](AssetKind, std::string_view name) { return name == "cube" || name == "floor"; };
    const AssetNode& a = graph.add("Scenes/a.swscene");
    CHECK(a.exists && a.kind == AssetKind::Scene);
    const auto closure = graph.closure("Scenes/a.swscene");
    for (const char* id :
         {"Scenes/a.swscene", "Content/textures/bricks.png", "Content/Prefabs/Crate.swprefab",
          "Content/materials/brick.swmaterial", "Content/materials/base.swmaterial", "Content/Circuits/Door.swcircuit",
          "Content/Audio/door.wav", "Content/UI/hud.rml", "Content/UI/hud.rcss", "Content/textures/sprites.png",
          "Content/post/warm.swpostprocess", "Content/post/warm.cube", "Scenes/a.swnavmesh", "builtin:cube",
          "builtin:floor"}) {
        if (!has(closure, id)) std::fprintf(stderr, "closure lacks %s\n", id);
        CHECK(has(closure, id));
    }
    // Not reached: a file nothing names, a commented-out image, a bound image, a sprite's
    // name, a lighting bake that isn't made yet.
    CHECK(!has(closure, "Content/textures/other.png"));
    CHECK(!has(closure, "Content/UI/gone.png"));
    CHECK(!has(closure, "Content/UI/commented.png"));
    CHECK(!has(closure, "Content/Lighting/a.swlighting"));
    const auto problems = graph.diagnostics("Scenes/a.swscene");
    if (!problems.empty()) dump(problems);
    CHECK(problems.empty());
    // b has no navigation mesh: the optional sidecar isn't in its closure.
    graph.add("Scenes/b.swscene");
    CHECK(!has(graph.closure("Scenes/b.swscene"), "Scenes/b.swnavmesh"));
    CHECK(graph.diagnostics("Scenes/b.swscene").empty());
    // Edges say where they came from.
    bool sawVia = false;
    for (const AssetEdge& e : a.edges)
        sawVia |= e.to == "Content/Circuits/Door.swcircuit" && e.via == "entity 'Door': Circuit.circuit" && e.line > 0;
    CHECK(sawVia);
}

void testHashesAndCache() {
    AssetGraph graph(root, root / "Content");
    graph.add("Scenes/a.swscene");
    graph.add("Scenes/b.swscene");
    const uint64_t a1 = graph.find("Scenes/a.swscene")->closureHash;
    const uint64_t b1 = graph.find("Scenes/b.swscene")->closureHash;
    const uint64_t material1 = graph.find("Content/materials/brick.swmaterial")->closureHash;
    // Nothing changed: nothing is read again, and nothing changes.
    graph.add("Scenes/a.swscene");
    CHECK(graph.lastReads() == 0);
    CHECK(graph.find("Scenes/a.swscene")->closureHash == a1);
    // A texture two steps down changes the material, the prefab and the scene, not b.
    write("Content/textures/bricks.png", "new png bytes");
    graph.add("Scenes/a.swscene");
    graph.add("Scenes/b.swscene");
    CHECK(graph.lastReads() == 0);   // b's own files didn't change
    CHECK(graph.find("Scenes/a.swscene")->closureHash != a1);
    CHECK(graph.find("Content/materials/brick.swmaterial")->closureHash != material1);
    CHECK(graph.find("Scenes/b.swscene")->closureHash == b1);
    // A file nothing reaches changes nothing.
    const uint64_t a2 = graph.find("Scenes/a.swscene")->closureHash;
    write("Content/textures/other.png", "still unrelated");
    graph.add("Scenes/a.swscene");
    CHECK(graph.find("Scenes/a.swscene")->closureHash == a2);
    // A second graph over the same files agrees: hashes are content, not timestamps.
    AssetGraph again(root, root / "Content");
    again.add("Scenes/a.swscene");
    CHECK(again.find("Scenes/a.swscene")->closureHash == a2);
}

void testProblems() {
    write(
        "Scenes/broken.swscene",
        "shockwave_scene 2\n\nentity \"Gone\"\n  AudioSource\n    clip \"Audio/gone.wav\"\nend\n\n"
        "entity \"Case\"\n  MeshRenderer\n    texture \"Textures/bricks.png\"\nend\n\n"
        "entity \"Out\"\n  UIDocument\n    document \"../../outside.rml\"\nend\n\n"
        "entity \"Kind\"\n  ModelRenderer\n    model \"textures/bricks.png\"\nend\n\n"
        "entity \"Code\"\n  MeshRenderer\n    mesh made_in_code\nend\n");
    AssetGraph graph(root, root / "Content");
    graph.knownName = [](AssetKind, std::string_view name) { return name == "cube"; };
    graph.add("Scenes/broken.swscene");
    const auto problems = graph.diagnostics("Scenes/broken.swscene");
    const Diagnostic* missing = findCode(problems, "AssetMissing", "Audio/gone.wav");
    CHECK(
        missing && missing->file == "Scenes/broken.swscene" && missing->line == 5 &&
        missing->object == "entity 'Gone': AudioSource.clip");
    CHECK(findCode(problems, "AssetPathCase", "\"Textures\" as \"textures\""));
    CHECK(findCode(problems, "AssetPathOutside", "outside.rml"));
    CHECK(findCode(problems, "AssetKindConflict", "is a texture, not a model"));
    const Diagnostic* code = findCode(problems, "AssetUnknownName", "made_in_code");
    CHECK(code && !code->error());
    if (problems.size() != 5) dump(problems);
    CHECK(problems.size() == 5);
    // A broken file is unreadable, with the parser's reason.
    write("Content/materials/bad.swmaterial", "swmaterial 1\nroughness lots\n");
    graph.add("Content/materials/bad.swmaterial");
    CHECK(findCode(graph.diagnostics("Content/materials/bad.swmaterial"), "AssetUnreadable"));
}

void testCycles() {
    write("Content/materials/x.swmaterial", "swmaterial 1\nparent \"materials/y.swmaterial\"\n");
    write("Content/materials/y.swmaterial", "swmaterial 1\nparent \"materials/x.swmaterial\"\n");
    write(
        "Content/Prefabs/Loop.swprefab", "shockwave_prefab 1\n\nentity \"Loop\"\nend\n\nentity \"Inner\"\n  Prefab\n   "
                                         " path \"Prefabs/Loop.swprefab\"\nend\n");
    write(
        "Scenes/cycle.swscene",
        "shockwave_scene 2\n\nentity \"M\"\n  MeshRenderer\n    material \"materials/x.swmaterial\"\nend\n\n"
        "entity \"P\"\n  Prefab\n    path \"Prefabs/Loop.swprefab\"\nend\n");
    AssetGraph graph(root, root / "Content");
    graph.add("Scenes/cycle.swscene");
    CHECK(graph.cycles().size() == 2);
    const auto problems = graph.diagnostics("Scenes/cycle.swscene");
    const Diagnostic* materials = findCode(
        problems, "AssetCycle",
        "Content/materials/x.swmaterial -> Content/materials/y.swmaterial -> Content/materials/x.swmaterial");
    CHECK(materials && materials->object == "parent");
    CHECK(findCode(problems, "AssetCycle", "Content/Prefabs/Loop.swprefab -> Content/Prefabs/Loop.swprefab"));
    // Closure hashes still finish, and the closure still lists each once.
    const auto closure = graph.closure("Scenes/cycle.swscene");
    CHECK(std::count(closure.begin(), closure.end(), "Content/materials/x.swmaterial") == 1);
    // A scene that doesn't reach a cycle doesn't report it.
    graph.add("Scenes/b.swscene");
    CHECK(!findCode(graph.diagnostics("Scenes/b.swscene"), "AssetCycle"));
}

void testRegistry() {
    CHECK(assetKindOf("a/b.SWSCENE") == AssetKind::Scene);
    CHECK(assetKindOf("x.png") == AssetKind::Texture);
    CHECK(assetKindOf("x.efkefc") == AssetKind::Effect);
    CHECK(assetKindOf("noextension") == AssetKind::Other);
    const auto scene = assetProviders(".swscene");
    CHECK(has(scene, "scene") && has(scene, "navigation"));
    CHECK(has(assetProviders(".rml"), "rml"));
    CHECK(has(assetProviders(".swcircuit"), "circuit"));
    CHECK(assetPathField("Prefab", "path").has_value());
    CHECK(assetPathField("BakedLighting", "asset") && !assetPathField("BakedLighting", "asset")->required);
    CHECK(!assetPathField("Name", "value"));
}

}   // namespace

int main() {
    registeredComponents();
    registerAudioComponents();
    registerGameUIComponents();
    registerNavigationComponents();
    registerCircuitComponents();
    makeProject();
    testRegistry();
    testClosure();
    testHashesAndCache();
    testProblems();
    testCycles();
    fs::remove_all(root);
    if (failures) {
        std::fprintf(stderr, "asset_dependencies: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("asset_dependencies: all checks passed\n");
    return 0;
}
