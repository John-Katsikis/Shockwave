#include <GameFramework/GameModuleLibrary.h>

#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/SceneFile.h>
#include <Engine/World.h>

#include <algorithm>
#include <sstream>

namespace shockwave {

std::unique_ptr<GameModuleLibrary> GameModuleLibrary::registerTypes(std::unique_ptr<Plugin> plugin) {
    auto library = std::unique_ptr<GameModuleLibrary>(new GameModuleLibrary());
    library->m_plugin = std::move(plugin);
    // What the module registers shows up as entries that are new, or that it replaced.
    std::vector<std::pair<std::string, uint32_t>> before;
    for (const ComponentInfo& info : registeredComponents()) before.emplace_back(info.name, info.id);
    if (const auto& registerFn = library->module().registerTypes) registerFn();
    for (const ComponentInfo& info : registeredComponents()) {
        const std::pair<std::string, uint32_t> entry{info.name, info.id};
        if (std::find(before.begin(), before.end(), entry) == before.end()) library->m_components.push_back(entry);
    }
    return library;
}

std::unique_ptr<GameModuleLibrary> GameModuleLibrary::load(std::string_view id, std::string& error) {
    auto plugin = loadPlugin("game", id, error);
    return plugin ? registerTypes(std::move(plugin)) : nullptr;
}

std::unique_ptr<GameModuleLibrary> GameModuleLibrary::loadFile(const std::filesystem::path& file, std::string& error) {
    auto plugin = loadPluginFile(file, "game", error);
    return plugin ? registerTypes(std::move(plugin)) : nullptr;
}

GameModuleLibrary::~GameModuleLibrary() {
    for (const auto& [name, id] : m_components) detail::removeComponentInfo(name, id);
}

std::vector<std::string> GameModuleLibrary::componentNames() const {
    std::vector<std::string> names;
    for (const auto& component : m_components) names.push_back(component.first);
    return names;
}

bool reloadGameModule(
    std::unique_ptr<GameModuleLibrary>& current, std::span<World* const> worlds, const Assets& assets,
    std::string& error) {
    if (!current || !current->reloadable()) {
        error = "this game module is built into the executable, so it can't reload";
        return false;
    }
    // Save first: the new code registers its types under the same names, after which the
    // old components can't be found to write out.
    // The entities themselves are kept as they are, so every handle stays valid.
    std::vector<std::string> scenes;
    std::vector<World> entities;
    scenes.reserve(worlds.size());
    entities.reserve(worlds.size());
    for (const World* world : worlds) {
        std::ostringstream out;
        writeScene(out, *world, assets);
        scenes.push_back(std::move(out).str());
        entities.push_back(world->withoutComponents());
    }
    const std::filesystem::path file = current->file();
    auto next = GameModuleLibrary::loadFile(file, error);
    if (!next) return false;
    if (next->id() != current->id()) {
        error = file.string() + " is now the module \"" + std::string(next->id()) + "\"";
        next.reset();   // takes its types with it, including ones it re-registered...
        if (const auto& registerFn = current->module().registerTypes) registerFn();   // ...so put back the old ones
        return false;
    }
    // The old module's components live in these worlds: clear them while its code is loaded.
    for (World* world : worlds) *world = World{};
    current = std::move(next);   // unloads the old code
    for (size_t i = 0; i < worlds.size(); ++i) {
        std::istringstream in(scenes[i]);
        std::string readError;
        std::vector<std::string> warnings;
        SceneFileHooks hooks;
        hooks.entities = &entities[i];
        if (!readScene(*worlds[i], assets, in, "reloaded scene", readError, hooks, &warnings))
            SW_LOG(LogGame, Error, "game code reload lost a scene: {}", readError);
        for (const std::string& warning : warnings) SW_LOG(LogGame, Warning, "after reloading game code: {}", warning);
    }
    return true;
}

}   // namespace shockwave
