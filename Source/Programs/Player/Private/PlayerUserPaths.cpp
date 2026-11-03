// Per-user folders (docs/UserPaths.md): which ones this run uses, --user-paths, and
// --import-legacy-saves. None of these needs the game module, a window or a GPU.
#include "Player.h"

#include <GameFramework/SaveGame.h>
#include <Packaging/Package.h>
#include <Projects/Diagnostic.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <cstdio>
#include <fstream>

namespace player {

std::string applicationIdFor(const shockwave::Project& project) { return shockwave::projectApplicationId(project); }

bool resolveUserPaths(
    const shockwave::Project& project, const UserPathsCommand& command, shockwave::UserPaths& out, std::string& error) {
    shockwave::UserPathsRequest request;
    request.applicationId = applicationIdFor(project);
    request.profile = command.profile;
    request.overrideRoot = command.userDir;
    if (packagedProjectFile().empty()) {
        request.developmentProject = project.root;   // run from source: the project's Saved/
    } else {
        // An installed game: <package>/Game/<project>.swproject. Its folder is read-only.
        request.installRoot = project.root.parent_path();
    }
    return shockwave::UserPaths::resolve(request, out, error);
}

int userPathsInfo(
    const shockwave::Project& project, const UserPathsCommand& command, const shockwave::UserPaths* paths,
    const std::string& error, const fs::path& json) {
    using shockwave::UserFolder;
    constexpr UserFolder kFolders[] = {
        UserFolder::Config, UserFolder::Saves, UserFolder::Logs, UserFolder::Cache, UserFolder::Crashes};
    if (json.empty()) {
        if (!paths) {
            std::fprintf(stderr, "Player: %s\n", error.c_str());
            return 1;
        }
        std::printf(
            "source %s\napplication_id %s\nprofile %s\nroot %s\n", shockwave::userPathsSourceName(paths->source()),
            paths->applicationId().c_str(), paths->profile().c_str(), paths->root().string().c_str());
        for (const UserFolder folder : kFolders) {
            std::error_code ec;
            std::printf(
                "%-8s %s%s\n", shockwave::userFolderName(folder), paths->folder(folder).string().c_str(),
                fs::is_directory(paths->folder(folder), ec) ? "" : "  (not made yet)");
        }
        return 0;
    }
    auto str = [](const std::string& text) { return shockwave::diagnosticJsonString(text); };
    std::string out = "{\n  \"format\": 1,\n  \"mode\": \"user-paths\",\n";
    out += "  \"project\": " + str(project.name) + ",\n";
    out += "  \"application_id\": " + str(applicationIdFor(project)) + ",\n";
    out += "  \"profile\": " + str(command.profile) + ",\n";
    out += "  \"installed\": " + std::string(packagedProjectFile().empty() ? "false" : "true") + ",\n";
    if (!paths) {
        out += "  \"ok\": false,\n  \"error\": " + str(error) + "\n}\n";
    } else {
        out += "  \"ok\": true,\n  \"source\": " + str(shockwave::userPathsSourceName(paths->source())) + ",\n";
        out += "  \"root\": " + str(paths->root().generic_string()) + ",\n  \"folders\": {";
        for (size_t i = 0; i < std::size(kFolders); ++i) {
            std::error_code ec;
            const fs::path folder = paths->folder(kFolders[i]);
            out += std::string(i ? ", " : "") + str(shockwave::userFolderName(kFolders[i])) +
                   ": {\"path\": " + str(folder.generic_string()) +
                   ", \"exists\": " + (fs::is_directory(folder, ec) ? "true" : "false") + "}";
        }
        out += "}\n}\n";
    }
    if (json == "-") {
        std::fputs(out.c_str(), stdout);
    } else {
        std::ofstream file(json, std::ios::binary);
        file << out;
        file.close();
        if (!file) {
            std::fprintf(stderr, "Player: can't write %s\n", json.string().c_str());
            return 1;
        }
        std::printf("Wrote %s\n", json.string().c_str());
    }
    return paths ? 0 : 1;
}

int importLegacySaves(const fs::path& from, const shockwave::UserPaths& paths, bool preview) {
    std::printf(
        "%s save games from %s to %s (profile %s)\n", preview ? "would import" : "importing", from.string().c_str(),
        paths.saves().string().c_str(), paths.profile().c_str());
    shockwave::SaveImport result;
    std::string error;
    if (!shockwave::importSaveGames(from, paths.saves(), preview, result, error)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    for (const shockwave::SaveImport::Entry& entry : result.entries) {
        using Action = shockwave::SaveImport::Action;
        const char* verb = entry.action == Action::Copy ? "copy" : entry.action == Action::Keep ? "keep" : "skip";
        if (entry.reason.empty())
            std::printf("%s %s\n", verb, entry.slot.c_str());
        else
            std::printf("%s %s: %s\n", verb, entry.slot.c_str(), entry.reason.c_str());
    }
    std::printf(
        "%s %zu, kept %zu, failed %zu\n", preview ? "would copy" : "copied", result.copied, result.kept, result.failed);
    return result.failed ? 1 : 0;
}

}   // namespace player
