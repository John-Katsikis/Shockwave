#pragma once

// GameModule: how a game's C++ plugs into the editor and the player, like Unreal's
// primary game module. The project's .swproject names the module by id so the tools can
// tell if they match.
//
// Most games need two macros (see docs/Projects.md):
//
//     struct Health { float current = 100, max = 100; };
//     SW_COMPONENT(Health, SW_FIELD(current, 0, 1000), SW_FIELD(max, 1, 1000))
//
//     class MyGameMode : public GameMode { ... };
//     SW_GAME("mygame", MyGameMode)
//
// For full control, define projectGameModule() yourself instead of using SW_GAME.

#include <Engine/Components.h>
#include <GameFramework/GameInstance.h>

#include <functional>
#include <string>
#include <memory>
#include <string_view>
#include <type_traits>

// Builds with SHOCKWAVE_MODULAR off link every game module into one executable, so each
// game target gets its own names for the two symbols every module defines.
// shockwave_game_module() sets SW_GAME_MODULE_ID; nothing outside the module names them.
#if defined(SW_GAME_MODULE_ID)
#define SW_GAME_SYMBOL_INNER(name, id) name##_##id
#define SW_GAME_SYMBOL(name, id) SW_GAME_SYMBOL_INNER(name, id)
#define projectGameModule SW_GAME_SYMBOL(projectGameModule, SW_GAME_MODULE_ID)
#define AutoComponent SW_GAME_SYMBOL(AutoComponent, SW_GAME_MODULE_ID)
#endif

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

namespace detail {

// One per SW_COMPONENT. They chain into a list kept by the game module itself
// (GameModuleEntry.cpp), and are registered when the module loads, so they come and go
// with it on hot reload.
struct AutoComponent {
    void (*registerFn)();
    AutoComponent* next;
    explicit AutoComponent(void (*fn)()) : registerFn(fn), next(gameComponents()) { gameComponents() = this; }
    static AutoComponent*& gameComponents();
};

// jumpSpeed -> jump_speed: scene files name fields in snake_case, like the built-in ones.
inline std::string sceneFieldName(std::string_view member) {
    std::string name;
    for (const char c : member) {
        if (c >= 'A' && c <= 'Z') {
            if (!name.empty() && name.back() != '_') name += '_';
            name += static_cast<char>(c - 'A' + 'a');
        } else {
            name += c;
        }
    }
    return name;
}

// A field named after its member, with the editor's drag speed worked out from its range.
template <class C, class M>
Field autoField(const char* member, M C::*pointer, float min = 0.0f, float max = 0.0f) {
    return field(sceneFieldName(member), pointer, max > min ? (max - min) / 200.0f : 0.0f, min, max);
}

template <class T>
GameModule simpleGameModule(std::string_view id) {
    static_assert(
        std::is_base_of_v<GameMode, T> || std::is_base_of_v<GameInstance, T>,
        "SW_GAME takes a GameMode (every scene plays with it) or a GameInstance");
    GameModule module;
    module.id = id;
    if constexpr (std::is_base_of_v<GameInstance, T>)
        module.createGameInstance = [] { return std::make_unique<T>(); };
    else
        module.createGameInstance = [] { return std::make_unique<SimpleGameInstance<T>>(); };
    return module;
}

}   // namespace detail

}   // namespace shockwave

#define SW_CONCAT_INNER(a, b) a##b
#define SW_CONCAT(a, b) SW_CONCAT_INNER(a, b)

// Makes a struct a component: saved in scenes, editable in the Inspector, usable by AI
// tools (player --schema). List the fields to save with SW_FIELD; the rest is runtime
// state. Put it in a .cpp file of the game module, at namespace scope.
#define SW_COMPONENT(Type, ...) SW_COMPONENT_IN("Gameplay", Type __VA_OPT__(, ) __VA_ARGS__)
// The same, under another heading of the Inspector's Add Component menu.
#define SW_COMPONENT_IN(category, Type, ...)                                                                           \
    static const ::shockwave::detail::AutoComponent SW_CONCAT(swComponent_, __COUNTER__){[] {                          \
        using SwComponent = Type;                                                                                      \
        (void)sizeof(SwComponent);                                                                                     \
        ::shockwave::registerComponent<Type>(#Type, {__VA_ARGS__}, category);                                          \
    }};

// A saved field of the component, optionally with the editor's limits:
// SW_FIELD(speed) or SW_FIELD(speed, 0, 10). Scene files spell it in snake_case
// (SW_FIELD(jumpSpeed) is saved as jump_speed).
#define SW_FIELD(member, ...) ::shockwave::detail::autoField(#member, &SwComponent::member __VA_OPT__(, ) __VA_ARGS__)
// An int-backed enum field with a name for each value: SW_ENUM_FIELD(mode, "Walk", "Run").
#define SW_ENUM_FIELD(member, ...) ::shockwave::enumField(::shockwave::detail::sceneFieldName(#member), &SwComponent::member, {__VA_ARGS__})

// Declares the game module: its id (as in the .swproject's game_module) and the GameMode
// every scene plays with, or your own GameInstance to pick one per scene. Write it once,
// at global scope (outside any namespace).
#define SW_GAME(id, Type)                                                                                              \
    const ::shockwave::GameModule& shockwave::projectGameModule() {                                                    \
        static const ::shockwave::GameModule module = ::shockwave::detail::simpleGameModule<Type>(id);                 \
        return module;                                                                                                 \
    }
