// The level catalog (S02 of docs/ShippingImplementation.md, docs/Levels.md): parsing and
// writing Config/Levels.swlevels, every structural mistake with its line, the checks
// against the disk (missing scenes, `..`, links leaving the project, case-only
// differences), the links between levels from scene facts, reachability, the implicit
// level of a project without a catalog, and the `levels` project key. No World needed.
#include <Projects/LevelCatalog.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

const char* kGood = "shockwave_levels 1\n"
                    "# the menu first\n"
                    "entry \"menu\"\n"
                    "level \"menu\"\n"
                    "  scene \"Scenes/menu.swscene\"\n"
                    "  kind menu\n"
                    "  next \"a\"\n"
                    "end\n"
                    "level \"a\"\n"
                    "  scene \"Scenes/a.swscene\"\n"
                    "  kind gameplay\n"
                    "  default_spawn \"start\"\n"
                    "  next \"b\"\n"
                    "  preload \"Content/UI/loading.rml\"\n"
                    "end\n"
                    "level \"b\"\n"
                    "  scene \"Scenes/b.swscene\"\n"
                    "  kind gameplay\n"
                    "  next \"menu\"   # a cycle: allowed\n"
                    "end\n";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at == std::string::npos) {
        std::fprintf(stderr, "test bug: '%s' not in the text\n", from.c_str());
        ++failures;
        return text;
    }
    return text.replace(at, from.size(), to);
}

const Diagnostic* find(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    for (const Diagnostic& d : diagnostics)
        if (d.code == code) return &d;
    return nullptr;
}

// Parses text that should fail with `code` at `line` (0: any line).
void expectError(const std::string& text, const std::string& code, int line) {
    LevelCatalog catalog;
    std::vector<Diagnostic> diagnostics;
    const bool ok = parseLevelCatalog(text, "Config/Levels.swlevels", catalog, diagnostics);
    const Diagnostic* d = find(diagnostics, code);
    CHECK(!ok);
    CHECK(d && d->error());
    if (d && line) CHECK(d->line == line);
    if (d) CHECK(d->file == "Config/Levels.swlevels");
    if (!d) {
        std::fprintf(stderr, "  expected %s; got:\n", code.c_str());
        for (const auto& x : diagnostics) std::fprintf(stderr, "    %s %s\n", x.code.c_str(), x.text().c_str());
    }
}

void testParse() {
    LevelCatalog catalog;
    std::vector<Diagnostic> diagnostics;
    CHECK(parseLevelCatalog(kGood, "Config/Levels.swlevels", catalog, diagnostics));
    CHECK(diagnostics.empty());
    CHECK(catalog.entry == "menu" && catalog.levels.size() == 3 && !catalog.implicit);
    const LevelEntry* a = catalog.find("a");
    CHECK(
        a && a->scene == "Scenes/a.swscene" && a->kind == LevelKind::Gameplay && a->defaultSpawn == "start" &&
        a->next == "b" && a->preload.size() == 1 && a->line == 9 && a->lines.at("next") == 13);
    CHECK(catalog.find("menu")->kind == LevelKind::Menu);
    // Writing and reading back gives the same catalog, and the same text again.
    const std::string text = levelCatalogText(catalog);
    LevelCatalog again;
    CHECK(parseLevelCatalog(text, "x", again, diagnostics));
    CHECK(again == catalog && levelCatalogText(again) == text);
    // CRLF line endings read the same.
    std::string crlf;
    for (const char c : std::string(kGood)) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    LevelCatalog windows;
    CHECK(parseLevelCatalog(crlf, "x", windows, diagnostics) && windows == catalog);
    // Paths are normalised: "Scenes/./a.swscene" is "Scenes/a.swscene".
    LevelCatalog normal;
    CHECK(parseLevelCatalog(
        replaced(kGood, "\"Scenes/a.swscene\"", "\"Scenes/./x/../a.swscene\""), "x", normal, diagnostics));
    CHECK(normal.find("a")->scene == "Scenes/a.swscene");
    // A failed parse leaves the destination alone.
    LevelCatalog kept = catalog;
    std::vector<Diagnostic> ignored;
    CHECK(!parseLevelCatalog("nonsense", "x", kept, ignored));
    CHECK(kept == catalog);

    CHECK(validLevelId("a") && validLevelId("factory_2") && validLevelId("x-y") && validLevelId(std::string(64, 'a')));
    CHECK(
        !validLevelId("") && !validLevelId("2a") && !validLevelId("A") && !validLevelId("a b") &&
        !validLevelId(std::string(65, 'a')) && !validLevelId("-a"));
    CHECK(validPersistentId("0f8fad5b-d9cb-469f-a165-70867728950e"));
    CHECK(!validPersistentId("0F8FAD5B-D9CB-469F-A165-70867728950E"));   // lowercase only
    CHECK(!validPersistentId("0f8fad5bd9cb469fa16570867728950e"));
    CHECK(!validPersistentId("0f8fad5b-d9cb-469f-a165-70867728950"));
    CHECK(!validPersistentId("0f8fad5b-d9cb-469f-a165-70867728950g"));
}

void testMistakes() {
    expectError("shockwave_level 1\n", "LevelHeader", 1);
    expectError("", "LevelHeader", 0);
    expectError("shockwave_levels 2\n", "LevelUnsupportedVersion", 1);
    expectError(replaced(kGood, "  kind menu\n", "  kind menu\n  colour \"red\"\n"), "LevelUnknownKey", 7);
    expectError(replaced(kGood, "entry \"menu\"\n", "entry \"menu\"\nstartup \"menu\"\n"), "LevelUnknownKey", 4);
    expectError(replaced(kGood, "entry \"menu\"\n", "entry \"menu\"\nentry \"a\"\n"), "LevelDuplicateKey", 4);
    expectError(replaced(kGood, "  kind menu\n", "  kind menu\n  kind ending\n"), "LevelDuplicateKey", 7);
    expectError(replaced(kGood, "level \"b\"", "level \"a\""), "LevelDuplicateId", 16);
    expectError(replaced(kGood, "level \"b\"", "level \"B\""), "LevelInvalidValue", 16);
    expectError(replaced(kGood, "kind menu", "kind cutscene"), "LevelInvalidValue", 6);
    expectError(replaced(kGood, "kind menu", "kind \"menu\""), "LevelInvalidValue", 6);
    expectError(replaced(kGood, "  scene \"Scenes/menu.swscene\"\n", ""), "LevelMissingKey", 0);
    expectError(replaced(kGood, "  kind menu\n", ""), "LevelMissingKey", 0);
    expectError(replaced(kGood, "\"Scenes/menu.swscene\"", "\"Scenes/menu.txt\""), "LevelInvalidValue", 5);
    expectError(replaced(kGood, "\"Scenes/menu.swscene\"", "\"../Other/menu.swscene\""), "LevelPathOutside", 5);
    expectError(replaced(kGood, "\"Scenes/menu.swscene\"", "\"Scenes/../../menu.swscene\""), "LevelPathOutside", 5);
    expectError(replaced(kGood, "\"Scenes/menu.swscene\"", "\"/tmp/menu.swscene\""), "LevelPathOutside", 5);
    expectError(replaced(kGood, "\"Content/UI/loading.rml\"", "\"../loading.rml\""), "LevelPathOutside", 14);
    expectError(
        replaced(
            kGood, "  preload \"Content/UI/loading.rml\"\n",
            "  preload \"Content/UI/loading.rml\"\n  preload \"Content/UI/loading.rml\"\n"),
        "LevelInvalidValue", 15);
    expectError(replaced(kGood, "entry \"menu\"\n", ""), "LevelNoEntry", 0);
    expectError(replaced(kGood, "entry \"menu\"", "entry \"title\""), "LevelUnknownEntry", 3);
    expectError(replaced(kGood, "next \"b\"", "next \"c\""), "LevelUnknownNext", 13);
    expectError(replaced(kGood, "default_spawn \"start\"", "default_spawn \"Start Here\""), "LevelInvalidValue", 12);
    expectError(replaced(kGood, "scene \"Scenes/menu.swscene\"", "scene Scenes/menu.swscene"), "LevelInvalidValue", 5);
    expectError(replaced(kGood, "end\nlevel \"a\"", "level \"a\""), "LevelStructure", 0);
    expectError(std::string(kGood) + "end\n", "LevelStructure", 21);
    expectError(std::string(kGood) + "  scene \"Scenes/x.swscene\"\n", "LevelStructure", 21);
    expectError(
        replaced(kGood, "kind gameplay\n  next \"menu\"   # a cycle: allowed\nend\n", "kind gameplay\n"),
        "LevelStructure", 0);
    expectError("shockwave_levels 1\nentry \"a\"\n", "LevelMissingKey", 0);
    expectError(replaced(kGood, "end\nlevel \"a\"", "end junk\nlevel \"a\""), "LevelInvalidValue", 8);
    // Every mistake in one file is reported, not just the first.
    LevelCatalog catalog;
    std::vector<Diagnostic> diagnostics;
    CHECK(!parseLevelCatalog(
        replaced(replaced(kGood, "kind menu", "kind nope"), "next \"b\"", "next \"zz\""), "x", catalog, diagnostics));
    CHECK(find(diagnostics, "LevelInvalidValue") && find(diagnostics, "LevelUnknownNext"));
}

LevelSceneFacts facts(std::vector<std::string> spawns, int cameras = 1) {
    LevelSceneFacts f;
    f.loaded = true;
    for (const std::string& s : spawns) f.spawns.push_back({s, "Spawn " + s});
    f.primaryCameras = cameras;
    return f;
}

void testLinks() {
    LevelCatalog catalog;
    std::vector<Diagnostic> diagnostics;
    CHECK(parseLevelCatalog(kGood, "Config/Levels.swlevels", catalog, diagnostics));
    std::map<std::string, LevelSceneFacts> good;
    good["menu"] = facts({}, 0);
    good["menu"].hasUserInterface = true;   // a menu drawn by UI alone needs no camera
    good["a"] = facts({"start", "halfway"});
    good["a"].exits.push_back({"b", "entrance", "Door", true});
    good["a"].checkpoints.push_back({"halfway", "halfway", "Checkpoint"});
    good["a"].identities.push_back({"3f2c9a1e-6b7d-4c58-9e0a-1b2c3d4e5f60", "Start", ""});
    good["b"] = facts({"entrance"});
    std::vector<Diagnostic> out;
    validateLevelLinks(catalog, good, "Config/Levels.swlevels", out);
    for (const auto& d : out) std::fprintf(stderr, "unexpected: %s\n", d.text().c_str());
    CHECK(out.empty());
    CHECK(reachableLevels(catalog, &good).size() == 3);

    auto expect = [&](std::map<std::string, LevelSceneFacts> f, const std::string& code, bool error = true) {
        std::vector<Diagnostic> found;
        validateLevelLinks(catalog, f, "Config/Levels.swlevels", found);
        const Diagnostic* d = find(found, code);
        CHECK(d && d->error() == error);
        if (!d) std::fprintf(stderr, "  expected %s\n", code.c_str());
        return d ? *d : Diagnostic{};
    };
    {
        auto f = good;
        f["a"].exits[0].destination = "nowhere";
        const Diagnostic d = expect(f, "LevelExitDestination");
        CHECK(d.file == "Scenes/a.swscene" && d.object == "Door" && d.message.find("nowhere") != std::string::npos);
    }
    {
        auto f = good;
        f["a"].exits[0].spawn = "back-door";
        const Diagnostic d = expect(f, "LevelExitSpawn");
        CHECK(d.message.find("Scenes/b.swscene") != std::string::npos);
    }
    {
        auto f = good;
        f["a"].spawns.erase(f["a"].spawns.begin());   // "start", the default spawn
        const Diagnostic d = expect(f, "LevelSpawnMissing");
        CHECK(d.file == "Config/Levels.swlevels" && d.line == 12);
    }
    {
        auto f = good;
        f["a"].spawns.push_back({"start", "Second start"});
        expect(f, "LevelDuplicateSpawn");
    }
    {
        auto f = good;
        f["a"].spawns.push_back({"Bad Id", "Odd"});
        expect(f, "LevelInvalidValue");
    }
    {
        auto f = good;
        f["a"].checkpoints[0].spawn = "nowhere";
        expect(f, "LevelCheckpointSpawn");
        f["a"].checkpoints[0].spawn = "halfway";
        f["a"].checkpoints.push_back({"halfway", "start", "Twin"});
        expect(f, "LevelCheckpoint");
    }
    {
        auto f = good;
        f["a"].identities.push_back({"3f2c9a1e-6b7d-4c58-9e0a-1b2c3d4e5f60", "Copy", ""});
        const Diagnostic d = expect(f, "LevelDuplicatePersistentId");
        CHECK(d.message.find("'Start'") != std::string::npos);
        f = good;
        f["a"].identities.push_back({"", "Bad", "PersistentIdentity \"x\" isn't a lowercase UUID"});
        expect(f, "LevelPersistentId");
        // The same identity in two levels is fine: identities are per level.
        f = good;
        f["b"].identities = f["a"].identities;
        std::vector<Diagnostic> none;
        validateLevelLinks(catalog, f, "x", none);
        CHECK(!find(none, "LevelDuplicatePersistentId"));
    }
    {
        // Cameras: gameplay needs one; a menu or ending needs one or a UIDocument.
        auto f = good;
        f["a"].primaryCameras = 0;
        const Diagnostic d = expect(f, "LevelNoCamera");
        CHECK(d.object == "a");
        f = good;
        f["a"].primaryCameras = 0;
        f["a"].hasUserInterface = true;   // UI doesn't excuse gameplay
        expect(f, "LevelNoCamera");
        f = good;
        f["menu"].hasUserInterface = false;
        expect(f, "LevelNoCamera");
    }
    {
        auto f = good;
        f["b"].loaded = false;
        f["b"].loadError = "Scenes/b.swscene:4: unknown component 'Wobble'";
        const Diagnostic d = expect(f, "LevelSceneBroken");
        CHECK(d.line == 16 && d.message.find("Wobble") != std::string::npos);
    }
    {
        // Unreachable: a warning, not an error. Exits count as links.
        LevelCatalog more;
        std::vector<Diagnostic> ignored;
        CHECK(parseLevelCatalog(
            std::string(kGood) + "level \"secret\"\n  scene \"Scenes/secret.swscene\"\n  kind gameplay\nend\n", "x",
            more, ignored));
        auto f = good;
        f["secret"] = facts({});
        std::vector<Diagnostic> found;
        validateLevelLinks(more, f, "Config/Levels.swlevels", found);
        const Diagnostic* d = find(found, "LevelUnreachable");
        CHECK(d && !d->error() && d->object == "secret" && d->line == 21);
        CHECK(!hasErrors(found));
        f["b"].exits.push_back({"secret", "", "Hatch", true});
        found.clear();
        validateLevelLinks(more, f, "x", found);
        CHECK(!find(found, "LevelUnreachable"));
        // A disabled exit doesn't make it reachable.
        f["b"].exits.back().enabled = false;
        found.clear();
        validateLevelLinks(more, f, "x", found);
        CHECK(find(found, "LevelUnreachable"));
        CHECK(reachableLevels(more, &f).size() == 3);
    }
}

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

void testProject() {
    const fs::path dir = fs::temp_directory_path() / "shockwave_level_catalog";
    fs::remove_all(dir);
    fs::create_directories(dir / "Scenes");
    write(dir / "Scenes" / "main.swscene", "");
    Project project;
    std::string error;
    CHECK(createProject(dir, "Level Test", "sample", project, error));
    // No catalog: the startup scene is the one implicit level, and nothing is written.
    LevelCatalog catalog;
    std::vector<Diagnostic> diagnostics;
    CHECK(project.levelCatalog.empty());
    CHECK(projectLevels(project, catalog, diagnostics) && diagnostics.empty());
    CHECK(catalog.implicit && catalog.entry == "startup" && catalog.levels.size() == 1);
    CHECK(catalog.levels[0].scene == "Scenes/main.swscene");
    CHECK(!fs::exists(dir / "Config" / "Levels.swlevels"));
    ReleaseInputs inputs;
    levelReleaseInputs(project, inputs);
    CHECK(inputs.levelCatalogChecked && inputs.levelCatalogImplicit);

    // The `levels` key: a .swlevels inside the project that exists.
    const fs::path manifest = project.file;
    std::string text;
    {
        std::ifstream in(manifest);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    auto withKey = [&](const std::string& value) {
        write(manifest, text + "levels \"" + value + "\"\n");
        Project loaded;
        std::string why;
        const bool ok = loadProject(manifest, loaded, why);
        return std::make_pair(ok, ok ? loaded : Project{});
    };
    CHECK(!withKey("Config/Levels.swlevels").first);   // doesn't exist yet
    write(dir / "Config" / "Levels.txt", "");
    CHECK(!withKey("Config/Levels.txt").first);
    CHECK(!withKey("../Levels.swlevels").first);
    write(dir / "Config" / "Levels.swlevels", kGood);
    auto [ok, withCatalog] = withKey("Config/Levels.swlevels");
    CHECK(ok && withCatalog.levelCatalog == (dir / "Config" / "Levels.swlevels").lexically_normal());
    // Saved projects keep the key.
    CHECK(saveProject(withCatalog, error));
    Project reloaded;
    CHECK(loadProject(manifest, reloaded, error) && reloaded.levelCatalog == withCatalog.levelCatalog);

    // Against the disk: everything missing at first.
    CHECK(projectLevels(reloaded, catalog, diagnostics));
    std::vector<Diagnostic> disk;
    validateLevelCatalog(reloaded, catalog, "Config/Levels.swlevels", disk);
    CHECK(find(disk, "LevelSceneMissing") && find(disk, "LevelPreloadMissing"));
    write(dir / "Scenes" / "menu.swscene", "");
    write(dir / "Scenes" / "a.swscene", "");
    write(dir / "Scenes" / "b.swscene", "");
    write(dir / "Content" / "UI" / "loading.rml", "");
    disk.clear();
    validateLevelCatalog(reloaded, catalog, "Config/Levels.swlevels", disk);
    for (const auto& d : disk) std::fprintf(stderr, "unexpected: %s\n", d.text().c_str());
    CHECK(disk.empty());
    inputs = {};
    levelReleaseInputs(reloaded, inputs);
    CHECK(
        inputs.levelCatalogChecked && !inputs.levelCatalogImplicit && inputs.levelCount == 3 &&
        inputs.levelCatalogProblem.empty() && !inputs.levelLinksChecked && inputs.levelEntry == "menu");

    // Spelt differently from the disk: fine on Windows and macOS, broken on Linux.
    LevelCatalog cased = catalog;
    cased.find("b")->scene = "scenes/b.swscene";
    disk.clear();
    validateLevelCatalog(reloaded, cased, "Config/Levels.swlevels", disk);
    const Diagnostic* d = find(disk, "LevelPathCase");
    CHECK(d && d->line == 17 && d->message.find("\"Scenes\"") != std::string::npos);
    // Two levels whose scenes differ only by case.
    write(dir / "Scenes" / "A.swscene", "");
    cased = catalog;
    cased.find("b")->scene = "Scenes/A.swscene";
    disk.clear();
    validateLevelCatalog(reloaded, cased, "Config/Levels.swlevels", disk);
    CHECK(find(disk, "LevelPathCase"));
    // Through a link out of the project.
    const fs::path outside = fs::temp_directory_path() / "shockwave_level_outside";
    fs::remove_all(outside);
    write(outside / "far.swscene", "");
    std::error_code ec;
    fs::create_directory_symlink(outside, dir / "Linked", ec);
    if (!ec) {
        LevelCatalog linked = catalog;
        linked.find("b")->scene = "Linked/far.swscene";
        disk.clear();
        validateLevelCatalog(reloaded, linked, "Config/Levels.swlevels", disk);
        const Diagnostic* escaped = find(disk, "LevelPathOutside");
        CHECK(escaped && escaped->message.find("link") != std::string::npos);
    } else {
        std::printf("skipped the symlink check: %s\n", ec.message().c_str());
    }

    // Written atomically; a temporary file never lingers.
    catalog.find("a")->next = "menu";
    CHECK(writeLevelCatalog(dir / "Config" / "Levels.swlevels", catalog, error));
    CHECK(!fs::exists(dir / "Config" / "Levels.swlevels.tmp"));
    LevelCatalog back;
    CHECK(readLevelCatalog(dir / "Config" / "Levels.swlevels", "x", back, diagnostics) && back == catalog);
    std::vector<Diagnostic> unreadable;
    CHECK(!readLevelCatalog(dir / "Config" / "Missing.swlevels", "x", back, unreadable));
    CHECK(find(unreadable, "LevelUnreadable"));
    fs::remove_all(dir);
    fs::remove_all(outside);
}

void testSchema() {
    const std::string schema = levelSchema();
    CHECK(schema.rfind("# Level catalog reference\n", 0) == 0);
    CHECK(schema == levelSchema());
    // Every code the catalog can report is in the schema, once.
    for (const char* code :
         {"LevelHeader",
          "LevelUnsupportedVersion",
          "LevelUnreadable",
          "LevelUnknownKey",
          "LevelDuplicateKey",
          "LevelStructure",
          "LevelMissingKey",
          "LevelInvalidValue",
          "LevelDuplicateId",
          "LevelNoEntry",
          "LevelUnknownEntry",
          "LevelUnknownNext",
          "LevelPathOutside",
          "LevelPathCase",
          "LevelSceneMissing",
          "LevelPreloadMissing",
          "LevelSceneBroken",
          "LevelSpawnMissing",
          "LevelDuplicateSpawn",
          "LevelExitDestination",
          "LevelExitSpawn",
          "LevelCheckpoint",
          "LevelCheckpointSpawn",
          "LevelPersistentId",
          "LevelDuplicatePersistentId",
          "LevelNoCamera",
          "LevelUnreachable",
          "LevelOptionConflict"}) {
        const std::string cell = std::string("| `") + code + "` |";
        const size_t first = schema.find(cell);
        CHECK(first != std::string::npos && schema.find(cell, first + 1) == std::string::npos);
        if (first == std::string::npos) std::fprintf(stderr, "  %s isn't in the schema\n", code);
    }
}

}   // namespace

int main() {
    testParse();
    testMistakes();
    testLinks();
    testProject();
    testSchema();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("level catalog: all checks passed\n");
    return 0;
}
