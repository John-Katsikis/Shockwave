// The release contract (docs/Release.md): --release-info, --release-schema and the
// contract's part of --check. None of them needs the game module, a window or a GPU.
#include "Player.h"

#include <Engine/GraphicsSettings.h>
#include <Multiplayer/NetworkSettings.h>
#include <Packaging/Cook.h>
#include <Packaging/Package.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <cstdio>
#include <fstream>

namespace player {

namespace net = shockwave::net;

std::string currentBuildId(const shockwave::Project& project) {
    // A package carries its cook beside the game; a project has its latest cook in Saved/.
    std::error_code ec;
    fs::path manifest = project.root / "Cook.swcook";
    if (!fs::is_regular_file(manifest, ec)) {
        const std::vector<std::string> targets = shockwave::cookTargets();
        if (targets.empty()) return {};
        manifest = shockwave::cookManifestPath(project.root, targets.front());
    }
    shockwave::CookManifest cook;
    std::string error;
    if (!fs::is_regular_file(manifest, ec) || !shockwave::loadCookManifest(manifest, cook, error)) return {};
    return shockwave::hex64(cook.buildId);
}

shockwave::ReleaseInputs
releaseInputs(const shockwave::Project& project, const std::map<std::string, shockwave::LevelSceneFacts>* levelFacts) {
    shockwave::ReleaseInputs inputs;
    inputs.projectTickRate = project.tickRate;
    inputs.buildConfig = shockwave::engineBuildConfig();

    // The version packages get: Config/Package.swpackage's, or its default without one. A
    // package file that doesn't read is --check's to report; its version isn't compared.
    shockwave::PackageSettings package;
    std::string error;
    inputs.packageFile = project.relative(shockwave::packageSettingsPath(project));
    if (shockwave::loadPackageSettings(project, package, error)) inputs.packageVersion = package.version;

    const fs::path network = net::networkSettingsPath(project);
    inputs.networkFile = project.relative(network);
    std::error_code ec;
    inputs.networkPresent = fs::exists(network, ec);
    if (inputs.networkPresent) {
        net::NetworkSettingsFile file;
        if (!net::readNetworkSettings(network, file, error)) {
            inputs.networkError = inputs.networkFile + " doesn't read (" + error + ")";
        } else {
            inputs.networkEnabled = file.settings.enabled;
            inputs.networkSteam = file.settings.transport == net::NetTransportKind::Steam;
            inputs.networkMaxPlayers = file.settings.maxPlayers;
            inputs.steamAppId = file.settings.steamAppId;
        }
    }

    shockwave::GraphicsSettings graphics;
    if (shockwave::projectGraphicsSettings(project, graphics, error))
        inputs.graphicsMemoryBudgetMb = graphics.memoryBudgetMb;
    shockwave::levelReleaseInputs(project, inputs, levelFacts);

    // Performance records (S10): Release/Performance/*.swperf, judged against this build.
    shockwave::PerformanceRecords records = shockwave::readPerformanceRecords(project);
    inputs.performanceChecked = true;
    inputs.performanceRecords = std::move(records.records);
    inputs.performanceFiles = std::move(records.files);
    inputs.performanceProblems = std::move(records.problems);
    inputs.currentBuildId = currentBuildId(project);

    // Where this game writes once installed (S01): the installed layout for its application
    // ID on this machine, kept out of the game's own folder (the project, from source).
    inputs.userPathsChecked = true;
    shockwave::UserPathsRequest request;
    request.applicationId = shockwave::projectApplicationId(project);
    request.installRoot = packagedProjectFile().empty() ? project.root : project.root.parent_path();
    shockwave::UserPaths paths;
    if (shockwave::UserPaths::resolve(request, paths, error)) {
        inputs.userPathsApplicationId = paths.applicationId();
        inputs.userPathsRoot = paths.root().string();
    } else {
        inputs.userPathsApplicationId = request.applicationId;
        inputs.userPathsProblem = error;
    }
    return inputs;
}

int releaseInfo(
    const shockwave::Project& project, const fs::path& json,
    const std::map<std::string, shockwave::LevelSceneFacts>* levelFacts) {
    const shockwave::ReleaseReport report = shockwave::releaseReport(project, releaseInputs(project, levelFacts));
    if (json.empty()) {
        std::fputs(shockwave::releaseReportText(report).c_str(), stdout);
    } else {
        const std::string text = shockwave::releaseReportJson(report, project.name);
        if (json == "-") {
            std::fputs(text.c_str(), stdout);
        } else {
            std::ofstream out(json, std::ios::binary);
            out << text;
            out.close();
            if (!out) {
                std::fprintf(stderr, "Player: can't write %s\n", json.string().c_str());
                return 1;
            }
            std::printf("Wrote %s\n", json.string().c_str());
        }
    }
    // A contract with errors fails; one that's valid but not qualified yet doesn't, since
    // the gates already say what's missing.
    return shockwave::hasErrors(report.diagnostics) ? 1 : 0;
}

int releaseSchema(const fs::path& file) {
    const std::string schema = shockwave::releaseSchema();
    if (file == "-") {
        std::fputs(schema.c_str(), stdout);
        return 0;
    }
    std::ofstream out(file, std::ios::binary);
    out << schema;
    out.close();
    if (!out) {
        std::fprintf(stderr, "Player: can't write %s\n", file.string().c_str());
        return 1;
    }
    return 0;
}

void checkRelease(
    const shockwave::Project& project, std::vector<std::string>& problems,
    const std::map<std::string, shockwave::LevelSceneFacts>* levelFacts) {
    const shockwave::ReleaseReport report = shockwave::releaseReport(project, releaseInputs(project, levelFacts));
    if (!report.present) return;   // a development project
    const size_t before = problems.size();
    for (const shockwave::Diagnostic& diagnostic : report.diagnostics) {
        if (diagnostic.error())
            problems.push_back(diagnostic.text());
        else
            std::printf("warning %s\n", diagnostic.text().c_str());
    }
    if (problems.size() == before) std::printf("ok %s\n", report.file.c_str());
}

}   // namespace player
