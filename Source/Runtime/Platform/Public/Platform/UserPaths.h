#pragma once

// Where a game writes: per-user folders for config, saves, logs, caches and crash reports,
// kept apart from the read-only install (docs/UserPaths.md, S01 of
// docs/ShippingImplementation.md). Nothing is ever written next to the game or in the
// current directory, and nothing is created until it's needed (ensure()).
//
// Three layouts, by where the root comes from:
//
//   Installed     the OS's per-user data folder, named by the game's application ID:
//                 ~/.local/share/<id>/ (Linux, $XDG_DATA_HOME first),
//                 %APPDATA%\<id>\ (Windows), ~/Library/Application Support/<id>/ (macOS).
//   Override      a folder given explicitly (player --user-dir), same layout. Tests and CI
//                 always use one, so they never touch real player data.
//   Development   a project run from its source folder (player, editor Play): the
//                 project's own Saved/ folder, the layout the engine always used there,
//                 so development never reads or writes a customer's profile.
//
// Installed and Override:          Development (<project>/Saved/):
//   <root>/profiles/<profile>/       SaveGames/           (profile "default")
//     Config/   settings            Profiles/<profile>/SaveGames/  (any other profile)
//     Saves/    save games          Config/
//   <root>/Logs/                     Logs/
//   <root>/Cache/                    Cache/
//   <root>/Crashes/                  Crashes/
//
// Profiles keep players apart: "default" is the local player; another profile (an account
// later) gets its own config and saves, never merged with someone else's. The profile ID is
// chosen explicitly and never derived from a display name or a project name.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace shockwave {

enum class UserPathsSource { Installed, Override, Development };
const char* userPathsSourceName(UserPathsSource source);   // "installed", "override", "development"

enum class UserFolder { Config, Saves, Logs, Cache, Crashes };
const char* userFolderName(UserFolder folder);   // "config", "saves", ...

struct UserPathsRequest {
    std::string applicationId;   // the release contract's application_id (or a fallback)
    std::string profile = "default";
    std::filesystem::path overrideRoot;   // --user-dir: wins over everything
    // The project's folder when it runs from source; empty for an installed game.
    std::filesystem::path developmentProject;
    // The read-only install (an installed game's package folder). An override inside it is
    // refused: player data never goes into the game's own files.
    std::filesystem::path installRoot;
    // Reads an environment variable (tests replace it); empty or unset both mean none.
    std::function<std::optional<std::filesystem::path>(const char*)> environment;
};

class UserPaths {
public:
    // Works out the folders. Fails, with a reason, when there's no per-user folder to use
    // (no HOME, say), the application or profile ID isn't valid, or an override is
    // inside the install. Creates nothing.
    static bool resolve(const UserPathsRequest& request, UserPaths& out, std::string& error);

    UserPathsSource source() const { return m_source; }
    const std::string& applicationId() const { return m_applicationId; }
    const std::string& profile() const { return m_profile; }
    const std::filesystem::path& root() const { return m_root; }
    std::filesystem::path folder(UserFolder folder) const;
    std::filesystem::path config() const { return folder(UserFolder::Config); }
    std::filesystem::path saves() const { return folder(UserFolder::Saves); }
    std::filesystem::path logs() const { return folder(UserFolder::Logs); }
    std::filesystem::path cache() const { return folder(UserFolder::Cache); }
    std::filesystem::path crashes() const { return folder(UserFolder::Crashes); }

    // Creates the folder (and the ones above it) if it's missing. A permission or disk
    // error comes back as text naming the folder; it's never swallowed.
    bool ensure(UserFolder folder, std::string& error) const;

private:
    UserPathsSource m_source = UserPathsSource::Development;
    std::string m_applicationId, m_profile = "default";
    std::filesystem::path m_root;
};

// A profile ID: 1 to 64 lowercase letters, digits, '-' and '_', starting with a letter or
// digit ("default", "steam-76561198000000000").
bool validUserProfile(std::string_view profile, std::string& why);

// The OS's per-user data folder, the one Installed roots are made in (without the
// application ID), from the environment given (the process's by default).
std::optional<std::filesystem::path>
userDataBase(const std::function<std::optional<std::filesystem::path>(const char*)>& environment = {});

// Whether `path` is `folder` or inside it, by their normal forms (no file system access).
bool pathInside(const std::filesystem::path& path, const std::filesystem::path& folder);

}   // namespace shockwave
