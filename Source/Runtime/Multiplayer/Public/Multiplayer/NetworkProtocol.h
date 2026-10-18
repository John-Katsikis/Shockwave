#pragma once

// Game-level identities and limits of multiplayer protocol v1: which numbers replication
// profiles, commands and events may use, and the per-peer budgets sessions enforce. The
// wire format itself (header, message catalog) is Network/Messages.h.

#include <Network/Messages.h>

#include <cstdint>

namespace shockwave::net {

// Replication profile IDs travel in every state record. 1..31 belong to the engine
// (names "engine.*"), 32..1023 to games.
inline constexpr uint16_t kFirstEngineProfile = 1;
inline constexpr uint16_t kLastEngineProfile = 31;
inline constexpr uint16_t kFirstGameProfile = 32;
inline constexpr uint16_t kLastGameProfile = 1023;

// Command and event IDs: 1..31 for the engine, 32 and up for games.
inline constexpr uint16_t kFirstGameCommand = 32;
inline constexpr uint16_t kFirstGameEvent = 32;

// Fields one profile, command or event may have.
inline constexpr uint32_t kMaxNetFields = 32;
// A command's own rate limit can only be stricter than the per-peer limit for all
// reliable gameplay commands (plan section 6.5).
inline constexpr uint16_t kMaxCommandsPerSecond = 20;
inline constexpr uint16_t kMaxCommandBurst = 10;
// Per-peer bytes per second kept for lifecycle messages and events, out of
// outbound_bytes_per_second; snapshots get the rest.
inline constexpr uint32_t kReservedControlBytesPerSecond = 16384;
// Bytes a Snapshot message spends before its records: the header plus snapshot_id,
// server_tick, required_control_serial, part_index, part_count and record_count.
inline constexpr uint32_t kSnapshotOverheadBytes = kHeaderBytes + 18;

}   // namespace shockwave::net
