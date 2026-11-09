// Package settings (Config/Package.swpackage): defaults, round trip, errors, validation.
// A cancelled package and one with no engine to ship; the cook report the editor shows.
// The whole packaging run is checked through the real CLI in PackageCheck.cmake and
// PackageReleaseCheck.cmake.
#include <Packaging/Cook.h>
#include <Packaging/Package.h>
#include <Projects/Project.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}   // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "shockwave_package_test";
    fs::remove_all(root);
    write(root / "Scenes" / "main.swscene", "shockwave_scene 2\n");
    write(
        root / "Game.swproject",
        "shockwave_project 1\nname \"My Game!\"\ngame_module \"sample\"\nstartup_scene \"Scenes/main.swscene\"\n");
    Project project;
    std::string error;
    check(loadProject(root / "Game.swproject", project, error), "test project loads");

    // No file: defaults, named after the project.
    PackageSettings settings;
    settings.version = "changed";
    check(loadPackageSettings(project, settings, error) && error.empty(), "a missing file is fine");
    check(settings == PackageSettings{}, "a missing file gives the defaults");
    check(settings.displayName(project) == "My Game!", "name defaults to the project's");
    check(settings.executableName(project) == "MyGame", "executable drops spaces and punctuation");
    check(settings.outputFolder(project) == project.root / "Saved" / "Packages", "output defaults to Saved/Packages");
    check(validatePackageSettings(project, settings).empty(), "defaults are valid");
    check(packageFolderName(project, settings) == std::string("MyGame-1.0.0-") + packagePlatform(), "folder name");

    // Round trip through the file.
    settings.name = "Space Ace";
    settings.executable = "space-ace";
    settings.version = "2.1.0-beta";
    settings.output = "../Builds";
    settings.exclude = {"*.blend", "Content/Raw"};
    settings.build = false;
    settings.archive = false;
    settings.strip = false;
    settings.cppRuntime = false;
    check(savePackageSettings(project, settings, error), "settings save");
    PackageSettings loaded;
    check(loadPackageSettings(project, loaded, error), "settings load back");
    check(loaded == settings, "every setting survives a round trip");
    check(loaded.outputFolder(project) == (root / ".." / "Builds").lexically_normal(), "output is project relative");
    check(validatePackageSettings(project, loaded).empty(), "an output folder outside the project is fine");

    // Broken files say where, and leave the settings alone.
    const fs::path file = packageSettingsPath(project);
    auto fails = [&](const std::string& text, const char* expected, const char* message) {
        write(file, text);
        PackageSettings kept = loaded;
        const bool ok = loadPackageSettings(project, kept, error);
        check(!ok && contains(error, expected) && kept == loaded, message);
        if (ok || !contains(error, expected)) std::fprintf(stderr, "  got: %s\n", error.c_str());
    };
    fails("version \"1\"\n", "expected shockwave_package 1 first", "the header comes first");
    fails("shockwave_package 2\n", "unsupported", "unknown versions are refused");
    fails("shockwave_package 1\nicon \"a.png\"\n", ":2: unknown key 'icon'", "unknown keys are errors");
    fails("shockwave_package 1\nversion \"1\"\nversion \"2\"\n", ":3: duplicate key", "duplicate keys are errors");
    fails("shockwave_package 1\nbuild yes\n", "build must be true or false", "booleans are true or false");
    write(file, "shockwave_package 1\nexclude \"*.psd\"\nexclude *.blend \"Raw Files\"  # comment\n");
    check(loadPackageSettings(project, loaded, error) && loaded.exclude.size() == 3, "exclude can repeat");

    // Validation catches what would make a broken package.
    PackageSettings bad;
    bad.executable = "../game";
    bad.version = "1 0";
    bad.output = "Content/Builds";
    const auto problems = validatePackageSettings(project, bad);
    check(problems.size() == 3, "bad executable, version and output are reported");
    bad = {};
    bad.output = "Saved/Elsewhere";
    check(validatePackageSettings(project, bad).empty(), "any folder in Saved/ is fine");

    // Not a packaged game: nothing marks this test's folder as one.
    check(!packagedProject(), "a normal build has no packaged project");

    // Cancelled (from the editor's Cancel): it stops, and the last package stays as it was.
    {
        PackageSettings settings;
        settings.output = root / "Saved" / "Out";
        settings.build = false;
        const fs::path previous = settings.output / packageFolderName(project, settings);
        write(previous / "Binaries" / "shockwave-package.txt", "shockwave_package_info 1\n");
        write(previous / "Game" / "kept.txt", "the last good package\n");
        std::atomic<bool> cancel{true};
        PackageOptions options;
        options.engineFolder = root / "Engine";
        options.buildFolder = fs::path();
        options.cancel = &cancel;
        const PackageResult result = packageProject(project, settings, options);
        if (!contains(result.error, "cancelled")) std::fprintf(stderr, "  got: %s\n", result.error.c_str());
        check(!result.ok && contains(result.error, "cancelled"), "a cancelled package says so");
        check(fs::exists(previous / "Game" / "kept.txt"), "a cancelled package keeps the last one");
        check(!fs::exists(previous.string() + ".partial"), "a cancelled package leaves nothing half made");

        // No engine build to ship: it says where it looked, and what to do.
        fs::create_directories(root / "Engine");
        PackageOptions none;
        none.engineFolder = root / "Engine";
        none.buildFolder = fs::path();
        const PackageResult missing = packageProject(project, settings, none);
        check(
            !missing.ok && contains(missing.error, "There's no player at") && contains(missing.error, "Build it first"),
            "no engine build: the package says so");
        check(fs::exists(previous / "Game" / "kept.txt"), "a failed package keeps the last one");
    }

    // The cook report (the editor's Package window): nothing cooked, then two levels.
    {
        CookReport report = cookReport(root, packagePlatform(), true);
        check(!report.cooked && report.error.empty(), "no cook yet is not an error");
        write(root / "Content" / "a.png", "aaaa");
        write(root / "Content" / "b.png", "bb");
        CookManifest m;
        m.application = "My Game!";
        m.target = packagePlatform();
        m.entryLevel = "one";
        m.levels = {{"one", "Scenes/one.swscene", 1}, {"two", "Scenes/two.swscene", 2}};
        m.assets = {
            {"Content/a.png", "texture", 4, 0, 0, {"one", "two"}},
            {"Content/b.png", "texture", 2, 0, 0, {"two"}},
            {"builtin:cube", "builtin", 0, 0, 0, {"one"}}};
        m.computeIds();
        check(saveCookManifest(cookManifestPath(root, packagePlatform()), m, error), "a cook saves");
        report = cookReport(root, packagePlatform(), false);
        check(
            report.cooked && report.levels.size() == 2 && report.files == 2 && report.bytes == 6,
            "the report counts the cook's files and bytes");
        check(
            report.levels[0].assets == 2 && report.levels[0].shared == 1 && report.levels[0].bytes == 4,
            "level one: a.png (shared) and the built-in cube");
        check(
            report.levels[1].assets == 2 && report.levels[1].shared == 1 && report.levels[1].bytes == 6,
            "level two: a.png (shared) and b.png");
        check(!report.verified, "verify only when asked");
        // Hashes of 0 never match real bytes: verify lists both files as changed.
        report = cookReport(root, packagePlatform(), true);
        check(report.verified && report.changed.size() == 2, "verify names files changed since the cook");
        write(cookManifestPath(root, packagePlatform()), "swcook 1\nnot a manifest\n");
        report = cookReport(root, packagePlatform(), false);
        check(!report.cooked && !report.error.empty(), "a damaged cook is reported, not trusted");
    }

    fs::remove_all(root);
    if (failures == 0) std::puts("package tests passed");
    return failures == 0 ? 0 : 1;
}
