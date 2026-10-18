#pragma once

// The replication registry for a game: what the application builds before checking a
// project or starting a session (docs/Networking.md).

#include <GameFramework/GameModule.h>
#include <Multiplayer/Replication.h>

#include <string>
#include <vector>

namespace shockwave {

// Fills `registry` with the engine's profiles, then the game module's
// (registerNetworkTypes, when it has any), and freezes it. False with every problem
// listed when registration or freezing fails; nothing networked may start then.
bool buildReplicationRegistry(
    const GameModule* module, net::ReplicationRegistry& registry, std::vector<std::string>& problems);

}   // namespace shockwave
