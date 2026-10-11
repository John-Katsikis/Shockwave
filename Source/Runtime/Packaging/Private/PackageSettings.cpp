// Config/Package.swpackage: reading, writing and validating package settings.
#include <Packaging/Package.h>

#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

// Letters, digits, '_', '-' and '.', not starting with '.' or '-': safe as a file name on
// every platform and in a shell.
bool safeFileName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.' || name[0] == '-') return false;
    for (const char c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    return true;
}

bool inside(const fs::path& path, const fs::path& folder) {
    const fs::path rel = path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

}   // namespace

std::string PackageSettings::displayName(const Project& project) const { return name.empty() ? project.name : name; }

std::string PackageSettings::executableName(const Project& project) const {
    if (!executable.empty()) return executable;
    std::string result;
    for (const char c : displayName(project))
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') result += c;
    while (!result.empty() && result[0] == '-') result.erase(0, 1);
    return result.empty() ? std::string("Game") : result;
}

fs::path PackageSettings::outputFolder(const Project& project) const {
    if (output.empty()) return project.statePath() / "Packages";
    return output.is_absolute() ? output.lexically_normal() : (project.root / output).lexically_normal();
}

fs::path packageSettingsPath(const Project& project) { return project.root / "Config" / "Package.swpackage"; }

bool loadPackageSettings(const Project& project, PackageSettings& settings, std::string& error) {
    error.clear();
    const fs::path path = packageSettingsPath(project);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        settings = PackageSettings{};
        return true;
    }
    std::ifstream in(path);
    if (!in) {
        error = "can't open " + path.string();
        return false;
    }
    PackageSettings result;
    std::set<std::string> seen;
    bool versionSeen = false;
    std::string line;
    int number = 0;
    auto fail = [&](const std::string& message) {
        error = project.relative(path) + ":" + std::to_string(number) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        std::istringstream row(line);
        std::string key;
        if (!(row >> key) || key[0] == '#') continue;
        // exclude may repeat; every other key appears once.
        if (key != "exclude" && !seen.insert(key).second) return fail("duplicate key '" + key + "'");
        if (key == "shockwave_package") {
            int version = 0;
            if (!(row >> version) || version != 1) return fail("unsupported package settings version");
            versionSeen = true;
            continue;
        }
        if (!versionSeen) return fail("expected shockwave_package 1 first");
        if (key == "name" || key == "executable" || key == "version" || key == "output") {
            std::string value;
            if (!(row >> std::quoted(value)) || value.empty()) return fail("empty or invalid " + key);
            if (key == "name")
                result.name = value;
            else if (key == "executable")
                result.executable = value;
            else if (key == "version")
                result.version = value;
            else
                result.output = value;
        } else if (key == "exclude") {
            row >> std::ws;
            while (row.peek() != EOF && row.peek() != '#') {
                std::string pattern;
                if (!(row >> std::quoted(pattern)) || pattern.empty()) return fail("exclude takes file patterns");
                result.exclude.push_back(pattern);
                row >> std::ws;
            }
        } else if (key == "build" || key == "archive" || key == "strip" || key == "cpp_runtime") {
            std::string value;
            row >> value;
            if (value != "true" && value != "false") return fail(key + " must be true or false");
            (key == "build"     ? result.build
             : key == "archive" ? result.archive
             : key == "strip"   ? result.strip
                                : result.cppRuntime) = value == "true";
        } else
            return fail("unknown key '" + key + "'");
        row >> std::ws;
        if (row.peek() != EOF && row.peek() != '#') return fail("unexpected trailing value");
    }
    if (in.bad()) return fail("error reading the file");
    if (!versionSeen) return fail("expected shockwave_package 1 first");
    settings = std::move(result);
    return true;
}

bool savePackageSettings(const Project& project, const PackageSettings& settings, std::string& error) {
    const fs::path path = packageSettingsPath(project);
    std::ostringstream out;
    out << "# How this project becomes a game folder for players: `player --package`, or Build >\n"
        << "# Package game in the editor. See docs/Packaging.md.\n"
        << "shockwave_package 1\n";
    if (!settings.name.empty()) out << "name " << std::quoted(settings.name) << "\n";
    if (!settings.executable.empty()) out << "executable " << std::quoted(settings.executable) << "\n";
    out << "version " << std::quoted(settings.version) << "\n";
    if (!settings.output.empty()) out << "output " << std::quoted(settings.output.generic_string()) << "\n";
    if (!settings.exclude.empty()) {
        out << "exclude";
        for (const std::string& pattern : settings.exclude) out << " " << std::quoted(pattern);
        out << "\n";
    }
    out << "build " << (settings.build ? "true" : "false") << "\n"
        << "archive " << (settings.archive ? "true" : "false") << "\n"
        << "strip " << (settings.strip ? "true" : "false") << "\n"
        << "cpp_runtime " << (settings.cppRuntime ? "true" : "false") << "\n";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!(file << out.str()) || !file.flush()) {
            error = "can't write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        error = "can't replace " + path.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::vector<std::string> validatePackageSettings(const Project& project, const PackageSettings& settings) {
    std::vector<std::string> problems;
    const std::string file = project.relative(packageSettingsPath(project));
    for (const char c : settings.displayName(project)) {
        if (static_cast<unsigned char>(c) < 32 || c == '"') {
            problems.push_back(file + ": name can't hold quotes or control characters");
            break;
        }
    }
    if (!safeFileName(settings.executableName(project)))
        problems.push_back(
            file + ": executable \"" + settings.executableName(project) +
            "\" must be letters, digits, '_', '-' or '.', and not start with '.' or '-'");
    if (!safeFileName(settings.version))
        problems.push_back(
            file + ": version \"" + settings.version + "\" must be letters, digits, '_', '-' or '.', like 1.0.0");
    // A package inside the project would be copied into the next package.
    const fs::path output = settings.outputFolder(project);
    if (inside(output, project.root) && !inside(output, project.statePath()))
        problems.push_back(
            file + ": output " + project.relative(output) +
            " is inside the project; use a folder in Saved/ or outside the project");
    for (const fs::path& folder : {project.assetFolder, project.sceneFolder}) {
        if (!inside(folder, project.root))
            problems.push_back(
                project.relative(project.file) + ": " + folder.string() +
                " is outside the project folder, so it can't be packaged with it");
    }
    return problems;
}

const char* packagePlatform() {
#if defined(_WIN32)
    return "windows-x86_64";
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "macos-arm64";
#else
    return "macos-x86_64";
#endif
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x86_64";
#endif
}

std::string packageFolderName(const Project& project, const PackageSettings& settings) {
    return settings.executableName(project) + "-" + settings.version + "-" + packagePlatform();
}

const char* packageStepName(PackageStep step) {
    switch (step) {
    case PackageStep::Build: return "Build";
    case PackageStep::Check: return "Check";
    case PackageStep::Copy: return "Copy";
    case PackageStep::Verify: return "Verify";
    case PackageStep::Archive: return "Archive";
    case PackageStep::Done: return "Done";
    }
    return "?";
}

}   // namespace shockwave
