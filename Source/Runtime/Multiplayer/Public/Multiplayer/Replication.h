#pragma once

// The replication registry: every replication profile, command and event a session can
// use, with stable wire IDs. The engine registers its profiles (registerEngineReplication),
// a game module adds its own (GameModule::registerNetworkTypes), and freeze() checks the
// whole catalog and builds the lookup tables. A session only starts with a frozen
// registry, and peers compare its digest (schemaDigest) before joining.
//
// A profile is the state one kind of network object replicates: an ordered list of typed
// fields with ranges. That list is the wire layout (encodeProfile/decodeProfile), the
// generated docs/NetworkSchema.md and the --check rules, so they can't disagree. The
// profile's capture and apply functions move values between the World and a
// ProfileValue; neither they nor the codecs allocate, look fields up by name or go
// through scene text.
//
// Commands are what clients may ask the host to do (intent, never outcomes); events are
// what the host tells clients happened. Neither is a way to call arbitrary code.

#include <Multiplayer/NetworkProtocol.h>

#include <Engine/World.h>
#include <Network/Messages.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave::net {

enum class NetFieldType : uint8_t { Bool, U8, U16, U32, U64, I8, I16, I32, F32, Vec3, Quat };

// What a field means, for tools and for later batches (prediction, interpolation).
enum class NetFieldRole : uint8_t {
    None,
    Position,
    Rotation,
    LinearVelocity,
    AngularVelocity,
    Flags,
    Aim,
    Health,
    Ammo,
    Timer,   // remaining ticks
    Team,
    State,   // a small enum (alive/dead, waiting/playing/finished)
    Score,
    Counter,
    EntityRef,   // a network entity ID; 0 is none
    Reserved,   // always zero in v1
};

const char* netFieldTypeName(NetFieldType type);
const char* netFieldRoleName(NetFieldRole role);
// Bytes on the wire: 1 for Bool, U8 and I8 up to 16 for Quat.
uint32_t netFieldBytes(NetFieldType type);

// One field. Integers must be in min..max; F32 and Vec3 components must be finite and
// within -max..max (min is ignored); a Quat's length must be 0.99..1.01. A nonzero
// mask allows only those bits.
struct NetFieldDesc {
    std::string name;
    NetFieldType type = NetFieldType::U8;
    double min = 0, max = 0;
    NetFieldRole role = NetFieldRole::None;
    uint64_t mask = 0;
};

// Shorthands for building field lists.
NetFieldDesc
netInt(std::string name, NetFieldType type, double min, double max, NetFieldRole role = NetFieldRole::None);
NetFieldDesc netFloat(std::string name, double bound, NetFieldRole role = NetFieldRole::None);
NetFieldDesc netVec3(std::string name, double bound, NetFieldRole role);
NetFieldDesc netQuat(std::string name, NetFieldRole role = NetFieldRole::Rotation);
NetFieldDesc netBits(std::string name, uint8_t mask, NetFieldRole role = NetFieldRole::Flags);
// The engine.pose.v1 fields (53 bytes), which other profiles may start with.
std::vector<NetFieldDesc> poseFields();

// A field's value: integers (and Bool) in `u`, signed ones two's complement; floats in f.
struct NetFieldValue {
    uint64_t u = 0;
    std::array<float, 4> f{};
};

// One object's state for a profile, field by field in profile order. Fixed size.
struct ProfileValue {
    std::array<NetFieldValue, kMaxNetFields> fields{};
};

// What capture and apply need besides the World.
struct ReplicationContext {
    uint32_t tickRate = 60;
};

// Null when it worked, else a static string saying why (no allocation).
using CaptureFn = const char* (*)(const World&, Entity, const ReplicationContext&, ProfileValue&);
using ApplyFn = void (*)(World&, Entity, const ReplicationContext&, const ProfileValue&);
// Rules between fields beyond their ranges; null when fine.
using ValidateFn = const char* (*)(const ProfileValue&);

struct ReplicationProfile {
    uint16_t id = 0;
    std::string name;   // "arena.player.v1": lowercase, dotted, ending in a version
    std::vector<NetFieldDesc> fields;
    // Components an object with this profile must have (--check).
    std::vector<std::string> requiredComponents;
    // One per player (a player's avatar): counted max_players times in --check's
    // bandwidth estimate.
    bool perPlayer = false;
    CaptureFn capture = nullptr;
    ApplyFn apply = nullptr;
    ValidateFn validate = nullptr;
    // Filled by freeze(): the encoded size, the same for every value in v1.
    uint16_t encodedBytes = 0;
};

enum class CommandPermission : uint8_t {
    AnyPlayer,   // any admitted player
    Owner,   // only the player owning the target entity
    Host,   // only the host's own player
};
const char* commandPermissionName(CommandPermission permission);

struct CommandDesc {
    uint16_t id = 0;
    std::string name;   // "arena.request_team"
    std::vector<NetFieldDesc> payload;
    CommandPermission permission = CommandPermission::AnyPlayer;
    // Profiles of entities it may target; empty for commands about the match, which
    // target no entity.
    std::vector<std::string> targetProfiles;
    uint16_t ratePerSecond = 2, burst = 2;   // per peer, at most 20 and 10
    std::string when;   // for people: when the host accepts it ("while the round is waiting")
    uint16_t payloadBytes = 0;   // filled by freeze()
};

struct EventDesc {
    uint16_t id = 0;
    std::string name;   // "arena.shot_resolved"
    std::vector<NetFieldDesc> payload;
    uint16_t payloadBytes = 0;   // filled by freeze()
};

class ReplicationRegistry {
public:
    // Each fails (false, with why) on a duplicate ID or name, an ID outside its range, or
    // once the registry is frozen; freeze() then fails too. Full checks come with freeze().
    bool addProfile(ReplicationProfile profile, std::string& why);
    bool addCommand(CommandDesc command, std::string& why);
    bool addEvent(EventDesc event, std::string& why);

    // Checks the whole catalog: names, ID ranges, field types and ranges, payload sizes,
    // callbacks, profile references. On success the registry is frozen for good and its
    // lookups work; on failure every problem is listed and it stays open.
    bool freeze(std::vector<std::string>& problems);
    bool frozen() const { return m_frozen; }

    // Lookups; null when unknown or not frozen yet. Constant time by ID, no allocation.
    const ReplicationProfile* profile(uint16_t id) const;
    const ReplicationProfile* profile(std::string_view name) const;
    const CommandDesc* command(uint16_t id) const;
    const EventDesc* event(uint16_t id) const;
    std::span<const ReplicationProfile> profiles() const { return m_profiles; }
    std::span<const CommandDesc> commands() const { return m_commands; }
    std::span<const EventDesc> events() const { return m_events; }

    // The catalog as canonical text, everything sorted by ID: what schemaDigest hashes.
    std::string catalogText() const;
    // SHA-256 of catalogText(); peers must match it to play together.
    Digest schemaDigest() const;

private:
    bool reject(std::string& why, std::string message);
    std::vector<std::string> m_rejected;   // registrations that failed, reported by freeze()
    std::vector<ReplicationProfile> m_profiles;
    std::vector<CommandDesc> m_commands;
    std::vector<EventDesc> m_events;
    std::array<int16_t, kLastGameProfile + 1> m_profileIndex{};   // id -> index + 1
    std::vector<uint16_t> m_commandIndex, m_eventIndex;   // id -> index + 1, sized to the largest id
    bool m_frozen = false;
};

// engine.pose.v1 (ID 1: a moving object's Transform) and engine.rigidbody.v1 (ID 2: a
// dynamic body the host simulates).
void registerEngineReplication(ReplicationRegistry& registry);

// Field-by-field encoding of a value with the profile's layout, checking every range
// first. Exactly profile.encodedBytes are written.
WireStatus
encodeProfile(const ReplicationProfile& profile, const ProfileValue& value, std::span<uint8_t> out, uint32_t& written);
// Decodes and checks a state record's payload (exactly encodedBytes long). out is only
// changed when the whole value is valid.
WireStatus decodeProfile(const ReplicationProfile& profile, std::span<const uint8_t> bytes, ProfileValue& out);
// The same for command and event payloads.
WireStatus encodeFields(
    std::span<const NetFieldDesc> fields, const ProfileValue& value, std::span<uint8_t> out, uint32_t& written);
WireStatus decodeFields(std::span<const NetFieldDesc> fields, std::span<const uint8_t> bytes, ProfileValue& out);

// Capture with validation: the World's state for the entity, checked against the
// profile's ranges, ready to encode. Out-of-range state is an error, never clamped.
const char* captureProfile(
    const ReplicationProfile& profile, const World& world, Entity entity, const ReplicationContext& context,
    ProfileValue& out);

// The engine.pose.v1 part (fields 0..4) of a profile that starts with poseFields(): the
// unparented root's Transform, and its RigidBody's or CharacterController's velocity.
const char* capturePoseFields(const World& world, Entity entity, ProfileValue& out);
void applyPoseFields(World& world, Entity entity, const ProfileValue& value);

// Helpers for capture/apply functions.
inline void setVec3(NetFieldValue& v, float x, float y, float z) { v.f = {x, y, z, 0}; }
inline void setQuat(NetFieldValue& v, float x, float y, float z, float w) { v.f = {x, y, z, w}; }

}   // namespace shockwave::net
