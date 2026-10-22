#include <GameFramework/GameNetwork.h>

#include <Circuits/CircuitNodes.h>

namespace shockwave {

bool buildReplicationRegistry(
    const GameModule* module, net::ReplicationRegistry& registry, std::vector<std::string>& problems) {
    net::registerEngineReplication(registry);
    if (module && module->registerNetworkTypes) module->registerNetworkTypes(registry);
    if (!registry.freeze(problems)) return false;
    registerNetworkNodes(registry);   // Network.Command.* and Network.Event.* for circuits
    return true;
}

}   // namespace shockwave
