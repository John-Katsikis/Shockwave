#pragma once

// player --net-test: the real session handshake between NetworkWorlds over an in-process
// LoopbackHub, on a virtual clock, with LoopbackOnlineServices for mutual authentication.
// No Steam, sockets, window or game code; the same seed gives the same run everywhere.
//
// Scenarios:
//   handshake   a host and `clients` clients (default 3) join through the `preset`'s
//               faults (default cafe), then everyone stops; nothing may be left open
//   rejections  one client for each way a join is refused or fails: protocol, content
//               and scene mismatches, a full server, a refused ticket, a ticket that's
//               never answered, and the host stopping mid-handshake
//   stop-start  100 rounds of host and client joining, then stopping in turn
//   replication a host with a moving authored box and spawned, respawned and despawned
//               crates owned by its players; `clients` clients (one joining late) must
//               end with the host's objects, owners and poses exactly
//
// The JSON follows the network tool layout of docs/Networking.md: version, mode,
// scenario, ok, errors, warnings, seed, duration_ticks, peers, counters.

#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/NetworkWorld.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace shockwave::net {

std::span<const char* const> netTestScenarios();

struct NetTestOptions {
    std::string scenario = "handshake";
    uint64_t seed = 1;
    std::string preset = "cafe";   // a FaultPreset name
    uint32_t clients = 3;
    NetworkSettings settings;   // tick_rate, max_players and timeouts; the transport is loopback
};

struct NetTestPeer {
    std::string name;   // "host", "client 1", "protocol mismatch", ...
    NetworkRole role = NetworkRole::Offline;
    NetworkState state = NetworkState::Offline;   // where it ended up before stopping
    ReasonCode reason = ReasonCode::Normal;   // its failure, if it failed
    ReasonCode expected = ReasonCode::Normal;   // rejections: the reason it should fail with
    uint16_t slot = 0;
    uint64_t peer = 0;
    double joinMs = -1;   // virtual time to Active (clients), -1 if it didn't
};

// Replication counters, summed over peers (objects: the most any peer had).
struct NetTestReplication {
    uint64_t objects = 0, spawns = 0, despawns = 0;
    uint64_t snapshotsSent = 0, snapshotPartsSent = 0, recordsApplied = 0, staleRecords = 0;
    uint64_t heldSnapshots = 0, expiredSnapshots = 0, resyncRequests = 0;
    uint32_t controlQueueHigh = 0;
    double maxPositionError = 0;   // replication: client vs host after traffic settled
};

struct NetTestReport {
    std::string scenario;
    uint64_t seed = 0;
    std::string preset;
    bool ok = false;
    std::vector<std::string> errors, warnings;
    uint32_t durationTicks = 0;
    std::vector<NetTestPeer> peers;
    uint32_t rounds = 0;   // stop-start
    uint64_t messagesSent = 0, messagesReceived = 0, rejected = 0, malformed = 0;
    uint64_t packetsInFlightAfterStop = 0;   // must be 0
    uint64_t openAuthSessionsAfterStop = 0, liveTicketsAfterStop = 0;   // must be 0
    NetTestReplication replication;
};

// False only for an unknown scenario or preset (errors say which); a scenario that runs
// and fails returns true with ok false.
bool runNetTest(const NetTestOptions& options, NetTestReport& report);
std::string netTestText(const NetTestReport& report);
std::string netTestJson(const NetTestReport& report);

}   // namespace shockwave::net
