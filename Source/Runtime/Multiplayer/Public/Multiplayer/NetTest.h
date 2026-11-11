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
//   prediction  a host and `clients` clients, each with Jolt physics and a predicted
//               character the host spawns for it; clients walk and jump by scripted
//               commands through the faults, then stand still: every client must end
//               where the host has it, with no correction offset left
//   map_rotation the host and `clients` clients change maps `rotations` times (default 3):
//               each client loads the next map for a seeded time, the host commits, and
//               everyone rejoins the next session; every client must be back, Active, in
//               its own slot, on the right map epoch, with nothing of the old session left
//   match       a measured match (S11): the host and `clients` clients, each a predicted
//               character on Jolt physics, walk, jump and shoot one another through the
//               faults; the host resolves the shots with lag compensation, respawns whoever
//               is hit and tells everyone the score. The last client joins 3 s late. After
//               10 s of play the map rotates and everyone plays 4 s more, then stands still.
//               Every client must be back in its slot with every score event, its own
//               character where the host has it and every other one drawn there too.
//               Reports host and client CPU per tick, physics, bytes each way, queues,
//               memory and update degradation, for 2 to 64 players (docs/Networking.md)
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
    uint32_t rotations = 3;   // map_rotation
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

// Map rotation counters (map_rotation).
struct NetTestRotation {
    uint32_t rotations = 0;   // committed and rejoined
    uint64_t slotsReclaimed = 0, drops = 0, refusals = 0, rejoinAttempts = 0;
    double maxRotationMs = 0;   // announced until every client was Active on the next map
    double meanRotationMs = 0;
};

// The measured match (match). Rates and times are over the 10 s of play before the map
// changes, with everyone in; CPU is wall-clock time in this one process (every machine
// stepped in turn on one thread), so it varies with the computer, unlike the rest.
struct NetTestMatch {
    uint32_t players = 0;   // the host included
    double measuredSeconds = 0;
    double hostTickMs = 0, hostTickP99Ms = 0, hostTickMaxMs = 0;   // everything the host did per tick
    double hostPhysicsMs = 0, hostNetworkMs = 0;   // its share in Jolt and in NetworkWorld
    double clientTickMs = 0, clientTickMaxMs = 0;   // the mean client, and the slowest tick of any
    double hostUpBytesPerSecond = 0, hostDownBytesPerSecond = 0;   // payload over every connection
    double hostUpPerClient = 0;   // the mean connection
    double clientDownBytesPerSecond = 0, clientUpBytesPerSecond = 0;   // the mean client
    double hostMessagesPerSecond = 0;   // sent: transport overhead is per message
    uint32_t controlQueueHigh = 0, pendingReliableHigh = 0, pendingUnreliableHigh = 0;   // bytes, any peer
    uint64_t hostMemoryBytes = 0, clientMemoryBytes = 0;   // session buffers, the most seen
    uint64_t recordsLimited = 0, updateLevelRaises = 0, updateLevelDrops = 0;
    uint32_t degradedPeersHigh = 0;
    uint64_t shots = 0, hits = 0, refusedShots = 0, respawns = 0, scoreEvents = 0;
    double lateJoinMs = -1, rotationMs = -1;
    double eventsSettledMs = 0;   // after play stopped, until every client had every score event
    double finalError = 0, finalPresentationError = 0;   // metres, after standing still
};

// Prediction counters, summed over clients.
struct NetTestPrediction {
    uint64_t predictedTicks = 0, checks = 0, matches = 0, corrections = 0, replayedTicks = 0, resets = 0;
    uint64_t catchUpInputs = 0, hostFallbacks = 0;
    uint64_t hostContacts = 0;   // the host's trigger events (clients must see none)
    double maxCorrection = 0;   // metres
    double maxOffset = 0;   // the largest correction offset shown
    double finalError = 0;   // the largest client-host distance after standing still
    double finalPresentationError = 0;   // the same for where clients draw every character
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
    NetTestPrediction prediction;
    NetTestRotation rotation;
    NetTestMatch match;
};

// False only for an unknown scenario or preset (errors say which); a scenario that runs
// and fails returns true with ok false.
bool runNetTest(const NetTestOptions& options, NetTestReport& report);
std::string netTestText(const NetTestReport& report);
std::string netTestJson(const NetTestReport& report);

}   // namespace shockwave::net
