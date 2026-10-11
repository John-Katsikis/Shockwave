// player --package: makes a game folder for players from the command line; see
// Packaging/Package.h and docs/Packaging.md.
#include "Player.h"

#include <Packaging/Package.h>

#include <cstdio>

namespace player {

int packageGame(const shockwave::Project& project, const PackageCommand& command) {
    shockwave::PackageSettings settings;
    std::string error;
    if (!shockwave::loadPackageSettings(project, settings, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    if (!command.output.empty()) settings.output = fs::absolute(command.output).lexically_normal();
    if (command.noBuild) settings.build = false;
    if (command.noArchive) settings.archive = false;

    shockwave::PackageOptions options;
    // Progress and results on standard output, for people and scripts alike; problems
    // are marked so they stand out.
    options.log = [](shockwave::LogLevel level, std::string_view line) {
        const char* mark = level == shockwave::LogLevel::Error     ? "error: "
                           : level == shockwave::LogLevel::Warning ? "warning: "
                                                                   : "";
        std::printf("%s%.*s\n", mark, static_cast<int>(line.size()), line.data());
        std::fflush(stdout);
    };
    shockwave::PackageStep shown = shockwave::PackageStep::Done;
    options.progress = [&shown](shockwave::PackageStep step, float) {
        if (step == shown || step == shockwave::PackageStep::Done) return;
        shown = step;
        std::printf("== %s\n", shockwave::packageStepName(step));
        std::fflush(stdout);
    };
    const shockwave::PackageResult result = shockwave::packageProject(project, settings, options);
    if (!result.ok) return 1;
    std::printf("package %s\n", result.folder.string().c_str());
    if (!result.archive.empty()) std::printf("archive %s\n", result.archive.string().c_str());
    std::printf("run %s\n", result.launcher.string().c_str());
    return 0;
}

fs::path packagedProjectFile() { return shockwave::packagedProject().value_or(fs::path()); }

void checkPackageSettings(const shockwave::Project& project, std::vector<std::string>& problems) {
    const fs::path file = shockwave::packageSettingsPath(project);
    if (!fs::exists(file)) return;
    shockwave::PackageSettings settings;
    std::string error;
    const size_t before = problems.size();
    if (!shockwave::loadPackageSettings(project, settings, error))
        problems.push_back(error);
    else
        for (std::string& problem : shockwave::validatePackageSettings(project, settings))
            problems.push_back(std::move(problem));
    if (problems.size() == before) std::printf("ok %s\n", project.relative(file).c_str());
}

}   // namespace player
