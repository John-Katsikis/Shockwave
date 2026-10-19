// No heap allocations in replication's hot path: after construction, 10,000 ticks of a
// 64-player match at 120 Hz (the host storing snapshots and encoding deltas for clients
// whose acks lag by different amounts and sometimes go missing, a client decoding and
// applying them, acking, interpolating every player, and the host recording rewind
// history, querying it by timestamp and raycasting into it) allocate 0 bytes.
#include <Multiplayer/Interpolation.h>
#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/RewindHistory.h>
#include <Multiplayer/SnapshotDelta.h>

#include <Network/Messages.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

using namespace shockwave::net;

// GCC sees the replaced new and delete inlined into one function and takes malloc/free
// for a mismatch.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

static std::atomic<uint64_t> g_allocations{0};
static std::atomic<uint64_t> g_bytes{0};
void* operator new(std::size_t size) {
    ++g_allocations;
    g_bytes += size;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    ++g_allocations;
    g_bytes += size;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static const Nonce kSession = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
constexpr uint16_t kPlayers = 64;
constexpr uint32_t kTickRate = 120;
constexpr int kClients = 4;   // each with its own ack lag
constexpr int kTicks = 10000;
constexpr int kWarmTicks = 100;

// A cheap deterministic generator, so the loop itself allocates nothing.
static uint32_t g_seed = 12345;
static uint32_t nextRandom() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}
static float unit() { return float(nextRandom() % 20001) / 10000.0f - 1.0f; }

int main() {
    NetworkSettings settings;
    settings.tickRate = kTickRate;
    settings.maxPlayers = kPlayers;
    settings.interpolationMs = 25;
    settings.maxExtrapolationMs = 50;
    settings.maxRewindMs = 200;

    // Everything is made here, before counting starts.
    SnapshotHistory history(kPlayers);
    std::vector<std::unique_ptr<SnapshotDeltaCompressor>> compressors;
    std::vector<std::unique_ptr<SnapshotDeltaDecoder>> decoders;
    for (int c = 0; c < kClients; ++c) {
        compressors.push_back(std::make_unique<SnapshotDeltaCompressor>(history));
        decoders.push_back(std::make_unique<SnapshotDeltaDecoder>(kPlayers));
    }
    InterpolationEngine interpolation(kPlayers, interpolationSettings(settings));
    RewindHistoryBuffer rewind(settings);
    std::vector<NetEntityState> slots(kPlayers);
    std::vector<RewindPlayerState> players(kPlayers);
    std::vector<RewindHitbox> boxes(kPlayers);
    // Acks in flight per client: a small fixed ring of (arrival tick, ack).
    struct Pending {
        uint32_t arrives = 0;
        SnapshotAck ack{};
        bool used = false;
    };
    std::vector<Pending> inFlight(size_t(kClients) * 16);
    const uint32_t lag[kClients] = {3, 6, 12, 40};
    struct Mover {
        float x, z, yaw, turn;
        uint32_t incarnation;
    };
    std::vector<Mover> movers(kPlayers);
    for (Mover& m : movers) m = {unit() * 100, unit() * 100, unit() * 3.14f, 0, 1};
    DecodedMessage holder;
    DecodeOptions options;
    options.delivery = Delivery::Unreliable;
    options.receiver = PeerRole::Client;

    uint64_t startAllocations = 0, startBytes = 0;
    uint64_t parts = 0, completed = 0, hits = 0, rewound = 0, interpolated = 0, keyframes = 0;
    for (int i = 0; i < kTicks; ++i) {
        if (i == kWarmTicks) {
            startAllocations = g_allocations.load();
            startBytes = g_bytes.load();
        }
        const uint32_t tick = 5000 + uint32_t(i);
        const float dt = 1.0f / kTickRate;
        for (uint16_t p = 0; p < kPlayers; ++p) {
            Mover& m = movers[p];
            m.turn = std::fmax(-3.0f, std::fmin(3.0f, m.turn + unit() * 0.6f));
            m.yaw += m.turn * dt;
            m.x = std::fmax(-100.0f, std::fmin(100.0f, m.x + std::sin(m.yaw) * 6 * dt));
            m.z = std::fmax(-100.0f, std::fmin(100.0f, m.z + std::cos(m.yaw) * 6 * dt));
            if (nextRandom() % 5000 == 0) ++m.incarnation;   // a respawn now and then
            PoseState pose;
            pose.position = {m.x, 0, m.z};
            pose.rotation = {0, std::sin(m.yaw / 2), 0, std::cos(m.yaw / 2)};
            pose.linearVelocity = {std::sin(m.yaw) * 6, 0, std::cos(m.yaw) * 6};
            pose.angularVelocity = {0, m.turn, 0};
            slots[p] = quantizeState(pose, uint16_t(m.incarnation));
            RewindPlayerState& r = players[p];
            r.feet = {m.x, 0, m.z};
            r.eye = {m.x, 1.6f, m.z};
            r.incarnation = m.incarnation;
            r.team = uint8_t(p & 1);
            r.flags = RewindPlayerState::kAlive;
        }
        CHECK(history.store(tick, slots));
        CHECK(rewind.record(tick, players));

        for (int c = 0; c < kClients; ++c) {
            // Acks that arrived.
            for (int k = 0; k < 16; ++k) {
                Pending& pending = inFlight[size_t(c) * 16 + k];
                if (pending.used && pending.arrives <= tick) {
                    compressors[c]->acknowledge(pending.ack);
                    pending.used = false;
                }
            }
            std::span<const DeltaPart> out;
            CHECK(compressors[c]->encode(tick, 1, kSession, out));
            keyframes += compressors[c]->lastStats().keyframe;
            for (const DeltaPart& part : out) {
                ++parts;
                if (nextRandom() % 100 < 5) continue;   // 5% loss
                CHECK(decodeMessage(part.message, options, holder));
                const DeltaApplyResult r = decoders[c]->apply(std::get<DeltaSnapshot>(holder.body));
                completed += r == DeltaApplyResult::Completed;
            }
            if (c == 0)
                if (const NetEntityState* done = decoders[c]->complete(tick))
                    interpolation.pushSnapshot(tick, std::span<const NetEntityState>(done, kPlayers));
            if (nextRandom() % 100 < 10) continue;   // a lost ack
            const uint32_t arrives = tick + lag[c];
            for (int k = 0; k < 16; ++k) {
                Pending& pending = inFlight[size_t(c) * 16 + k];
                if (pending.used) continue;
                pending = {arrives, decoders[c]->ack(), true};
                break;
            }
        }

        // The client draws everyone 3 ticks behind.
        if (i >= 4) {
            InterpolatedPose pose;
            for (uint16_t p = 0; p < kPlayers; ++p)
                interpolated += interpolation.sample(p, {tick - 3, 0.37f}, pose) == InterpolationResult::Interpolated;
        }

        // The host checks a few shots, as each shooter saw the world 0..200 ms ago.
        if (i >= 30) {
            for (int shot = 0; shot < 8; ++shot) {
                const uint16_t shooter = uint16_t(nextRandom() % kPlayers);
                const uint16_t target = uint16_t((shooter + 1 + nextRandom() % (kPlayers - 1)) % kPlayers);
                const uint64_t viewMs = rewind.timestampMs(tick) - nextRandom() % 200;
                const RewindQuery q = rewind.queryAtTimestamp(viewMs, boxes);
                rewound += q.status == RewindStatus::Exact;
                const glm::vec3 from = players[shooter].eye;
                const glm::vec3 to = (boxes[target].a + boxes[target].b) * 0.5f;
                const glm::vec3 d = to - from;
                const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len < 1e-3f) continue;
                RewindHit hit;
                hits += rewind.raycast(from, d / len, 500, viewMs, shooter, hit);
                hits += rewind.raycastAtTick(from, d / len, 500, tick - 5, 0.5f, shooter, hit);
            }
        }
    }
    const uint64_t allocations = g_allocations.load() - startAllocations;
    const uint64_t bytes = g_bytes.load() - startBytes;
    std::printf(
        "%d ticks, %d clients: %llu parts, %llu snapshots completed, %llu keyframes, %llu interpolated poses, %llu "
        "rewind queries in the window, %llu raycast hits; heap: %llu allocations, %llu bytes\n",
        kTicks - kWarmTicks, kClients, (unsigned long long)parts, (unsigned long long)completed,
        (unsigned long long)keyframes, (unsigned long long)interpolated, (unsigned long long)rewound,
        (unsigned long long)hits, (unsigned long long)allocations, (unsigned long long)bytes);
    CHECK(allocations == 0);
    CHECK(bytes == 0);
    // The run did the work it claims.
    CHECK(completed > uint64_t(kTicks) * kClients * 8 / 10);
    CHECK(interpolated > uint64_t(kTicks) * kPlayers * 8 / 10);
    CHECK(rewound > 9000 * 8 * 9 / 10);
    CHECK(hits > 10000);
    if (failures) {
        std::fprintf(stderr, "test_replication_allocations: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_replication_allocations: all checks passed\n");
    return 0;
}
