#pragma once

// Host-side lag compensation (docs/SteamMultiplayerImplementation.md section 10.1): after
// every authority step the host records each player's capsule, and a shot is checked
// against the capsules as they were when the shooter saw them, instead of where they are
// now. Nothing in the live physics world moves.
//
// Each player slot keeps a ring of ceil(max_rewind_ms * tick_rate / 1000) + 2 samples (at
// most 32: 250 ms at 120 Hz), so finding a tick is one index. A query time between two
// ticks blends the two samples, but only when both belong to the same life (incarnation)
// and agree on alive; across a death, respawn or teleport the player can't be hit. A time
// newer than the latest tick is clamped to it; one older than the rewind window is clamped
// within `toleranceTicks` and rejected beyond, so a client can't pick an old, favourable
// pose. Everything is allocated in the constructor.

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace shockwave::net {

struct NetworkSettings;

inline constexpr uint16_t kMaxRewindSamples = 32;   // a power of two

// What the host records for one player slot each tick.
struct RewindPlayerState {
    glm::vec3 feet{0.0f};   // bottom of the capsule
    glm::vec3 eye{0.0f};   // the ray origin when this player shoots
    float radius = 0.4f;
    float height = 1.8f;   // feet to top, at least 2 * radius
    uint32_t incarnation = 0;   // 0: no player in this slot
    uint8_t team = 0;
    uint8_t flags = 0;
    static constexpr uint8_t kAlive = 1, kProtected = 2, kTeleported = 4;
};

// A player's capsule at the time asked for: the segment a..b swept by `radius`.
struct RewindHitbox {
    glm::vec3 a{0.0f}, b{0.0f};
    glm::vec3 eye{0.0f};
    float radius = 0;
    uint32_t incarnation = 0;
    uint8_t team = 0;
    uint8_t flags = 0;
    bool valid = false;   // a player was there, alive, with no discontinuity in between
};

enum class RewindStatus : uint8_t {
    Exact,   // the time was inside the history
    Clamped,   // in the future, or just older than the window: moved to its edge
    Rejected,   // further back than the window allows
    Empty,   // nothing recorded yet
};
const char* rewindStatusName(RewindStatus status);

struct RewindQuery {
    RewindStatus status = RewindStatus::Empty;
    uint32_t tick = 0;   // the sample time used: tick plus fraction
    float fraction = 0;
};

struct RewindHit {
    uint16_t slot = 0;
    float distance = 0;
    glm::vec3 point{0.0f};
    uint32_t incarnation = 0;
    uint8_t team = 0;
    uint8_t flags = 0;   // the target's RewindPlayerState flags: the game decides protection and teams
};

class RewindHistoryBuffer {
public:
    RewindHistoryBuffer(uint16_t playerSlots, uint32_t tickRate, uint32_t maxRewindMs, uint32_t toleranceTicks = 2);
    // From the session settings: max_players slots, tick_rate, max_rewind_ms.
    explicit RewindHistoryBuffer(const NetworkSettings& settings);
    RewindHistoryBuffer(const RewindHistoryBuffer&) = delete;
    RewindHistoryBuffer& operator=(const RewindHistoryBuffer&) = delete;

    uint16_t playerSlots() const { return m_slots; }
    uint16_t capacityTicks() const { return m_capacity; }
    uint32_t maxRewindTicks() const { return m_maxRewindTicks; }

    // One authority tick: a state per player slot (exactly playerSlots of them). The tick
    // must be newer than the last; skipped ticks simply aren't in the history.
    bool record(uint32_t tick, std::span<const RewindPlayerState> players);
    bool hasLatest() const { return m_recorded; }
    uint32_t latestTick() const { return m_latest; }

    // The session timeline in milliseconds: tick 0 is 0 ms.
    uint64_t timestampMs(uint32_t tick) const;

    // Every slot's capsule at a time on the timeline (client_timestamp_ms: when the
    // shooter's view showed the targets). `out` needs playerSlots entries.
    RewindQuery queryAtTimestamp(uint64_t clientTimestampMs, std::span<RewindHitbox> out) const;
    // The same at a tick and a fraction of the next (an input command's view_tick and
    // view_fraction / 65536).
    RewindQuery queryAtTick(uint32_t tick, float fraction, std::span<RewindHitbox> out) const;

    // The nearest living player (other than `shooterSlot`) a ray hits at that time, within
    // maxDistance. `direction` must be unit length. Static-world obstruction is the
    // caller's: compare hit.distance with the world ray's.
    bool raycast(
        const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint64_t clientTimestampMs,
        uint16_t shooterSlot, RewindHit& hit, RewindQuery* query = nullptr) const;
    bool raycastAtTick(
        const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t tick, float fraction,
        uint16_t shooterSlot, RewindHit& hit, RewindQuery* query = nullptr) const;

private:
    struct alignas(16) Sample {
        RewindPlayerState state;
        uint32_t tick = 0;
        bool used = false;
    };
    RewindQuery resolve(double ticksSinceZero) const;
    void fill(const RewindQuery& query, std::span<RewindHitbox> out) const;
    bool cast(
        const RewindQuery& q, const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        uint16_t shooterSlot, RewindHit& hit, RewindQuery* query) const;
    const Sample* at(uint16_t slot, uint32_t tick) const;

    uint16_t m_slots;
    uint32_t m_tickRate;
    uint32_t m_maxRewindTicks;
    uint32_t m_toleranceTicks;
    uint16_t m_capacity;
    std::unique_ptr<Sample[]> m_samples;   // slots x kMaxRewindSamples, indexed by tick & 31
    std::unique_ptr<RewindHitbox[]> m_scratch;   // raycast's hitboxes: raycasts aren't thread-safe
    bool m_recorded = false;
    uint32_t m_latest = 0;
};

// Where a ray first touches a capsule (segment a..b, radius), as a distance along the unit
// direction, or a negative number for a miss. 0 when the origin is inside.
float rayCapsule(
    const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a, const glm::vec3& b, float radius);

}   // namespace shockwave::net
