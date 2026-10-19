// Delta snapshots (Multiplayer/SnapshotDelta.h) and remote interpolation
// (Multiplayer/Interpolation.h): quantization limits and round trips; 64 players all moving
// at 120 Hz fit one part under 1152 bytes against an acknowledged baseline, and a keyframe
// splits into parts that each fit; the client rebuilds the host's snapshots bit for bit
// over loss, reordering and duplication; acks, too-old baselines, respawns and emptied
// slots; every kind of malformed part is refused without changing anything; and the
// interpolation engine's Hermite blending, slerp, extrapolation limit, teleport reset,
// out-of-order samples, server clock and 3-tick jitter buffer.
#include <Multiplayer/Interpolation.h>
#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/SnapshotDelta.h>

#include <Network/Messages.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
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

static const Nonce kSession = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
constexpr uint32_t kTickRate = 120;
constexpr uint16_t kPlayers = 64;

// ---- A deterministic crowd ----------------------------------------------------------------

// 64 first-person players running, strafing, turning and jumping in a 200 m arena, all of
// them moving every tick.
struct Crowd {
    struct Player {
        float x, y, z, vx, vy, vz, yaw, turn;
        uint32_t incarnation = 1;
    };
    std::vector<Player> players;
    std::mt19937 rng;
    explicit Crowd(size_t n, uint32_t seed = 7) : players(n), rng(seed) {
        std::uniform_real_distribution<float> pos(-100, 100), angle(-3.14159f, 3.14159f);
        for (Player& p : players) p = {pos(rng), 0, pos(rng), 0, 0, 0, angle(rng), 0};
    }
    void step() {
        std::uniform_real_distribution<float> unit(-1, 1);
        const float dt = 1.0f / kTickRate;
        for (Player& p : players) {
            // Steer: turn rate wanders, speed stays 4..7 m/s along a wandering heading.
            p.turn = std::clamp(p.turn + unit(rng) * 0.6f, -3.0f, 3.0f);
            p.yaw += p.turn * dt;
            const float speed = 5.5f + 1.5f * std::sin(p.yaw * 3.0f);
            const float strafe = unit(rng) * 0.5f;
            p.vx = std::sin(p.yaw) * speed + std::cos(p.yaw) * strafe;
            p.vz = std::cos(p.yaw) * speed - std::sin(p.yaw) * strafe;
            if (p.y <= 0 && unit(rng) > 0.995f) p.vy = 5.0f;   // jump
            p.vy -= 9.81f * dt;
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.z += p.vz * dt;
            if (p.y < 0) p.y = p.vy = 0;
            if (std::fabs(p.x) > 100) p.vx = -p.vx, p.x = std::clamp(p.x, -100.0f, 100.0f);
            if (std::fabs(p.z) > 100) p.vz = -p.vz, p.z = std::clamp(p.z, -100.0f, 100.0f);
        }
    }
    static PoseState pose(const Player& p) {
        PoseState s;
        s.position = {p.x, p.y, p.z};
        s.rotation = {0, std::sin(p.yaw / 2), 0, std::cos(p.yaw / 2)};
        s.linearVelocity = {p.vx, p.vy, p.vz};
        s.angularVelocity = {0, p.turn, 0};
        return s;
    }
    void fill(std::vector<NetEntityState>& slots) const {
        for (size_t i = 0; i < players.size(); ++i) slots[i] = quantizeState(pose(players[i]), players[i].incarnation);
    }
};

static DeltaSnapshot decodePart(const DeltaPart& part, DecodedMessage& holder) {
    DecodeOptions options;
    options.delivery = Delivery::Unreliable;
    options.receiver = PeerRole::Client;
    const WireStatus status = decodeMessage(part.message, options, holder);
    CHECK(status);
    CHECK(holder.header.kind == MessageKind::DeltaSnapshot);
    return std::get<DeltaSnapshot>(holder.body);
}

static bool sameSlots(const NetEntityState* a, const NetEntityState* b, size_t n) {
    return a && b && std::memcmp(a, b, n * sizeof(NetEntityState)) == 0;
}

// ---- Quantization -----------------------------------------------------------------------

static void testQuantization() {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> pos(-32768, 32768), vel(-511, 511), ang(-1023, 1023), unit(-1, 1);
    for (int i = 0; i < 20000; ++i) {
        PoseState p;
        p.position = {pos(rng), pos(rng), pos(rng)};
        float q[4] = {unit(rng), unit(rng), unit(rng), unit(rng)};
        float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (len < 0.1f) continue;
        p.rotation = {q[0] / len, q[1] / len, q[2] / len, q[3] / len};
        p.linearVelocity = {vel(rng), vel(rng), vel(rng)};
        p.angularVelocity = {ang(rng), ang(rng), ang(rng)};
        p.flags = PoseState::kSleeping;
        const NetEntityState s = quantizeState(p, 7);
        CHECK(!invalidState(s));
        const PoseState back = dequantizeState(s);
        CHECK(std::fabs(back.position.x - p.position.x) <= 0.5f / 1024 + 1e-3f);
        CHECK(std::fabs(back.linearVelocity.y - p.linearVelocity.y) <= 0.5f / 64 + 1e-6f);
        CHECK(std::fabs(back.angularVelocity.z - p.angularVelocity.z) <= 0.5f / 32 + 1e-6f);
        // The same rotation (or its negative) to within about 0.2 degrees.
        const float dot = std::fabs(
            back.rotation.x * p.rotation.x + back.rotation.y * p.rotation.y + back.rotation.z * p.rotation.z +
            back.rotation.w * p.rotation.w);
        CHECK(dot > 0.99999f);
        CHECK(back.flags == PoseState::kSleeping);
        // Quantizing the result again gives the same position, velocities and flags, and a
        // rotation within a step (the largest component can change when two are close).
        NetEntityState again = quantizeState(back, 7);
        CHECK(
            std::fabs(
                dequantizeState(again).rotation.w * back.rotation.w +
                dequantizeState(again).rotation.x * back.rotation.x +
                dequantizeState(again).rotation.y * back.rotation.y +
                dequantizeState(again).rotation.z * back.rotation.z) > 0.99999f);
        again.rotation = s.rotation;
        CHECK(again == s);
    }
    // The limits: velocities past the largest step become it.
    PoseState fast;
    fast.linearVelocity = {512, -512, 0};
    fast.angularVelocity = {1024, 0, -1024};
    const NetEntityState s = quantizeState(fast, 1);
    CHECK(s.velocity[0] == 32767 && s.velocity[1] == -32767);
    CHECK(s.angularVelocity[0] == 32767 && s.angularVelocity[2] == -32767);
    CHECK(quantizeState(PoseState{}, 1).rotation == 0xC0000000u);   // identity: w largest, the rest 0
    // invalidState
    CHECK(!invalidState(NetEntityState{}));
    NetEntityState bad = s;
    bad.reserved = 1;
    CHECK(invalidState(bad) && std::strcmp(invalidState(bad), "reserved") == 0);
    bad = s;
    bad.flags = 0x80;
    CHECK(invalidState(bad));
    bad = s;
    bad.velocity[1] = -32768;
    CHECK(invalidState(bad));
    bad = s;
    bad.position[2] = kNetMaxPosition + 1;
    CHECK(invalidState(bad));
    bad = {};
    bad.position[0] = 5;   // empty slot with data
    CHECK(invalidState(bad));
    bad = s;
    bad.rotation = 0x200;   // component -512
    CHECK(invalidState(bad));
}

// ---- One host, one client -----------------------------------------------------------------

struct Link {
    SnapshotHistory history{kPlayers};
    SnapshotDeltaCompressor compressor{history};
    SnapshotDeltaDecoder decoder{kPlayers};
    Crowd crowd{kPlayers};
    std::vector<NetEntityState> slots = std::vector<NetEntityState>(kPlayers);
    uint32_t tick = 1000;
    std::vector<std::pair<uint32_t, SnapshotAck>> acksInFlight;   // arrival tick, ack
};

static void testMovingCrowdFitsOnePart() {
    Link link;
    const uint32_t ackDelayTicks = 6;   // 50 ms round trip at 120 Hz
    uint32_t maxBytes = 0, maxBits = 0, deltaTicks = 0, totalBytes = 0;
    bool firstWasKeyframe = false;
    DecodedMessage holder;
    for (int i = 0; i < 2400; ++i) {   // 20 seconds
        link.crowd.step();
        link.crowd.fill(link.slots);
        CHECK(link.history.store(link.tick, link.slots));
        // Acks that have made it back by now.
        for (auto it = link.acksInFlight.begin(); it != link.acksInFlight.end();)
            if (it->first <= link.tick) {
                link.compressor.acknowledge(it->second);
                it = link.acksInFlight.erase(it);
            } else {
                ++it;
            }
        std::span<const DeltaPart> parts;
        CHECK(link.compressor.encode(link.tick, 1, kSession, parts));
        const DeltaEncodeStats& stats = link.compressor.lastStats();
        if (i == 0) firstWasKeyframe = stats.keyframe;
        if (!stats.keyframe) {
            ++deltaTicks;
            CHECK(stats.parts == 1);   // every player, every tick, in one message
            CHECK(stats.changed == kPlayers);
            maxBytes = std::max(maxBytes, stats.bytes);
            totalBytes += stats.bytes;
        }
        for (const DeltaPart& p : parts) {
            CHECK(p.message.size() <= kMaxUnreliableMessageBytes);
            const DeltaSnapshot part = decodePart(p, holder);
            maxBits = std::max<uint32_t>(maxBits, uint32_t(part.payload.size()) * 8);
            const DeltaApplyResult r = link.decoder.apply(part);
            CHECK(r == DeltaApplyResult::Completed || r == DeltaApplyResult::Applied);
        }
        CHECK(sameSlots(link.decoder.complete(link.tick), link.slots.data(), kPlayers));
        link.acksInFlight.push_back({link.tick + ackDelayTicks, link.decoder.ack()});
        ++link.tick;
    }
    CHECK(firstWasKeyframe);
    CHECK(deltaTicks > 2300);
    CHECK(maxBytes <= kMaxUnreliableMessageBytes);
    const uint32_t raw = kHeaderBytes + 18 + kPlayers * (kStateRecordHeaderBytes + kPoseStateBytes);
    std::printf(
        "64 moving players at 120 Hz, baseline 6 ticks old: raw v1 snapshot %u bytes, delta max %u bytes, mean %.0f "
        "bytes (%.1fx smaller), 1 part\n",
        raw, maxBytes, double(totalBytes) / deltaTicks, double(raw) / (double(totalBytes) / deltaTicks));
}

static void testKeyframeAndOldBaselineSplit() {
    for (uint32_t maxMessage : {kMaxUnreliableMessageBytes, 512u}) {
        Link link;
        SnapshotDeltaCompressor compressor(link.history, maxMessage);
        link.crowd.step();
        link.crowd.fill(link.slots);
        CHECK(link.history.store(link.tick, link.slots));
        std::span<const DeltaPart> parts;
        CHECK(compressor.encode(link.tick, 1, kSession, parts));
        CHECK(compressor.lastStats().keyframe);
        CHECK(parts.size() >= 2);   // 64 full states don't fit 1152 bytes
        DecodedMessage holder;
        uint32_t changed = 0;
        for (size_t i = 0; i < parts.size(); ++i) {
            CHECK(parts[i].message.size() <= maxMessage);
            const DeltaSnapshot part = decodePart(parts[i], holder);
            CHECK(part.partIndex == i && part.partCount == parts.size());
            CHECK(!(part.flags & DeltaSnapshot::kHasBaseline));
            changed += part.changedCount;
            const DeltaApplyResult r = link.decoder.apply(part);
            CHECK(r == (i + 1 == parts.size() ? DeltaApplyResult::Completed : DeltaApplyResult::Applied));
            if (i + 1 < parts.size()) CHECK(!link.decoder.complete(link.tick));
        }
        CHECK(changed == kPlayers);
        CHECK(sameSlots(link.decoder.complete(link.tick), link.slots.data(), kPlayers));
        if (maxMessage == kMaxUnreliableMessageBytes)
            std::printf("keyframe of 64 players: %zu parts, %u bytes\n", parts.size(), compressor.lastStats().bytes);
    }
}

// Every slot as large as possible: the worst case the constructor sizes for.
static void testWorstCase() {
    for (uint32_t maxMessage : {kMaxUnreliableMessageBytes, 512u}) {
        SnapshotHistory history(kMaxDeltaSlots);
        SnapshotDeltaCompressor compressor(history, maxMessage);
        SnapshotDeltaDecoder decoder(kMaxDeltaSlots);
        std::vector<NetEntityState> a(kMaxDeltaSlots), b(kMaxDeltaSlots);
        for (size_t i = 0; i < a.size(); ++i) {
            NetEntityState& s = a[i];
            s.position = {kNetMaxPosition, -kNetMaxPosition, kNetMaxPosition};
            s.rotation = 0x3FFFFFFFu & ~0x20080200u;   // index 0, components +511
            s.velocity = {32767, -32767, 32767};
            s.angularVelocity = {-32767, 32767, -32767};
            s.flags = PoseState::kTeleport;
            s.incarnation = 0xFFFF;
            NetEntityState& t = b[i];
            t = s;
            t.position = {-kNetMaxPosition, kNetMaxPosition, -kNetMaxPosition};
            t.rotation = 0xC0000000u | 0x20180601u;   // index 3, components -511
            t.velocity = {-32767, 32767, -32767};
            t.angularVelocity = {32767, -32767, 32767};
            t.flags = PoseState::kSleeping;
        }
        CHECK(!invalidState(a[0]) && !invalidState(b[0]));
        CHECK(history.store(1, a) && history.store(2, b));
        DecodedMessage holder;
        for (uint32_t tick : {1u, 2u}) {
            std::span<const DeltaPart> parts;
            CHECK(compressor.encode(tick, 1, kSession, parts));
            for (const DeltaPart& p : parts) {
                CHECK(p.message.size() <= maxMessage);
                const DeltaApplyResult r = decoder.apply(decodePart(p, holder));
                CHECK(r == DeltaApplyResult::Applied || r == DeltaApplyResult::Completed);
            }
            CHECK(sameSlots(decoder.complete(tick), history.find(tick), kMaxDeltaSlots));
            compressor.acknowledge(decoder.ack());
            if (tick == 2) {
                CHECK(!compressor.lastStats().keyframe);
                CHECK(parts.size() < 255);
            }
        }
    }
}

// Loss, reordering and duplicates on the way out, lost and late acks on the way back.
static void testLossyLink() {
    Link link;
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> percent(0, 99);
    struct Flight {
        uint32_t arrives;
        std::vector<uint8_t> bytes;
    };
    std::vector<Flight> toClient;
    std::vector<std::pair<uint32_t, SnapshotAck>> toHost;
    uint32_t completed = 0, verified = 0, keyframes = 0;
    for (int i = 0; i < 3000; ++i) {
        link.crowd.step();
        if (i % 500 == 250) link.crowd.players[i % kPlayers].incarnation++;   // a respawn
        link.crowd.fill(link.slots);
        if (i % 700 == 350) link.slots[3] = {};   // a despawn
        CHECK(link.history.store(link.tick, link.slots));
        for (auto it = toHost.begin(); it != toHost.end();)
            if (it->first <= link.tick) {
                link.compressor.acknowledge(it->second);
                it = toHost.erase(it);
            } else {
                ++it;
            }
        std::span<const DeltaPart> parts;
        CHECK(link.compressor.encode(link.tick, 1, kSession, parts));
        keyframes += link.compressor.lastStats().keyframe;
        for (const DeltaPart& p : parts) {
            if (percent(rng) < 10) continue;   // 10% loss
            const uint32_t delay = 3 + uint32_t(percent(rng) % 4);   // 25..50 ms, reordering
            toClient.push_back({link.tick + delay, {p.message.begin(), p.message.end()}});
            if (percent(rng) < 3) toClient.push_back({link.tick + delay + 2, {p.message.begin(), p.message.end()}});
        }
        DecodedMessage holder;
        for (auto it = toClient.begin(); it != toClient.end();)
            if (it->arrives <= link.tick) {
                DecodeOptions options;
                options.delivery = Delivery::Unreliable;
                options.receiver = PeerRole::Client;
                CHECK(decodeMessage(it->bytes, options, holder));
                const DeltaSnapshot part = std::get<DeltaSnapshot>(holder.body);
                const DeltaApplyResult r = link.decoder.apply(part);
                CHECK(r != DeltaApplyResult::Malformed);
                if (r == DeltaApplyResult::Completed) {
                    ++completed;
                    // Whatever completes is exactly what the host stored for that tick.
                    if (const NetEntityState* host = link.history.find(part.snapshotTick)) {
                        CHECK(sameSlots(link.decoder.complete(part.snapshotTick), host, kPlayers));
                        ++verified;
                    }
                }
                it = toClient.erase(it);
            } else {
                ++it;
            }
        if (percent(rng) >= 15) toHost.push_back({link.tick + 3 + uint32_t(percent(rng) % 4), link.decoder.ack()});
        ++link.tick;
    }
    CHECK(completed > 2000 && verified == completed);
    CHECK(keyframes >= 1);
    std::printf("lossy link: %u of 3000 snapshots completed, %u keyframes\n", completed, keyframes);
}

static void testAcksAndBaselines() {
    Link link;
    DecodedMessage holder;
    std::span<const DeltaPart> parts;
    auto send = [&](bool deliver) {
        link.crowd.step();
        link.crowd.fill(link.slots);
        CHECK(link.history.store(link.tick, link.slots));
        CHECK(link.compressor.encode(link.tick, 1, kSession, parts));
        if (deliver)
            for (const DeltaPart& p : parts) link.decoder.apply(decodePart(p, holder));
        return link.tick++;
    };
    // No ack: keyframes.
    const uint32_t t0 = send(true);
    CHECK(link.compressor.lastStats().keyframe);
    send(true);
    CHECK(link.compressor.lastStats().keyframe);
    // An ack for a tick never sent is ignored.
    link.compressor.acknowledge({t0 + 100, 0});
    CHECK(!link.compressor.hasBaseline());
    link.compressor.acknowledge(link.decoder.ack());
    CHECK(link.compressor.hasBaseline() && link.compressor.baselineTick() == t0 + 1);
    // An older ack doesn't move the baseline back.
    link.compressor.acknowledge({t0, 0});
    CHECK(link.compressor.baselineTick() == t0 + 1);
    const uint32_t t2 = send(true);
    CHECK(!link.compressor.lastStats().keyframe);
    CHECK(sameSlots(link.decoder.complete(t2), link.slots.data(), kPlayers));
    // The ack bits: t2 complete, t0 and t0+1 before it.
    const SnapshotAck ack = link.decoder.ack();
    CHECK(ack.latestTick == t2 && (ack.receivedBits & 3) == 3);
    // The baseline leaves the history: a keyframe again.
    for (int i = 0; i < kDeltaHistoryTicks + 2; ++i) send(false);
    send(true);
    CHECK(link.compressor.lastStats().keyframe);
    CHECK(sameSlots(link.decoder.complete(link.tick - 1), link.slots.data(), kPlayers));
    // reset() forgets acks.
    link.compressor.acknowledge(link.decoder.ack());
    CHECK(link.compressor.hasBaseline());
    link.compressor.reset();
    send(true);
    CHECK(link.compressor.lastStats().keyframe);
    // A part whose baseline the client doesn't have.
    link.compressor.acknowledge(link.decoder.ack());
    const uint32_t lost = send(false);
    (void)lost;
    link.decoder.reset();
    send(false);
    CHECK(link.compressor.encode(link.tick - 1, 1, kSession, parts));
    CHECK(link.decoder.apply(decodePart(parts[0], holder)) == DeltaApplyResult::MissingBaseline);
    CHECK(!link.decoder.hasComplete());
}

// Nothing changed: one empty part; one entity changed: one small part.
static void testSmallChanges() {
    SnapshotHistory history(kPlayers);
    SnapshotDeltaCompressor compressor(history);
    SnapshotDeltaDecoder decoder(kPlayers);
    std::vector<NetEntityState> slots(kPlayers);
    Crowd crowd(kPlayers);
    crowd.fill(slots);
    DecodedMessage holder;
    std::span<const DeltaPart> parts;
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        if (tick == 4) slots[40].position[0] += 3;
        CHECK(history.store(tick, slots));
        CHECK(compressor.encode(tick, 9, kSession, parts));
        for (const DeltaPart& p : parts) {
            const DeltaSnapshot part = decodePart(p, holder);
            CHECK(part.requiredControlSerial == 9);
            CHECK(decoder.apply(part) == DeltaApplyResult::Completed);
            if (tick == 3) CHECK(part.slotCount == 0 && part.changedCount == 0 && part.payload.empty());
            if (tick == 4) CHECK(part.firstSlot == 40 && part.slotCount == 1 && part.changedCount == 1);
        }
        CHECK(sameSlots(decoder.complete(tick), slots.data(), kPlayers));
        compressor.acknowledge(decoder.ack());
    }
    CHECK(parts.size() == 1 && parts[0].message.size() <= kDeltaSnapshotOverheadBytes + 4);
    // The overhead constant is what an empty part encodes to.
    DeltaSnapshot empty;
    std::array<uint8_t, 128> buffer{};
    uint32_t written = 0;
    CHECK(encodeMessage(MessageBody(empty), kSession, 0, buffer, written));
    CHECK(written == kDeltaSnapshotOverheadBytes);
    // Respawn (fresh) and despawn (emptied) round-trip.
    slots[5].incarnation++;
    slots[6] = {};
    CHECK(history.store(5, slots));
    CHECK(compressor.encode(5, 9, kSession, parts));
    for (const DeltaPart& p : parts) CHECK(decoder.apply(decodePart(p, holder)) == DeltaApplyResult::Completed);
    CHECK(sameSlots(decoder.complete(5), slots.data(), kPlayers));
    CHECK(!decoder.complete(5)[6].active());
    // History rules.
    CHECK(!history.store(5, slots));   // not newer
    CHECK(!history.store(6, std::span<const NetEntityState>(slots.data(), 3)));   // wrong size
    std::vector<NetEntityState> bad = slots;
    bad[0].reserved = 1;
    CHECK(!history.store(6, bad));
    CHECK(!history.find(6) && history.find(5));
    CHECK(!compressor.encode(6, 9, kSession, parts) && parts.empty());
}

// Every way a part can be wrong, refused with nothing changed.
static void testMalformed() {
    Link link;
    DecodedMessage holder;
    std::span<const DeltaPart> parts;
    link.crowd.fill(link.slots);
    CHECK(link.history.store(1, link.slots));
    CHECK(link.compressor.encode(1, 1, kSession, parts));
    for (const DeltaPart& p : parts) link.decoder.apply(decodePart(p, holder));
    link.compressor.acknowledge(link.decoder.ack());
    link.crowd.step();
    link.crowd.fill(link.slots);
    CHECK(link.history.store(2, link.slots));
    CHECK(link.compressor.encode(2, 1, kSession, parts));
    CHECK(parts.size() == 1);
    const DeltaSnapshot good = decodePart(parts[0], holder);
    std::vector<uint8_t> payload(good.payload.begin(), good.payload.end());

    auto refused = [&](DeltaSnapshot part) {
        const DeltaApplyResult r = link.decoder.apply(part);
        return r == DeltaApplyResult::Malformed && !link.decoder.complete(2);
    };
    DeltaSnapshot p = good;
    p.slotCount = kPlayers + 1;   // past the table
    CHECK(refused(p));
    p = good;
    p.changedCount = uint16_t(good.changedCount - 1);
    CHECK(refused(p));
    p = good;
    p.changedCount = uint16_t(good.changedCount + 1);
    CHECK(refused(p));
    p = good;
    p.partIndex = 1;   // >= part_count
    CHECK(refused(p));
    p = good;
    p.payload = std::span<const uint8_t>(payload.data(), payload.size() - 1);   // truncated
    CHECK(refused(p));
    std::vector<uint8_t> longer = payload;
    longer.push_back(0);   // a whole spare byte
    p = good;
    p.payload = longer;
    CHECK(refused(p));
    // A padding bit set, when the last byte has padding.
    const uint32_t bits = link.compressor.lastStats().payloadBits;
    CHECK((bits + 7) / 8 == payload.size());
    if (bits % 8) {
        std::vector<uint8_t> flipped = payload;
        flipped.back() |= 0x80;
        p = good;
        p.payload = flipped;
        CHECK(refused(p));
    }
    p = good;
    p.flags = 0;   // no baseline: the deltas would be read against empty slots
    p.baselineTick = 0;
    CHECK(
        link.decoder.apply(p) != DeltaApplyResult::Completed ||
        !sameSlots(link.decoder.complete(2), link.slots.data(), kPlayers));
    link.decoder.reset();
    // Parts that disagree with each other, duplicates, and stale ticks.
    {
        SnapshotDeltaDecoder decoder(kPlayers);
        SnapshotHistory history(kPlayers);
        SnapshotDeltaCompressor compressor(history);
        Crowd crowd(kPlayers);
        std::vector<NetEntityState> slots(kPlayers);
        crowd.step();
        crowd.fill(slots);
        CHECK(history.store(1, slots));
        CHECK(compressor.encode(1, 1, kSession, parts));
        CHECK(parts.size() >= 2);
        std::vector<std::vector<uint8_t>> copies;
        for (const DeltaPart& part : parts) copies.emplace_back(part.message.begin(), part.message.end());
        DecodedMessage h0, h1;
        DecodeOptions options;
        options.delivery = Delivery::Unreliable;
        options.receiver = PeerRole::Client;
        CHECK(decodeMessage(copies[0], options, h0) && decodeMessage(copies[1], options, h1));
        const DeltaSnapshot p0 = std::get<DeltaSnapshot>(h0.body), p1 = std::get<DeltaSnapshot>(h1.body);
        CHECK(decoder.apply(p0) == DeltaApplyResult::Applied);
        CHECK(decoder.apply(p0) == DeltaApplyResult::Duplicate);
        DeltaSnapshot q = p1;
        q.flags = DeltaSnapshot::kHasBaseline;   // disagrees with part 0
        q.baselineTick = 0xFFFFFFF0u;
        CHECK(decoder.apply(q) == DeltaApplyResult::Malformed);
        q = p1;
        q.partCount = uint8_t(p1.partCount + 1);   // disagrees with part 0
        CHECK(decoder.apply(q) == DeltaApplyResult::Malformed);
        q = p1;
        q.firstSlot = p0.firstSlot;   // overlaps part 0
        if (q.firstSlot + q.slotCount <= kPlayers) CHECK(decoder.apply(q) == DeltaApplyResult::Malformed);
        CHECK(!decoder.complete(1));
        for (size_t i = 1; i < copies.size(); ++i) {
            DecodedMessage h;
            CHECK(decodeMessage(copies[i], options, h));
            const DeltaApplyResult r = decoder.apply(std::get<DeltaSnapshot>(h.body));
            CHECK(r == (i + 1 == copies.size() ? DeltaApplyResult::Completed : DeltaApplyResult::Applied));
        }
        CHECK(sameSlots(decoder.complete(1), slots.data(), kPlayers));
        CHECK(decoder.apply(p0) == DeltaApplyResult::Duplicate);
        // Far older than anything kept.
        q = p0;
        for (uint32_t t = 2; t < 2 + kDeltaHistoryTicks; ++t) {
            crowd.step();
            crowd.fill(slots);
            CHECK(history.store(t, slots));
            compressor.acknowledge(decoder.ack());
            CHECK(compressor.encode(t, 1, kSession, parts));
            for (const DeltaPart& part : parts) {
                DecodedMessage h;
                CHECK(decodeMessage(part.message, options, h));
                decoder.apply(std::get<DeltaSnapshot>(h.body));
            }
        }
        q.snapshotTick = 1;
        CHECK(decoder.apply(q) == DeltaApplyResult::Stale);
    }
}

// ---- Interpolation --------------------------------------------------------------------------

static NetEntityState
stateAt(glm::vec3 position, glm::vec3 velocity, float yaw, uint16_t incarnation = 1, uint8_t flags = 0) {
    PoseState p;
    p.position = {position.x, position.y, position.z};
    p.linearVelocity = {velocity.x, velocity.y, velocity.z};
    p.rotation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    p.flags = flags;
    return quantizeState(p, incarnation);
}

static bool near(glm::vec3 a, glm::vec3 b, float e = 2e-3f) { return glm::length(a - b) <= e; }

static void testInterpolation() {
    NetworkSettings settings;
    settings.tickRate = 120;
    settings.interpolationMs = 25;
    settings.maxExtrapolationMs = 50;
    const InterpolationSettings is = interpolationSettings(settings);
    CHECK(std::fabs(is.delayTicks - 3.0) < 1e-9);   // the 3-tick jitter buffer
    CHECK(std::fabs(is.maxExtrapolationTicks - 6.0) < 1e-9);

    InterpolationEngine engine(4, is);
    InterpolatedPose out;
    CHECK(engine.sample(0, {10, 0}, out) == InterpolationResult::Empty);
    CHECK(engine.sample(9, {10, 0}, out) == InterpolationResult::Empty);

    // Constant velocity: Hermite reproduces the straight line exactly.
    const glm::vec3 v(6, 0, -3);
    for (uint32_t t = 10; t <= 14; ++t) engine.push(0, t, stateAt(v * (float(t) / 120), v, 0.1f * t));
    CHECK(engine.sampleCount(0) == 5);
    CHECK(engine.sample(0, {12, 0.25f}, out) == InterpolationResult::Interpolated);
    CHECK(near(out.position, v * (12.25f / 120)));
    CHECK(near(out.velocity, v, 0.02f));
    // Rotation: halfway between yaw 1.2 and 1.3.
    const glm::quat expect(std::cos(1.25f / 2), 0, std::sin(1.25f / 2), 0);
    CHECK(engine.sample(0, {12, 0.5f}, out) == InterpolationResult::Interpolated);
    CHECK(std::fabs(glm::dot(out.rotation, expect)) > 0.99999f);
    // Exactly on a sample.
    CHECK(engine.sample(0, {11, 0}, out) == InterpolationResult::Interpolated);
    CHECK(near(out.position, v * (11.0f / 120)));

    // Hermite follows the velocities through a curve; linear cuts the corner.
    InterpolationSettings linear = is;
    linear.hermite = false;
    InterpolationEngine flat(1, linear), curved(1, is);
    // A point on a circle of radius 2 m at 3 rad/s, sampled every 4 ticks (30 Hz snapshots).
    auto circle = [](float t) { return glm::vec3(2 * std::cos(3 * t), 0, 2 * std::sin(3 * t)); };
    auto circleV = [](float t) { return glm::vec3(-6 * std::sin(3 * t), 0, 6 * std::cos(3 * t)); };
    for (uint32_t t = 0; t <= 16; t += 4) {
        flat.push(0, t, stateAt(circle(t / 120.0f), circleV(t / 120.0f), 0));
        curved.push(0, t, stateAt(circle(t / 120.0f), circleV(t / 120.0f), 0));
    }
    InterpolatedPose a, b;
    CHECK(flat.sample(0, {6, 0}, a) == InterpolationResult::Interpolated);
    CHECK(curved.sample(0, {6, 0}, b) == InterpolationResult::Interpolated);
    const glm::vec3 truth = circle(6 / 120.0f);
    CHECK(glm::length(b.position - truth) < glm::length(a.position - truth) * 0.2f);
    CHECK(glm::length(b.position - truth) < 1e-3f);

    // Past the newest sample: velocity carries it for 6 ticks, then it holds.
    CHECK(engine.sample(0, {16, 0}, out) == InterpolationResult::Extrapolated);
    CHECK(near(out.position, v * (16.0f / 120)));
    CHECK(engine.sample(0, {20, 0}, out) == InterpolationResult::Extrapolated);
    CHECK(engine.sample(0, {25, 0}, out) == InterpolationResult::Held);
    CHECK(near(out.position, v * (20.0f / 120)));   // stopped at the limit
    // Sleeping objects don't drift.
    engine.push(1, 10, stateAt({1, 2, 3}, {0, 0, 0}, 0, 1, PoseState::kSleeping));
    CHECK(engine.sample(1, {14, 0}, out) == InterpolationResult::Extrapolated && near(out.position, {1, 2, 3}));
    // Before the oldest sample: held at it.
    CHECK(engine.sample(0, {8, 0}, out) == InterpolationResult::Held && near(out.position, v * (10.0f / 120)));

    // Out of order and duplicates.
    InterpolationEngine order(1, is);
    for (uint32_t t : {5u, 3u, 4u, 4u, 7u, 6u, 3u})
        order.push(0, t, stateAt(glm::vec3(float(t), 0, 0), {120, 0, 0}, 0));
    CHECK(order.sampleCount(0) == 5);
    CHECK(order.sample(0, {4, 0.5f}, out) == InterpolationResult::Interpolated && near(out.position, {4.5f, 0, 0}));
    // Full: the oldest goes, and anything older than everything kept is dropped.
    InterpolationEngine small(1, [&] {
        InterpolationSettings s4 = is;
        s4.samplesPerSlot = 4;
        return s4;
    }());
    for (uint32_t t = 1; t <= 6; ++t) small.push(0, t, stateAt({float(t), 0, 0}, {120, 0, 0}, 0));
    CHECK(small.sampleCount(0) == 4);
    small.push(0, 2, stateAt({2, 0, 0}, {120, 0, 0}, 0));
    CHECK(small.sampleCount(0) == 4);
    CHECK(small.sample(0, {2, 0}, out) == InterpolationResult::Held && near(out.position, {3, 0, 0}));
    // Full, and a late sample lands between newer ones.
    small.push(0, 8, stateAt({8, 0, 0}, {120, 0, 0}, 0));
    small.push(0, 7, stateAt({7, 0, 0}, {120, 0, 0}, 0));
    CHECK(small.sampleCount(0) == 4);
    CHECK(small.sample(0, {7, 0.5f}, out) == InterpolationResult::Interpolated && near(out.position, {7.5f, 0, 0}));
    CHECK(small.sample(0, {5, 0.5f}, out) == InterpolationResult::Interpolated && near(out.position, {5.5f, 0, 0}));
    CHECK(small.sample(0, {4, 0.5f}, out) == InterpolationResult::Held);

    // A teleport or a respawn starts the slot over, and late samples from before it are dropped.
    InterpolationEngine jump(1, is);
    jump.push(0, 1, stateAt({0, 0, 0}, {0, 0, 0}, 0));
    jump.push(0, 2, stateAt({0, 0, 0}, {0, 0, 0}, 0));
    jump.push(0, 4, stateAt({50, 0, 0}, {0, 0, 0}, 0, 1, PoseState::kTeleport));
    CHECK(jump.sampleCount(0) == 1);
    jump.push(0, 3, stateAt({1, 0, 0}, {0, 0, 0}, 0));
    CHECK(jump.sampleCount(0) == 1);
    jump.push(0, 5, stateAt({50, 0, 0}, {0, 0, 0}, 0));
    CHECK(jump.sample(0, {4, 0.5f}, out) == InterpolationResult::Interpolated && near(out.position, {50, 0, 0}));
    jump.push(0, 6, stateAt({-20, 0, 0}, {0, 0, 0}, 0, 2));   // respawned elsewhere
    CHECK(jump.sampleCount(0) == 1 && jump.sample(0, {6, 0}, out) == InterpolationResult::Interpolated);
    CHECK(out.incarnation == 2 && near(out.position, {-20, 0, 0}));
    jump.push(0, 7, NetEntityState{});   // despawned
    CHECK(jump.sampleCount(0) == 0);

    // The clock follows the earliest arrivals; render time trails it by 3 ticks.
    InterpolationEngine clock(1, is);
    CHECK(!clock.hasClock() && clock.renderTime(5).tick == 0);
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> jitter(0, 0.008);   // up to ~1 tick late
    const double start = 100.0;
    for (uint32_t t = 0; t < 600; ++t) clock.observeServerTick(500 + t, start + t / 120.0 + 0.030 + jitter(rng));
    CHECK(clock.hasClock());
    const double now = start + 600 / 120.0 + 0.030;
    const NetTime rt = clock.renderTime(now);
    const double shown = double(rt.tick) + rt.fraction;
    // The newest tick is ~1100 at `now` (arrival offset 30 ms included); 3 behind it.
    CHECK(std::fabs(shown - (1100 - 3)) < 0.6);

    // The jitter buffer: snapshots arrive 0..2 ticks late (a 120 Hz stream with up to
    // ~17 ms jitter), and objects never run out of samples to interpolate towards.
    InterpolationEngine live(1, is);
    struct Arrival {
        double at;
        uint32_t tick;
    };
    std::vector<Arrival> arrivals;
    std::uniform_int_distribution<int> late(0, 2);
    for (uint32_t t = 0; t < 1200; ++t) arrivals.push_back({t / 120.0 + 0.020 + late(rng) / 120.0, t});
    std::sort(arrivals.begin(), arrivals.end(), [](const Arrival& x, const Arrival& y) { return x.at < y.at; });
    size_t next = 0;
    uint32_t interpolated = 0, other = 0;
    for (double frame = 0; frame < 10.0; frame += 1.0 / 144) {   // a 144 Hz display
        while (next < arrivals.size() && arrivals[next].at <= frame) {
            const uint32_t t = arrivals[next++].tick;
            live.push(0, t, stateAt(v * (t / 120.0f), v, 0));
            live.observeServerTick(t, arrivals[next - 1].at);
        }
        if (frame < 1.0) continue;   // let the clock settle
        const NetTime when = live.renderTime(frame);
        const InterpolationResult r = live.sample(0, when, out);
        if (r == InterpolationResult::Interpolated) {
            ++interpolated;
            CHECK(near(out.position, v * ((when.tick + when.fraction) / 120.0f), 5e-3f));
        } else {
            ++other;
        }
    }
    CHECK(interpolated > 1200 && other == 0);
}

// ---- One encoder, many clients ---------------------------------------------------------

// Clients with the same baseline share one encoding; each still rebuilds the host's slots.
static void testSharedEncoder() {
    SnapshotHistory history(kPlayers);
    SnapshotDeltaEncoder encoder(history);
    constexpr int kClients = 6;
    std::vector<std::unique_ptr<SnapshotDeltaCompressor>> compressors;
    std::vector<std::unique_ptr<SnapshotDeltaDecoder>> decoders;
    for (int c = 0; c < kClients; ++c) {
        compressors.push_back(std::make_unique<SnapshotDeltaCompressor>(encoder));
        decoders.push_back(std::make_unique<SnapshotDeltaDecoder>(kPlayers));
    }
    Crowd crowd(kPlayers);
    std::vector<NetEntityState> slots(kPlayers);
    DecodedMessage holder;
    uint64_t calls = 0;
    for (uint32_t tick = 1; tick <= 300; ++tick) {
        crowd.step();
        crowd.fill(slots);
        CHECK(history.store(tick, slots));
        std::vector<std::span<const DeltaPart>> sent(kClients);
        for (int c = 0; c < kClients; ++c) {
            // Clients 0-2 ack every tick, 3-5 every third: two baselines per tick at most
            // (plus keyframes at the start).
            if (tick > 1 && (c < 3 || tick % 3 == 0)) compressors[c]->acknowledge(decoders[c]->ack());
            CHECK(compressors[c]->encode(tick, 1, kSession, sent[c]));
            ++calls;
        }
        // Everyone's parts are still valid after the others encoded.
        for (int c = 0; c < kClients; ++c) {
            if (c > 0 && compressors[c]->baselineTick() == compressors[0]->baselineTick() &&
                compressors[c]->hasBaseline() == compressors[0]->hasBaseline())
                CHECK(sent[c].data() == sent[0].data());   // the same encoding
            for (const DeltaPart& p : sent[c]) decoders[c]->apply(decodePart(p, holder));
            CHECK(sameSlots(decoders[c]->complete(tick), slots.data(), kPlayers));
        }
    }
    CHECK(encoder.encodeCount() <= 2 * 300 + 2);
    CHECK(encoder.encodeCount() < calls / 2);
    // A baseline or tick the history doesn't have.
    std::span<const DeltaPart> parts;
    const uint32_t gone = 1;
    CHECK(!encoder.encode(300, &gone, 1, kSession, parts) && parts.empty());
    CHECK(!encoder.encode(301, nullptr, 1, kSession, parts));
    // A cache of one: a second baseline replaces the first.
    SnapshotDeltaEncoder single(history, kMaxUnreliableMessageBytes, 1);
    const uint32_t b1 = 298, b2 = 299;
    CHECK(single.encode(300, &b1, 1, kSession, parts));
    CHECK(single.encode(300, &b2, 1, kSession, parts));
    CHECK(single.encode(300, &b1, 1, kSession, parts));
    CHECK(single.encodeCount() == 3);
    CHECK(encoder.encode(300, &b1, 1, kSession, parts) && encoder.encode(300, &b1, 1, kSession, parts));
}

// ---- Time --------------------------------------------------------------------------------

// Server delta generation for every client and client ingestion, per 120 Hz tick: the brief's
// budget is 150 us. 63 clients with round trips of 25 to 83 ms (8 distinct baselines) share
// one encoder. Asserted in optimised builds only.
static void testTiming() {
    constexpr int kClients = kPlayers - 1;
    SnapshotHistory history(kPlayers);
    SnapshotDeltaEncoder encoder(history);
    std::vector<std::unique_ptr<SnapshotDeltaCompressor>> compressors;
    for (int i = 0; i < kClients; ++i) compressors.push_back(std::make_unique<SnapshotDeltaCompressor>(encoder));
    SnapshotDeltaCompressor alone(history);   // one client's cost without sharing
    SnapshotDeltaDecoder decoder(kPlayers);
    InterpolationEngine interpolation(kPlayers, interpolationSettings([] {
                                          NetworkSettings s;
                                          s.tickRate = 120;
                                          s.interpolationMs = 25;
                                          return s;
                                      }()));
    Crowd crowd(kPlayers);
    std::vector<NetEntityState> slots(kPlayers);
    DecodedMessage holder;
    DecodeOptions options;
    options.delivery = Delivery::Unreliable;
    options.receiver = PeerRole::Client;
    using Clock = std::chrono::steady_clock;
    double hostWorst = 0, hostTotal = 0, clientWorst = 0, clientTotal = 0, aloneTotal = 0;
    uint32_t verified = 0;
    constexpr int kTicks = 2400, kWarm = 120;
    const uint64_t encodesBefore = encoder.encodeCount();
    for (int i = 0; i < kTicks; ++i) {
        const uint32_t tick = 1 + uint32_t(i);
        crowd.step();
        crowd.fill(slots);
        // Host: store the tick, then one delta per client. Client c's acks take 3 + c % 8 ticks.
        const auto h0 = Clock::now();
        history.store(tick, slots);
        std::span<const DeltaPart> parts;
        for (int c = 0; c < kClients; ++c) {
            const uint32_t lag = 3 + uint32_t(c % 8);
            if (tick > lag) compressors[c]->acknowledge({tick - lag, 0});
            compressors[c]->encode(tick, 1, kSession, parts);
        }
        const auto h1 = Clock::now();
        std::span<const DeltaPart> own;
        if (tick > 3) alone.acknowledge({tick - 3, 0});
        alone.encode(tick, 1, kSession, own);
        const auto h2 = Clock::now();
        // One client (lag 3): decode the messages, apply them, interpolate everyone.
        for (const DeltaPart& p : own) {
            if (!decodeMessage(p.message, options, holder)) continue;
            decoder.apply(std::get<DeltaSnapshot>(holder.body));
        }
        if (const NetEntityState* done = decoder.complete(tick)) {
            interpolation.pushSnapshot(tick, {done, kPlayers});
            verified += sameSlots(done, slots.data(), kPlayers);
        }
        InterpolatedPose pose;
        for (uint16_t s = 0; s < kPlayers; ++s) interpolation.sample(s, {tick - 3, 0.5f}, pose);
        (void)decoder.ack();
        const auto h3 = Clock::now();
        if (i < kWarm) continue;
        const double host = std::chrono::duration<double, std::micro>(h1 - h0).count();
        const double client = std::chrono::duration<double, std::micro>(h3 - h2).count();
        hostWorst = std::max(hostWorst, host);
        clientWorst = std::max(clientWorst, client);
        hostTotal += host;
        clientTotal += client;
        aloneTotal += std::chrono::duration<double, std::micro>(h2 - h1).count();
    }
    const int n = kTicks - kWarm;
    CHECK(verified == uint32_t(kTicks));
    const double encodesPerTick = double(encoder.encodeCount() - encodesBefore) / kTicks;
    std::printf(
        "per 120 Hz tick, 64 moving players: host deltas for %d clients mean %.1f us (worst %.1f, %.1f encodes per "
        "tick, one encode %.1f us), client ingest + interpolation mean %.1f us (worst %.1f)\n",
        kClients, hostTotal / n, hostWorst, encodesPerTick, aloneTotal / n, clientTotal / n, clientWorst);
#ifdef NDEBUG
    CHECK(hostTotal / n < 150);
    CHECK(clientTotal / n < 150);
#endif
}

int main() {
    testQuantization();
    testMovingCrowdFitsOnePart();
    testKeyframeAndOldBaselineSplit();
    testWorstCase();
    testLossyLink();
    testAcksAndBaselines();
    testSmallChanges();
    testMalformed();
    testSharedEncoder();
    testInterpolation();
    testTiming();
    if (failures) {
        std::fprintf(stderr, "test_snapshot_delta: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_snapshot_delta: all checks passed\n");
    return 0;
}
