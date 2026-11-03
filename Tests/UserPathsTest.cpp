// Per-user folders (S01 of docs/ShippingImplementation.md, docs/UserPaths.md): the OS base
// from an injected environment, the three layouts, profiles, IDs, the install guard, paths
// with spaces and non-ASCII names, and ensure() creating only what's asked. No window or GPU.
#include <Platform/UserPaths.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

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

using Env = std::map<std::string, fs::path>;

std::function<std::optional<fs::path>(const char*)> environment(Env env) {
    return [env](const char* name) -> std::optional<fs::path> {
        auto it = env.find(name);
        if (it == env.end() || it->second.empty()) return std::nullopt;
        return it->second;
    };
}

fs::path absoluteTemp(const char* name) { return (fs::temp_directory_path() / name).lexically_normal(); }

// The variable that names this OS's per-user base, and where that base ends up.
#if defined(_WIN32)
const char* kBaseVar = "APPDATA";
fs::path expectedBase(const fs::path& value) { return value; }
#elif defined(__APPLE__)
const char* kBaseVar = "HOME";
fs::path expectedBase(const fs::path& value) { return value / "Library" / "Application Support"; }
#else
const char* kBaseVar = "HOME";
fs::path expectedBase(const fs::path& value) { return value / ".local" / "share"; }
#endif

void testIds() {
    std::string why;
    CHECK(validUserProfile("default", why));
    CHECK(validUserProfile("steam-76561198000000000", why));
    CHECK(validUserProfile("p2_local", why));
    CHECK(!validUserProfile("", why));
    CHECK(!validUserProfile("-x", why));
    CHECK(!validUserProfile("_x", why));
    CHECK(!validUserProfile("Player One", why));
    CHECK(!validUserProfile("../escape", why));
    CHECK(!validUserProfile(std::string(65, 'a'), why));
    CHECK(validUserProfile(std::string(64, 'a'), why));

    // A bad application ID is refused (an installed root is named by it).
    const fs::path home = absoluteTemp("sw-userpaths-home");
    for (const char* bad : {"", "Bad.Id", ".game", "game.", "a..b", "with space", "a/b"}) {
        UserPathsRequest request;
        request.applicationId = bad;
        request.environment = environment({{kBaseVar, home}});
        UserPaths paths;
        std::string error;
        CHECK(!UserPaths::resolve(request, paths, error));
        CHECK(!error.empty());
    }
    // A bad profile is refused for every source.
    UserPathsRequest request;
    request.applicationId = "com.example.game";
    request.profile = "Not Valid";
    request.developmentProject = home;
    UserPaths paths;
    std::string error;
    CHECK(!UserPaths::resolve(request, paths, error));
    CHECK(error.find("profile") != std::string::npos);
}

void testInstalled() {
    const fs::path base = absoluteTemp("sw-userpaths-base");
    UserPathsRequest request;
    request.applicationId = "com.example.game";
    request.installRoot = absoluteTemp("sw-userpaths-install");
    request.environment = environment({{kBaseVar, base}});
    UserPaths paths;
    std::string error;
    CHECK(UserPaths::resolve(request, paths, error));
    CHECK(error.empty());
    CHECK(paths.source() == UserPathsSource::Installed);
    CHECK(paths.applicationId() == "com.example.game");
    CHECK(paths.profile() == "default");
    CHECK(paths.root() == (expectedBase(base) / "com.example.game").lexically_normal());
    CHECK(paths.config() == paths.root() / "profiles" / "default" / "Config");
    CHECK(paths.saves() == paths.root() / "profiles" / "default" / "Saves");
    CHECK(paths.logs() == paths.root() / "Logs");
    CHECK(paths.cache() == paths.root() / "Cache");
    CHECK(paths.crashes() == paths.root() / "Crashes");
    CHECK(!pathInside(paths.root(), request.installRoot));

    // Another profile: its own config and saves, the shared logs/cache/crashes.
    request.profile = "p2";
    UserPaths second;
    CHECK(UserPaths::resolve(request, second, error));
    CHECK(second.saves() == second.root() / "profiles" / "p2" / "Saves");
    CHECK(second.saves() != paths.saves());
    CHECK(second.config() != paths.config());
    CHECK(second.cache() == paths.cache());

    // No base folder at all: a clear failure, not the current directory.
    request.environment = environment({});
    UserPaths none;
    CHECK(!UserPaths::resolve(request, none, error));
    CHECK(error.find("per-user data folder") != std::string::npos);

    // A relative base is never used.
    request.environment = environment({{kBaseVar, fs::path("relative") / "home"}});
    CHECK(!UserPaths::resolve(request, none, error));

#if !defined(_WIN32) && !defined(__APPLE__)
    // XDG_DATA_HOME wins when absolute and is ignored when relative.
    const fs::path xdg = absoluteTemp("sw-userpaths-xdg");
    request.environment = environment({{"HOME", base}, {"XDG_DATA_HOME", xdg}});
    UserPaths xdgPaths;
    CHECK(UserPaths::resolve(request, xdgPaths, error));
    CHECK(xdgPaths.root() == (xdg / "com.example.game").lexically_normal());
    request.environment = environment({{"HOME", base}, {"XDG_DATA_HOME", "relative/data"}});
    CHECK(UserPaths::resolve(request, xdgPaths, error));
    CHECK(xdgPaths.root() == (base / ".local" / "share" / "com.example.game").lexically_normal());
#endif
#if defined(_WIN32)
    // Without APPDATA, the roaming folder under USERPROFILE.
    request.environment = environment({{"USERPROFILE", base}});
    UserPaths roaming;
    CHECK(UserPaths::resolve(request, roaming, error));
    CHECK(roaming.root() == (base / "AppData" / "Roaming" / "com.example.game").lexically_normal());
#endif
}

void testOverrideAndDevelopment() {
    const fs::path install = absoluteTemp("sw-userpaths-install");
    UserPathsRequest request;
    request.applicationId = "com.example.game";
    request.installRoot = install;
    request.environment = environment({});   // the override needs no HOME

    // An override wins and uses the installed layout.
    request.overrideRoot = absoluteTemp("sw-userpaths-override");
    UserPaths paths;
    std::string error;
    CHECK(UserPaths::resolve(request, paths, error));
    CHECK(paths.source() == UserPathsSource::Override);
    CHECK(paths.saves() == request.overrideRoot / "profiles" / "default" / "Saves");

    // Never inside the install, however it's spelled.
    for (const fs::path& inside : {install, install / "Saved", install / "Binaries" / ".." / "Game" / "x"}) {
        request.overrideRoot = inside;
        UserPaths refused;
        CHECK(!UserPaths::resolve(request, refused, error));
        CHECK(error.find("inside the installed game") != std::string::npos);
    }
    // A sibling whose name only starts the same way is fine.
    request.overrideRoot = install.string() + "-user";
    CHECK(UserPaths::resolve(request, paths, error));

    // Development: the project's Saved/, the layout it always had.
    UserPathsRequest dev;
    dev.applicationId = "shockwave.sample";
    dev.developmentProject = absoluteTemp("sw-userpaths-project");
    dev.environment = environment({});
    CHECK(UserPaths::resolve(dev, paths, error));
    CHECK(paths.source() == UserPathsSource::Development);
    CHECK(paths.root() == dev.developmentProject / "Saved");
    CHECK(paths.saves() == dev.developmentProject / "Saved" / "SaveGames");
    CHECK(paths.config() == dev.developmentProject / "Saved" / "Config");
    dev.profile = "p2";
    CHECK(UserPaths::resolve(dev, paths, error));
    CHECK(paths.saves() == dev.developmentProject / "Saved" / "Profiles" / "p2" / "SaveGames");
}

void testPathInside() {
    const fs::path root = absoluteTemp("root");
    CHECK(pathInside(root, root));
    CHECK(pathInside(root / "a" / "b", root));
    CHECK(pathInside(root / "a" / ".." / "b", root));
    CHECK(!pathInside(root / ".." / "other", root));
    CHECK(!pathInside(root.string() + "2", root));
    CHECK(!pathInside(root.parent_path(), root));
}

void testEnsure() {
    // Spaces and non-ASCII in every part of the path.
    const fs::path base = absoluteTemp("sw userpaths") / fs::path(u8"Jos\u00e9 \u00fcser");
    std::error_code ec;
    fs::remove_all(base.parent_path(), ec);
    UserPathsRequest request;
    request.applicationId = "com.example.game";
    request.environment = environment({{kBaseVar, base}});
    UserPaths paths;
    std::string error;
    CHECK(UserPaths::resolve(request, paths, error));
    CHECK(!fs::exists(paths.root()));   // resolving creates nothing
    CHECK(paths.ensure(UserFolder::Saves, error));
    CHECK(fs::is_directory(paths.saves()));
    CHECK(!fs::exists(paths.config()));   // only what was asked for
    CHECK(!fs::exists(paths.logs()));
    CHECK(paths.ensure(UserFolder::Saves, error));   // twice is fine

    // A file where the folder should be: an error naming it, never swallowed.
    fs::create_directories(paths.root(), ec);
    { std::ofstream(paths.root() / "Logs") << "in the way"; }
    CHECK(!paths.ensure(UserFolder::Logs, error));
    CHECK(error.find("logs") != std::string::npos);
    fs::remove_all(base.parent_path(), ec);
}

}   // namespace

int main() {
    testIds();
    testInstalled();
    testOverrideAndDevelopment();
    testPathInside();
    testEnsure();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("user paths: all checks passed\n");
    return 0;
}
