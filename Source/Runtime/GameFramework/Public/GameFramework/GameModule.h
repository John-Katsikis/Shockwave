#pragma once

// GameModule: how a game's C++ plugs into the editor and the player, like Unreal's
// primary game module. The project's library defines projectGameModule(); the
// project's .swproject names the module by id so the tools can tell if they match.

#include <GameFramework/GameInstance.h>

#include <functional>
#include <memory>
#include <string_view>

namespace shockwave {

struct GameModule {
    std::string_view id;
    // Registers the game's components (registerComponent) before any scene loads.
    std::function<void()> registerTypes;
    // Creates assets the game needs (meshes, textures) once the engine exists.
    std::function<void(Engine&)> createAssets;
    // The GameInstance for this run. Empty: a plain GameInstance.
    std::function<std::unique_ptr<GameInstance>()> createGameInstance;
};

// Implemented by the project library, linked into both editor and player.
const GameModule& projectGameModule();

// The module's GameInstance, or a plain one when it doesn't make its own.
inline std::unique_ptr<GameInstance> makeGameInstance(const GameModule* module) {
    if (module && module->createGameInstance)
        if (auto instance = module->createGameInstance()) return instance;
    return std::make_unique<GameInstance>();
}

} // namespace shockwave
