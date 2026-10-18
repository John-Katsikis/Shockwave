// Compiled into every game module (shockwave_game_module in CMake): makes the module's
// projectGameModule() loadable as the plugin game_<id>, and registers its SW_COMPONENTs
// along with whatever its own registerTypes does, and its SW_NETWORK_TYPES the same way.

#include <Core/Plugin.h>
#include <GameFramework/GameModule.h>

#include <vector>

namespace shockwave {

detail::AutoComponent*& detail::AutoComponent::gameComponents() {
    static AutoComponent* head = nullptr;
    return head;
}

detail::AutoNetworkTypes*& detail::AutoNetworkTypes::gameNetworkTypes() {
    static AutoNetworkTypes* head = nullptr;
    return head;
}

namespace {

const GameModule& entryModule() {
    static const GameModule module = [] {
        GameModule result = projectGameModule();
        result.registerTypes = [own = result.registerTypes] {
            // The list is newest first; register in the order they appear in the code.
            std::vector<void (*)()> components;
            for (auto* c = detail::AutoComponent::gameComponents(); c; c = c->next) components.push_back(c->registerFn);
            for (auto it = components.rbegin(); it != components.rend(); ++it) (*it)();
            if (own) own();
        };
        result.registerNetworkTypes = [own = result.registerNetworkTypes](net::ReplicationRegistry& registry) {
            std::vector<void (*)(net::ReplicationRegistry&)> types;
            for (auto* t = detail::AutoNetworkTypes::gameNetworkTypes(); t; t = t->next) types.push_back(t->registerFn);
            for (auto it = types.rbegin(); it != types.rend(); ++it) (*it)(registry);
            if (own) own(registry);
        };
        return result;
    }();
    return module;
}

const PluginDesc plugin{
    .kind = "game",
    .name = entryModule().id.data(),   // ids are string literals, so this ends in a NUL
    .description = "game module",
    .api = &entryModule(),
};

}   // namespace
}   // namespace shockwave

SHOCKWAVE_PLUGIN(shockwave::plugin)
