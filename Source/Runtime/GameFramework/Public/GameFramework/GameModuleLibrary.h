#pragma once

// GameModuleLibrary: a project's game module, loaded at runtime from
// bin/modules/game_<id>.so (or linked in, in non-modular builds), and reloaded when it's
// rebuilt. The editor uses it for hot reload of game code; the player loads the module
// its project names. See docs/Plugins.md.

#include <Core/Plugin.h>
#include <GameFramework/GameModule.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

class Assets;
class World;

class GameModuleLibrary {
public:
    // Loads the module and registers its component types. Null with error set when it
    // can't be found or loaded.
    static std::unique_ptr<GameModuleLibrary> load(std::string_view id, std::string& error);
    static std::unique_ptr<GameModuleLibrary> loadFile(const std::filesystem::path& file, std::string& error);

    // Unregisters the module's component types and unloads its code. Everything the module
    // made must be gone first: its GameInstance and GameModes, and every World holding its
    // components (reload() moves worlds across for you).
    ~GameModuleLibrary();
    GameModuleLibrary(const GameModuleLibrary&) = delete;
    GameModuleLibrary& operator=(const GameModuleLibrary&) = delete;

    const GameModule& module() const { return m_plugin->api<GameModule>(); }
    std::string_view id() const { return module().id; }
    // The library it was loaded from; empty when linked into the executable, which can't
    // reload.
    const std::filesystem::path& file() const { return m_plugin->path(); }
    bool reloadable() const { return !file().empty(); }
    // Component types it registered, by name.
    std::vector<std::string> componentNames() const;

private:
    GameModuleLibrary() = default;
    static std::unique_ptr<GameModuleLibrary> registerTypes(std::unique_ptr<Plugin> plugin);

    std::unique_ptr<Plugin> m_plugin;
    std::vector<std::pair<std::string, uint32_t>> m_components;   // name, component id
};

// Replaces `current` with a fresh load of the same module from disk. The worlds' components
// are carried across as scene text, so game components come back as the new code's types
// (with new fields at their defaults); anything scenes don't save (unregistered
// components) is dropped. Entity handles stay the same. Before
// calling, destroy what the old module made other than worlds (GameInstance, PlaySession),
// and make them again afterwards. On failure nothing changes and error says why: the old
// module keeps running.
bool reloadGameModule(
    std::unique_ptr<GameModuleLibrary>& current, std::span<World* const> worlds, const Assets& assets,
    std::string& error);

}   // namespace shockwave
