#pragma once

// Plugins: code the engine loads at runtime instead of linking in, so it can be swapped
// or rebuilt and reloaded while the editor runs. A renderer is a plugin, and so is a
// project's game module.
//
// A plugin is a shared library (bin/plugins/<kind>_<name>.so for engine plugins,
// bin/modules/game_<id>.so for game modules) that defines one PluginDesc with
// SHOCKWAVE_PLUGIN. What `api` points to depends on the kind: a RendererPluginApi for
// "renderer", a GameModule for "game".
//
// Builds made with SHOCKWAVE_MODULAR off (the default on Windows for now, and later for
// shipping) link every plugin into the executable instead. SHOCKWAVE_PLUGIN then
// registers it at startup, and everything below works the same, minus reloading.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace shockwave {

// Bump when a plugin-facing interface changes (RenderBackend, GameModule, the engine
// classes game code uses), so an engine refuses plugins built against an older one.
inline constexpr uint32_t kPluginAbiVersion = 15;   // 15: shading families (clearcoat, glass, skin, hair)

struct PluginDesc {
    uint32_t abiVersion = kPluginAbiVersion;
    const char* kind = "";   // "renderer", "game"
    const char* name = "";   // "vulkan", "sample": what projects and flags use to pick it
    const char* description = "";
    const void* api = nullptr;   // depends on kind
};

// A plugin the engine has loaded, or one linked into the executable.
class Plugin {
public:
    ~Plugin();
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    const PluginDesc& desc() const { return *m_desc; }
    template <class Api>
    const Api& api() const {
        return *static_cast<const Api*>(m_desc->api);
    }
    // The library it came from; empty when it's linked into the executable.
    const std::filesystem::path& path() const { return m_path; }
    bool builtIn() const { return m_path.empty(); }
    // The library was rebuilt since it was loaded (always false when built in).
    bool changedOnDisk() const;

private:
    friend std::unique_ptr<Plugin> loadPlugin(std::string_view, std::string_view, std::string&);
    friend std::unique_ptr<Plugin> loadPluginFile(const std::filesystem::path&, std::string_view, std::string&);
    Plugin() = default;

    const PluginDesc* m_desc = nullptr;
    void* m_library = nullptr;
    std::filesystem::path m_path;   // the library as built
    std::filesystem::path m_copy;   // the private copy actually loaded, so rebuilding never touches it
    std::filesystem::file_time_type m_time{};
};

// Finds and loads the plugin of this kind and name: one linked into the executable, or
// <kind>_<name> in bin/plugins (bin/modules for game modules). The library is loaded
// from a private copy, so the original can be rebuilt while it runs; loading it again
// after a rebuild gives the new code. Null with `error` set when it can't be loaded.
std::unique_ptr<Plugin> loadPlugin(std::string_view kind, std::string_view name, std::string& error);
// Loads a plugin library by path (the player's --game-module, tests).
std::unique_ptr<Plugin> loadPluginFile(const std::filesystem::path& path, std::string_view kind, std::string& error);

// Names of the plugins of a kind that loadPlugin can find, built-in ones first. Cheap:
// libraries are listed, not loaded.
std::vector<std::string> availablePlugins(std::string_view kind);
// Where loadPlugin looks for a plugin library: bin/plugins/<kind>_<name>.so (bin/modules
// for "game"), whether or not it exists.
std::filesystem::path pluginPath(std::string_view kind, std::string_view name);
// True when plugins are separate libraries that can be reloaded (SHOCKWAVE_MODULAR).
bool pluginsReloadable();

// Tells when a library has been rebuilt and is ready to load: its time stamp changed and
// then held still for `settle` seconds, so the linker has finished writing it. Each build
// is reported once, so a broken one isn't retried until the next rebuild. Main thread.
class RebuildWatcher {
public:
    explicit RebuildWatcher(double settleSeconds = 0.4) : m_settle(settleSeconds) {}
    // Starts watching `file` as it is now; empty stops watching.
    void watch(const std::filesystem::path& file);
    // Cheap enough to call every frame: it looks at the file a few times a second.
    // `now` is any steadily increasing time in seconds.
    bool poll(double now);
    const std::filesystem::path& file() const { return m_file; }

private:
    double m_settle;
    std::filesystem::path m_file;
    std::filesystem::file_time_type m_seen{};   // the build already loaded or reported
    std::filesystem::file_time_type m_candidate{};   // a newer build, waiting to settle
    double m_candidateSince = 0.0;
    double m_nextCheck = 0.0;
};

// The folder of the running executable.
std::filesystem::path executableDir();

namespace detail {
bool registerBuiltInPlugin(const PluginDesc* desc);
}

}   // namespace shockwave

// Defines the plugin: `desc` is a PluginDesc with static storage duration.
#if defined(SHOCKWAVE_MODULAR) && SHOCKWAVE_MODULAR
#if defined(_WIN32)
#define SHOCKWAVE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define SHOCKWAVE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif
#define SHOCKWAVE_PLUGIN(desc)                                                                                         \
    SHOCKWAVE_PLUGIN_EXPORT const ::shockwave::PluginDesc* shockwave_plugin() { return &(desc); }
#else
#define SHOCKWAVE_PLUGIN(desc)                                                                                         \
    namespace {                                                                                                        \
    [[maybe_unused]] const bool shockwavePluginRegistered = ::shockwave::detail::registerBuiltInPlugin(&(desc));       \
    }
#endif
