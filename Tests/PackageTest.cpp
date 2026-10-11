// Package settings (Config/Package.swpackage): defaults, round trip, errors, validation.
// The whole packaging run is checked through the real CLI in PackageCheck.cmake.
#include <Packaging/Package.h>
#include <Projects/Project.h>

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

    fs::remove_all(root);
    if (failures == 0) std::puts("package tests passed");
    return failures == 0 ? 0 : 1;
}
