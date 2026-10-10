#include <Core/Plugin.h>

#include <Core/Log.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace shockwave {
namespace {

#if defined(_WIN32)
constexpr const char* kLibraryExtension = ".dll";
#elif defined(__APPLE__)
constexpr const char* kLibraryExtension = ".dylib";
#else
constexpr const char* kLibraryExtension = ".so";
#endif

std::vector<const PluginDesc*>& builtIns() {
    static std::vector<const PluginDesc*> plugins;
    return plugins;
}

const PluginDesc* findBuiltIn(std::string_view kind, std::string_view name) {
    for (const PluginDesc* desc : builtIns())
        if (kind == desc->kind && name == desc->name) return desc;
    return nullptr;
}

fs::path pluginFolder(std::string_view kind) { return executableDir() / (kind == "game" ? "modules" : "plugins"); }

std::string fileStem(std::string_view kind, std::string_view name) {
    return std::string(kind) + "_" + std::string(name);
}

fs::file_time_type modifiedTime(const fs::path& path) {
    std::error_code ec;
    const auto time = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type{} : time;
}

void* openLibrary(const fs::path& path, std::string& error) {
#if defined(_WIN32)
    void* library = LoadLibraryW(path.wstring().c_str());
    if (!library) error = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
#else
    // RTLD_LOCAL keeps the plugin's own symbols out of the global namespace, so a reloaded
    // copy never resolves to the old one; engine symbols still come from the executable.
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) error = dlerror();
#endif
    return library;
}

void* findSymbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void closeLibrary(void* library) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

// A private copy of the library to load. The loader keeps the file it loaded mapped, so
// a linker overwriting it in place would crash the program; with a copy, the build is
// free to replace the original at any time.
fs::path privateCopy(const fs::path& path, std::string& error) {
    static std::atomic<uint32_t> counter{0};
#if defined(_WIN32)
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    std::error_code ec;
    const fs::path folder = fs::temp_directory_path(ec) / ("shockwave-" + std::to_string(pid));
    fs::create_directories(folder, ec);
    const fs::path copy = folder / (path.stem().string() + "-" + std::to_string(counter++) + path.extension().string());
    fs::copy_file(path, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "couldn't copy " + path.string() + " to load it: " + ec.message();
        return {};
    }
    return copy;
}

}   // namespace

namespace detail {
bool registerBuiltInPlugin(const PluginDesc* desc) {
    builtIns().push_back(desc);
    return true;
}
}   // namespace detail

Plugin::~Plugin() {
    if (m_library) closeLibrary(m_library);
    if (!m_copy.empty()) {
        std::error_code ec;
        fs::remove(m_copy, ec);
    }
}

bool Plugin::changedOnDisk() const {
    if (builtIn()) return false;
    const auto time = modifiedTime(m_path);
    return time != fs::file_time_type{} && time != m_time;
}

std::unique_ptr<Plugin> loadPluginFile(const fs::path& path, std::string_view kind, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = path.string() + " doesn't exist";
        return nullptr;
    }
    auto plugin = std::unique_ptr<Plugin>(new Plugin());
    plugin->m_path = fs::absolute(path, ec).lexically_normal();
    plugin->m_time = modifiedTime(path);
    plugin->m_copy = privateCopy(path, error);
    if (plugin->m_copy.empty()) return nullptr;
    plugin->m_library = openLibrary(plugin->m_copy, error);
#if !defined(_WIN32)
    // Mapped now; the copy isn't needed on disk any more.
    fs::remove(plugin->m_copy, ec);
    plugin->m_copy.clear();
#endif
    if (!plugin->m_library) {
        error = "couldn't load " + path.string() + ": " + error;
        return nullptr;
    }
    using Entry = const PluginDesc* (*)();
    const auto entry = reinterpret_cast<Entry>(findSymbol(plugin->m_library, "shockwave_plugin"));
    if (!entry) {
        error = path.string() + " isn't a Shockwave plugin (no shockwave_plugin function)";
        return nullptr;
    }
    plugin->m_desc = entry();
    if (!plugin->m_desc || plugin->m_desc->abiVersion != kPluginAbiVersion) {
        error = path.string() + " was built for a different engine version; rebuild it";
        return nullptr;
    }
    if (kind != plugin->m_desc->kind) {
        error = path.string() + " is a " + std::string(plugin->m_desc->kind) + " plugin, not " + std::string(kind);
        return nullptr;
    }
    return plugin;
}

std::unique_ptr<Plugin> loadPlugin(std::string_view kind, std::string_view name, std::string& error) {
    if (const PluginDesc* desc = findBuiltIn(kind, name)) {
        auto plugin = std::unique_ptr<Plugin>(new Plugin());
        plugin->m_desc = desc;
        return plugin;
    }
    const fs::path path = pluginPath(kind, name);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "no " + std::string(kind) + " plugin named \"" + std::string(name) + "\" (looked for " + path.string() +
                ")";
        return nullptr;
    }
    auto plugin = loadPluginFile(path, kind, error);
    if (plugin && name != plugin->desc().name) {
        error = path.string() + " calls itself \"" + plugin->desc().name + "\", not \"" + std::string(name) + "\"";
        return nullptr;
    }
    return plugin;
}

std::vector<std::string> availablePlugins(std::string_view kind) {
    std::vector<std::string> names;
    for (const PluginDesc* desc : builtIns())
        if (kind == desc->kind) names.emplace_back(desc->name);
    const std::string prefix = std::string(kind) + "_";
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(pluginFolder(kind), ec)) {
        const fs::path& path = entry.path();
        const std::string stem = path.stem().string();
        if (path.extension() != kLibraryExtension || !stem.starts_with(prefix)) continue;
        std::string name = stem.substr(prefix.size());
        if (std::find(names.begin(), names.end(), name) == names.end()) found.push_back(std::move(name));
    }
    std::sort(found.begin(), found.end());
    names.insert(names.end(), found.begin(), found.end());
    return names;
}

fs::path pluginPath(std::string_view kind, std::string_view name) {
    return pluginFolder(kind) / (fileStem(kind, name) + kLibraryExtension);
}

bool pluginsReloadable() {
#if defined(SHOCKWAVE_MODULAR) && SHOCKWAVE_MODULAR
    return true;
#else
    return false;
#endif
}

void RebuildWatcher::watch(const fs::path& file) {
    m_file = file;
    m_seen = file.empty() ? fs::file_time_type{} : modifiedTime(file);
    m_candidate = {};
    m_nextCheck = 0.0;
}

bool RebuildWatcher::poll(double now) {
    if (m_file.empty() || now < m_nextCheck) return false;
    m_nextCheck = now + 0.1;
    const auto time = modifiedTime(m_file);
    if (time == fs::file_time_type{} || time == m_seen) {
        m_candidate = {};
        return false;
    }
    if (time != m_candidate) {   // a new write: wait for it to settle
        m_candidate = time;
        m_candidateSince = now;
        return false;
    }
    if (now - m_candidateSince < m_settle) return false;
    m_seen = time;
    m_candidate = {};
    return true;
}

fs::path executableDir() {
    static const fs::path dir = [] {
        std::error_code ec;
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        const DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return fs::path(std::wstring(buffer, size)).parent_path();
#elif defined(__APPLE__)
        char buffer[4096];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0) return fs::canonical(buffer, ec).parent_path();
        return fs::current_path(ec);
#else
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::current_path(ec) : self.parent_path();
#endif
    }();
    return dir;
}

}   // namespace shockwave
