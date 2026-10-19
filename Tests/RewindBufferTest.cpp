// Lag compensation (Multiplayer/RewindHistory.h): ring sizes from max_rewind_ms and
// tick_rate; exact ticks, blended fractions and the millisecond timeline at 120 and 60 Hz;
// future times clamped, slightly-too-old times clamped and older ones rejected; no hitbox
// across a respawn, a death or a teleport; ray-capsule geometry; and raycasts that hit
// where a target was, miss where it is now, skip the shooter and pick the nearest.
#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/RewindHistory.h>

#include <glm/geometric.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace shockwave::net;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool near(glm::vec3 a, glm::vec3 b, float e = 1e-4f) { return glm::length(a - b) <= e; }

static RewindPlayerState alive(glm::vec3 feet, uint32_t incarnation = 1, uint8_t team = 0) {
    RewindPlayerState s;
    s.feet = feet;
    s.eye = feet + glm::vec3(0, 1.6f, 0);
    s.incarnation = incarnation;
    s.team = team;
    s.flags = RewindPlayerState::kAlive;
    return s;
}

static void testSizes() {
    CHECK(RewindHistoryBuffer(4, 120, 200).capacityTicks() == 26);   // 24 ticks + 2
    CHECK(RewindHistoryBuffer(4, 120, 200).maxRewindTicks() == 24);
    CHECK(RewindHistoryBuffer(4, 120, 250).capacityTicks() == kMaxRewindSamples);   // 30 + 2
    CHECK(RewindHistoryBuffer(4, 120, 250).maxRewindTicks() == 30);
    CHECK(RewindHistoryBuffer(4, 60, 200).capacityTicks() == 14);
    CHECK(RewindHistoryBuffer(4, 120, 1000).capacityTicks() == kMaxRewindSamples);
    CHECK(RewindHistoryBuffer(4, 120, 1000).maxRewindTicks() == 30);   // the most 32 samples allow
    NetworkSettings settings;
    settings.maxPlayers = 64;
    settings.tickRate = 120;
    settings.maxRewindMs = 200;
    const RewindHistoryBuffer fromSettings(settings);
    CHECK(fromSettings.playerSlots() == 64 && fromSettings.maxRewindTicks() == 24);
}

// Slot 0 walks along +x at 1 m per tick; slot 1 stands still.
static void walk(RewindHistoryBuffer& rewind, uint32_t from, uint32_t to) {
    std::vector<RewindPlayerState> players(rewind.playerSlots());
    for (uint32_t t = from; t <= to; ++t) {
        players[0] = alive({float(t), 0, 0});
        players[1] = alive({0, 0, 10});
        CHECK(rewind.record(t, players));
    }
}

static void testQueries() {
    RewindHistoryBuffer rewind(3, 120, 200);
    std::vector<RewindHitbox> boxes(3);
    CHECK(rewind.queryAtTimestamp(0, boxes).status == RewindStatus::Empty);
    CHECK(!boxes[0].valid);
    walk(rewind, 1000, 1100);
    CHECK(rewind.latestTick() == 1100);
    // Records must move forward and have one state per slot.
    std::vector<RewindPlayerState> players(3);
    CHECK(!rewind.record(1100, players));
    CHECK(!rewind.record(1101, std::span<const RewindPlayerState>(players.data(), 2)));

    // Exact ticks, by tick and by timestamp.
    RewindQuery q = rewind.queryAtTick(1090, 0, boxes);
    CHECK(q.status == RewindStatus::Exact && q.tick == 1090 && q.fraction == 0);
    CHECK(boxes[0].valid && near(boxes[0].a, {1090, 0.4f, 0}) && near(boxes[0].b, {1090, 1.4f, 0}));
    CHECK(boxes[0].radius == 0.4f && near(boxes[0].eye, {1090, 1.6f, 0}));
    CHECK(boxes[1].valid && near(boxes[1].a, {0, 0.4f, 10}));
    CHECK(!boxes[2].valid);   // nobody there
    // 1090 ticks at 120 Hz is 9083.33 ms.
    CHECK(rewind.timestampMs(1090) == 9083);
    CHECK(rewind.timestampMs(1200) == 10000);
    q = rewind.queryAtTimestamp(10000 - 50, boxes);   // tick 1194, after the latest: clamped to now
    CHECK(q.status == RewindStatus::Clamped && q.tick == 1100 && q.fraction == 0);
    // 9125 ms is tick 1095 exactly.
    q = rewind.queryAtTimestamp(9125, boxes);
    CHECK(q.status == RewindStatus::Exact && q.tick == 1095 && q.fraction == 0);
    CHECK(near(boxes[0].a, {1095, 0.4f, 0}));
    // 9126 ms: 0.12 of the way to 1096.
    q = rewind.queryAtTimestamp(9126, boxes);
    CHECK(q.status == RewindStatus::Exact && q.tick == 1095 && std::fabs(q.fraction - 0.12f) < 1e-3f);
    CHECK(near(boxes[0].a, {1095.12f, 0.4f, 0}, 2e-3f));
    // A fraction by tick.
    q = rewind.queryAtTick(1080, 0.75f, boxes);
    CHECK(q.status == RewindStatus::Exact && q.tick == 1080 && q.fraction == 0.75f);
    CHECK(near(boxes[0].a, {1080.75f, 0.4f, 0}));

    // The window: 24 ticks back is the edge, up to 2 more are clamped, beyond is rejected.
    q = rewind.queryAtTick(1076, 0, boxes);
    CHECK(q.status == RewindStatus::Exact && boxes[0].valid);
    q = rewind.queryAtTick(1075, 0, boxes);
    CHECK(q.status == RewindStatus::Clamped && q.tick == 1076 && boxes[0].valid && near(boxes[0].a, {1076, 0.4f, 0}));
    q = rewind.queryAtTick(1074, 0, boxes);
    CHECK(q.status == RewindStatus::Clamped && q.tick == 1076);
    q = rewind.queryAtTick(1074, 0.5f, boxes);
    CHECK(q.status == RewindStatus::Clamped);
    q = rewind.queryAtTick(1073, 0, boxes);
    CHECK(q.status == RewindStatus::Rejected && !boxes[0].valid && !boxes[1].valid);
    q = rewind.queryAtTimestamp(0, boxes);
    CHECK(q.status == RewindStatus::Rejected);
    // The future by tick.
    q = rewind.queryAtTick(1105, 0.5f, boxes);
    CHECK(q.status == RewindStatus::Clamped && q.tick == 1100 && q.fraction == 0);
    CHECK(std::strcmp(rewindStatusName(RewindStatus::Rejected), "rejected") == 0);

    // A skipped tick isn't in the history: nothing to blend with.
    std::vector<RewindPlayerState> more(3);
    more[0] = alive({1102, 0, 0});
    CHECK(rewind.record(1102, more));
    q = rewind.queryAtTick(1100, 0.5f, boxes);
    CHECK(q.status == RewindStatus::Exact && !boxes[0].valid);
    q = rewind.queryAtTick(1102, 0, boxes);
    CHECK(boxes[0].valid && !boxes[1].valid);

    // 60 Hz: 16.67 ms ticks.
    RewindHistoryBuffer slow(2, 60, 200);
    walk(slow, 100, 130);
    CHECK(slow.timestampMs(120) == 2000);
    q = slow.queryAtTimestamp(2000 + 8, boxes);
    CHECK(q.status == RewindStatus::Exact && q.tick == 120 && std::fabs(q.fraction - 0.48f) < 1e-3f);
    CHECK(near(boxes[0].a, {120.48f, 0.4f, 0}, 2e-3f));
}

static void testDiscontinuities() {
    RewindHistoryBuffer rewind(4, 120, 200);
    std::vector<RewindHitbox> boxes(4);
    std::vector<RewindPlayerState> players(4);
    // Tick 10: everyone alive. Tick 11: slot 0 respawned, slot 1 died, slot 2 teleported,
    // slot 3 kept going (protected after spawning: flagged, still hittable for the game to judge).
    for (uint16_t i = 0; i < 4; ++i) players[i] = alive({float(i), 0, 0});
    CHECK(rewind.record(10, players));
    players[0] = alive({50, 0, 0}, 2);
    players[1].flags = 0;
    players[2] = alive({-40, 0, 0});
    players[2].flags |= RewindPlayerState::kTeleported;
    players[3] = alive({3.5f, 0, 0}, 1, 2);
    players[3].flags |= RewindPlayerState::kProtected;
    CHECK(rewind.record(11, players));
    // Between them: only slot 3.
    rewind.queryAtTick(10, 0.5f, boxes);
    CHECK(!boxes[0].valid && !boxes[1].valid && !boxes[2].valid);
    CHECK(boxes[3].valid && near(boxes[3].a, {3.25f, 0.4f, 0}) && boxes[3].team == 2);
    CHECK(boxes[3].flags & RewindPlayerState::kProtected);
    // At either end each is what it was.
    rewind.queryAtTick(10, 0, boxes);
    CHECK(boxes[0].valid && boxes[1].valid && boxes[2].valid && boxes[0].incarnation == 1);
    rewind.queryAtTick(11, 0, boxes);
    CHECK(boxes[0].valid && boxes[0].incarnation == 2 && !boxes[1].valid && boxes[2].valid);
    CHECK(near(boxes[2].a, {-40, 0.4f, 0}));
    // Crouching: the height blends, and never below a sphere.
    RewindHistoryBuffer crouch(1, 120, 200);
    RewindPlayerState s = alive({0, 0, 0});
    CHECK(crouch.record(1, std::span<const RewindPlayerState>(&s, 1)));
    s.height = 0.2f;
    CHECK(crouch.record(2, std::span<const RewindPlayerState>(&s, 1)));
    crouch.queryAtTick(1, 0.5f, std::span<RewindHitbox>(boxes.data(), 1));
    CHECK(near(boxes[0].b, {0, 0.6f, 0}));   // height 1.0
    crouch.queryAtTick(2, 0, std::span<RewindHitbox>(boxes.data(), 1));
    CHECK(near(boxes[0].a, boxes[0].b));
}

static void testRayCapsule() {
    const glm::vec3 a(0, 0.4f, 0), b(0, 1.4f, 0);
    const float r = 0.4f;
    // Side.
    CHECK(std::fabs(rayCapsule({-5, 1, 0}, {1, 0, 0}, a, b, r) - 4.6f) < 1e-4f);
    // Top cap, straight down.
    CHECK(std::fabs(rayCapsule({0, 10, 0}, {0, -1, 0}, a, b, r) - 8.2f) < 1e-4f);
    // Bottom cap, from below.
    CHECK(std::fabs(rayCapsule({0, -3, 0}, {0, 1, 0}, a, b, r) - 3.0f) < 1e-4f);
    // Inside.
    CHECK(rayCapsule({0, 1, 0.1f}, {1, 0, 0}, a, b, r) == 0);
    // Misses: beside, behind, above.
    CHECK(rayCapsule({-5, 1, 0.41f}, {1, 0, 0}, a, b, r) < 0);
    CHECK(rayCapsule({5, 1, 0}, {1, 0, 0}, a, b, r) < 0);
    CHECK(rayCapsule({-5, 1.81f, 0}, {1, 0, 0}, a, b, r) < 0);
    // Grazing the top cap's sphere off the axis.
    const glm::vec3 d = glm::normalize(glm::vec3(1, -1, 0));
    const float t = rayCapsule(glm::vec3(-3, 4.4f, 0), d, a, b, r);
    CHECK(t > 0);
    const glm::vec3 p = glm::vec3(-3, 4.4f, 0) + d * t;
    CHECK(std::fabs(glm::length(p - b) - r) < 1e-3f);
    // A degenerate capsule is a sphere.
    CHECK(std::fabs(rayCapsule({-5, 1, 0}, {1, 0, 0}, {0, 1, 0}, {0, 1, 0}, 0.5f) - 4.5f) < 1e-4f);
}

static void testRaycast() {
    RewindHistoryBuffer rewind(3, 120, 200);
    walk(rewind, 1000, 1100);   // slot 0 at x = tick, slot 1 at (0, 0, 10)
    // Slot 2 (empty) shoots across slot 0's path at x = 1090.
    const glm::vec3 origin(1090, 1, -20), dir(0, 0, 1);
    RewindHit hit;
    RewindQuery q;
    // Where slot 0 was 10 ticks ago: hit.
    CHECK(rewind.raycastAtTick(origin, dir, 100, 1090, 0, 2, hit, &q));
    CHECK(q.status == RewindStatus::Exact && hit.slot == 0 && std::fabs(hit.distance - 19.6f) < 1e-3f);
    CHECK(near(hit.point, {1090, 1, -0.4f}, 1e-3f) && hit.incarnation == 1);
    // The same shot judged now misses: slot 0 has walked 10 m on.
    CHECK(!rewind.raycastAtTick(origin, dir, 100, 1100, 0, 2, hit, &q));
    // The same in milliseconds: 1090 ticks is 9083 ms, 0.04 of a tick short of 1090.
    CHECK(rewind.raycast(origin, dir, 100, 9083, 2, hit, &q) && hit.slot == 0);
    CHECK(q.tick == 1089 && q.fraction > 0.9f);
    // Too old: rejected, so no hit even though someone was there once.
    walk(rewind, 1101, 1101);
    CHECK(!rewind.raycastAtTick({1050, 1, -20}, dir, 100, 1050, 0, 2, hit, &q) && q.status == RewindStatus::Rejected);
    // maxDistance.
    CHECK(!rewind.raycastAtTick(origin, dir, 19, 1090, 0, 2, hit, &q));
    // The nearest of two in line.
    RewindHistoryBuffer line(3, 120, 200);
    std::vector<RewindPlayerState> players(3);
    players[0] = alive({0, 0, 5});
    players[1] = alive({0, 0, 10});
    players[2] = alive({0, 0, -1});   // the shooter, standing on the ray
    CHECK(line.record(1, players));
    CHECK(
        line.raycastAtTick({0, 1, -1}, dir, 100, 1, 0, 2, hit) && hit.slot == 0 &&
        std::fabs(hit.distance - 5.6f) < 1e-3f);
    // Without skipping the shooter the ray starts inside it.
    CHECK(line.raycastAtTick({0, 1, -1}, dir, 100, 1, 0, 1, hit) && hit.slot == 2 && hit.distance == 0);
    // Dead players aren't hit: the ray goes on to slot 1.
    players[0].flags = 0;
    CHECK(line.record(2, players));
    CHECK(line.raycastAtTick({0, 1, -1}, dir, 100, 2, 0, 2, hit) && hit.slot == 1);
}

// Wrapping the ring (and the tick counter) changes nothing.
static void testWrap() {
    RewindHistoryBuffer rewind(2, 120, 200);
    std::vector<RewindHitbox> boxes(2);
    const uint32_t start = 0xFFFFFFFFu - 40;
    std::vector<RewindPlayerState> players(2);
    for (uint32_t i = 0; i < 100; ++i) {
        const uint32_t t = start + i;
        players[0] = alive({float(i), 0, 0});
        CHECK(rewind.record(t, players));
        if (i < 30) continue;
        for (uint32_t back = 0; back <= rewind.maxRewindTicks(); ++back) {
            const RewindQuery q = rewind.queryAtTick(t - back, 0.5f * (back > 0), boxes);
            CHECK(q.status == RewindStatus::Exact);
            const float expect = float(i - back) + (back > 0 ? 0.5f : 0.0f);
            CHECK(boxes[0].valid && std::fabs(boxes[0].a.x - expect) < 1e-3f);
        }
    }
}

int main() {
    testSizes();
    testQueries();
    testDiscontinuities();
    testRayCapsule();
    testRaycast();
    testWrap();
    if (failures) {
        std::fprintf(stderr, "test_rewind_buffer: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_rewind_buffer: all checks passed\n");
    return 0;
}
