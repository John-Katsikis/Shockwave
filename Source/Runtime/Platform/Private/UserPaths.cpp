// Per-user folders for installed games: see UserPaths.h and docs/UserPaths.md.
#include <Platform/UserPaths.h>

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <cwchar>
#endif

namespace shockwave {

namespace fs = std::filesystem;

namespace {

std::optional<fs::path> processEnvironment(const char* name) {
#if defined(_WIN32)
    // Wide, so a user name outside the ANSI code page still resolves.
    std::wstring wide;
    for (const char* c = name; *c; ++c) wide += wchar_t(*c);
    if (const wchar_t* value = _wgetenv(wide.c_str()); value && *value) return fs::path(value);
#else
    if (const char* value = std::getenv(name); value && *value) return fs::path(value);
#endif
    return std::nullopt;
}

// The same shape as the release contract's application_id: dot-separated parts of
// lowercase letters, digits, '-' and '_', so it names the same folder on every system.
bool validApplicationId(std::string_view id, std::string& why) {
    if (id.empty() || id.size() > 128) return why = "the application ID must be 1 to 128 characters", false;
    if (id.front() == '.' || id.back() == '.' || id.find("..") != std::string_view::npos)
        return why = "the application ID \"" + std::string(id) + "\" has an empty part", false;
    for (const char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return why = "the application ID \"" + std::string(id) +
                         "\" may only hold lowercase letters, digits, '-', '_' and '.'",
                   false;
    return true;
}

}   // namespace

const char* userPathsSourceName(UserPathsSource source) {
    switch (source) {
    case UserPathsSource::Installed: return "installed";
    case UserPathsSource::Override: return "override";
    case UserPathsSource::Development: return "development";
    }
    return "?";
}

const char* userFolderName(UserFolder folder) {
    switch (folder) {
    case UserFolder::Config: return "config";
    case UserFolder::Saves: return "saves";
    case UserFolder::Logs: return "logs";
    case UserFolder::Cache: return "cache";
    case UserFolder::Crashes: return "crashes";
    }
    return "?";
}

bool validUserProfile(std::string_view profile, std::string& why) {
    if (profile.empty() || profile.size() > 64) return why = "a profile ID is 1 to 64 characters", false;
    if (profile.front() == '-' || profile.front() == '_')
        return why = "a profile ID starts with a letter or a digit", false;
    for (const char c : profile)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return why = "the profile ID \"" + std::string(profile) +
                         "\" may only hold lowercase letters, digits, '-' and '_'",
                   false;
    return true;
}

std::optional<fs::path> userDataBase(const std::function<std::optional<fs::path>(const char*)>& environment) {
    const auto env = [&](const char* name) { return environment ? environment(name) : processEnvironment(name); };
#if defined(_WIN32)
    if (auto appData = env("APPDATA")) return *appData;
    if (auto profile = env("USERPROFILE")) return *profile / "AppData" / "Roaming";
    return std::nullopt;
#elif defined(__APPLE__)
    if (auto home = env("HOME")) return *home / "Library" / "Application Support";
    return std::nullopt;
#else
    // XDG: a relative XDG_DATA_HOME is invalid and ignored.
    if (auto data = env("XDG_DATA_HOME"); data && data->is_absolute()) return *data;
    if (auto home = env("HOME")) return *home / ".local" / "share";
    return std::nullopt;
#endif
}

bool pathInside(const fs::path& path, const fs::path& folder) {
    const fs::path rel = path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !rel.empty() && (rel == "." || *rel.begin() != "..");
}

bool UserPaths::resolve(const UserPathsRequest& request, UserPaths& out, std::string& error) {
    UserPaths result;
    if (!validUserProfile(request.profile, error)) return false;
    result.m_profile = request.profile;
    result.m_applicationId = request.applicationId;
    if (!request.overrideRoot.empty()) {
        std::error_code ec;
        result.m_root = fs::absolute(request.overrideRoot, ec).lexically_normal();
        if (ec) return error = "--user-dir " + request.overrideRoot.string() + ": " + ec.message(), false;
        if (!request.installRoot.empty() && pathInside(result.m_root, fs::absolute(request.installRoot, ec)))
            return error = "the user folder " + result.m_root.string() + " is inside the installed game (" +
                           request.installRoot.string() + "); player data never goes into the game's own files",
                   false;
        result.m_source = UserPathsSource::Override;
    } else if (!request.developmentProject.empty()) {
        std::error_code ec;
        result.m_root = (fs::absolute(request.developmentProject, ec) / "Saved").lexically_normal();
        result.m_source = UserPathsSource::Development;
    } else {
        if (!validApplicationId(request.applicationId, error)) return false;
        const auto base = userDataBase(request.environment);
        if (!base)
            return error =
#if defined(_WIN32)
                       "there's no per-user data folder (APPDATA isn't set)",
#else
                       "there's no per-user data folder (HOME isn't set)",
#endif
                   false;
        if (!base->is_absolute())
            return error = "the per-user data folder " + base->string() + " isn't an absolute path", false;
        result.m_root = (*base / request.applicationId).lexically_normal();
        result.m_source = UserPathsSource::Installed;
        std::error_code ec;
        if (!request.installRoot.empty() && pathInside(result.m_root, fs::absolute(request.installRoot, ec)))
            return error = "the per-user data folder " + result.m_root.string() + " is inside the installed game (" +
                           request.installRoot.string() + "); player data never goes into the game's own files",
                   false;
    }
    out = std::move(result);
    error.clear();
    return true;
}

fs::path UserPaths::folder(UserFolder folder) const {
    if (m_source == UserPathsSource::Development) {
        // The project's Saved/ folder, as it always was; other profiles beside the default.
        const fs::path profile = m_profile == "default" ? m_root : m_root / "Profiles" / m_profile;
        switch (folder) {
        case UserFolder::Config: return profile / "Config";
        case UserFolder::Saves: return profile / "SaveGames";
        case UserFolder::Logs: return m_root / "Logs";
        case UserFolder::Cache: return m_root / "Cache";
        case UserFolder::Crashes: return m_root / "Crashes";
        }
    }
    const fs::path profile = m_root / "profiles" / m_profile;
    switch (folder) {
    case UserFolder::Config: return profile / "Config";
    case UserFolder::Saves: return profile / "Saves";
    case UserFolder::Logs: return m_root / "Logs";
    case UserFolder::Cache: return m_root / "Cache";
    case UserFolder::Crashes: return m_root / "Crashes";
    }
    return m_root;
}

bool UserPaths::ensure(UserFolder which, std::string& error) const {
    const fs::path path = folder(which);
    std::error_code ec;
    if (fs::is_directory(path, ec)) return true;
    fs::create_directories(path, ec);
    if (ec || !fs::is_directory(path)) {
        error = "can't create the " + std::string(userFolderName(which)) + " folder " + path.string() + ": " +
                (ec ? ec.message() : std::string("it isn't a folder"));
        return false;
    }
    return true;
}

}   // namespace shockwave
