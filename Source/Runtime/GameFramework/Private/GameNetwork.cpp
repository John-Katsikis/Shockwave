#include <GameFramework/GameNetwork.h>

namespace shockwave {

bool buildReplicationRegistry(
    const GameModule* module, net::ReplicationRegistry& registry, std::vector<std::string>& problems) {
    net::registerEngineReplication(registry);
    if (module && module->registerNetworkTypes) module->registerNetworkTypes(registry);
    return registry.freeze(problems);
}

}   // namespace shockwave
