#pragma once

// Arena's network types (plan sections 6.3, 7 and 10): the player and rules profiles, the
// lobby commands and the match events, registered with SW_NETWORK_TYPES.

#include <cstdint>

namespace shockwave::net {
class ReplicationRegistry;
}

namespace arena {

inline constexpr uint16_t kRequestTeam = 32;   // {team u8}, while the round is waiting
inline constexpr uint16_t kReadyToPlay = 33;   // {ready u8}, while the round is waiting
inline constexpr uint16_t kRequestRoundRestart = 34;   // {}, host only, after the round
inline constexpr const char* kNetPlayerPrefab = "Prefabs/Net Player.swprefab";

void registerNetworkTypes(shockwave::net::ReplicationRegistry& registry);

}   // namespace arena
