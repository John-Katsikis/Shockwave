#pragma once

// In-process transport for tests and tools: a LoopbackHub connects any number of
// ITransport endpoints in one process, on a virtual clock the test advances, with
// seeded fault injection. No sockets, threads, Steam or sleeps; the same seed gives the
// same deliveries on every platform.
//
// Messages are copied as bytes, exactly like a real transport: peers share no C++
// objects. Each connection's two directions are separate FaultLinks, each with its own
// seed. Reliable messages may be delayed (a "lost" one waits for its retry) but always
// arrive once and in order; unreliable ones may be dropped, duplicated and reordered.
// Closing a connection drops whatever it still had in flight, and the other side gets
// one Closed event.
//
// The hub is an object a test creates, never a global. It owns every buffer up front,
// so once endpoints are connected, sending and polling don't allocate. It must outlive
// its endpoints; if it doesn't, they stop working rather than crash.

#include <Network/Transport.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace shockwave::net {

// SplitMix64: a small generator whose sequence is the same on every compiler, unlike
// the standard library's distributions.
class DeterministicRandom {
public:
    explicit DeterministicRandom(uint64_t seed = 1) : m_state(seed) {}
    uint64_t next();
    double unit();   // [0, 1)
    // Uniform in [lo, hi].
    int64_t range(int64_t lo, int64_t hi);

private:
    uint64_t m_state;
};

struct FaultProfile {
    uint32_t latencyUs = 0;   // one way
    uint32_t jitterUs = 0;   // plus or minus, uniform
    double unreliableLoss = 0;   // 0..1
    double duplication = 0;   // unreliable only
    double reliableLoss = 0;   // each "loss" delays a reliable message by retryUs
    uint32_t retryUs = 0;   // 0: twice the latency plus 20 ms
    uint32_t bytesPerSecond = 0;   // 0: unlimited
    uint32_t maxQueueDelayUs = 1000000;   // throttled unreliable messages that would wait longer are dropped
};

struct FaultPreset {
    const char* name;
    FaultProfile profile;
    uint32_t durationTicks;   // how long a scenario with this preset runs
};

// clean, cafe, poor and hostile.
std::span<const FaultPreset> faultPresets();
const FaultPreset* findFaultPreset(std::string_view name);

// One direction of a connection: decides when (and whether) each message arrives.
class FaultLink {
public:
    FaultLink() = default;
    FaultLink(const FaultProfile& profile, uint64_t seed) : m_profile(profile), m_random(seed) {}

    void setProfile(const FaultProfile& profile) { m_profile = profile; }
    const FaultProfile& profile() const { return m_profile; }
    // While an outage lasts, unreliable messages are dropped and reliable ones wait for
    // it to end. Up to four at once; older ones are replaced.
    void addOutage(uint64_t startUs, uint64_t durationUs);

    struct Arrivals {
        uint8_t copies = 0;   // 0 lost, 1, or 2 when duplicated
        std::array<uint64_t, 2> atUs{};
    };
    Arrivals schedule(uint64_t nowUs, uint32_t bytes, Delivery delivery);

    struct Counters {
        uint64_t messages = 0, dropped = 0, duplicated = 0, retries = 0, throttleDropped = 0;
    };
    const Counters& counters() const { return m_counters; }

private:
    bool inOutage(uint64_t us, uint64_t& endUs) const;

    FaultProfile m_profile;
    DeterministicRandom m_random;
    std::array<std::array<uint64_t, 2>, 4> m_outages{};
    uint32_t m_nextOutage = 0;
    uint64_t m_lastReliableUs = 0;
    uint64_t m_linkFreeUs = 0;
    Counters m_counters;
};

struct LoopbackHubConfig {
    uint64_t seed = 1;
    // Messages in flight across the hub: small ones (up to 1152 bytes), large ones (up to
    // 16384), and connection control.
    uint32_t smallPackets = 4096;
    uint32_t largePackets = 128;
    uint32_t controlPackets = 1024;
    // Per connection: past this, send() reports Backpressure (reliable) or Dropped.
    uint32_t maxPendingReliableBytes = 65536;
    uint32_t maxPendingUnreliableBytes = 65536;
    FaultProfile faults;   // for every connection
};

struct LoopbackStats {
    uint32_t packetsInFlight = 0;
    uint32_t packetsInFlightHigh = 0;
    uint32_t smallBuffersHigh = 0;
    uint32_t largeBuffersHigh = 0;
    uint64_t delivered = 0;
    uint64_t staleDiscarded = 0;   // in flight on a connection that closed first
    uint64_t poolExhausted = 0;
};

class LoopbackHub {
public:
    explicit LoopbackHub(const LoopbackHubConfig& config = {});
    ~LoopbackHub();
    LoopbackHub(const LoopbackHub&) = delete;
    LoopbackHub& operator=(const LoopbackHub&) = delete;

    // A new endpoint with this transport identity. Listen on any TransportAddress of
    // kind Loopback; connect with the same address.
    std::unique_ptr<ITransport> createTransport(PeerId identity);

    uint64_t nowUs() const;
    void advance(uint64_t us);

    // Applies to every connection, now and later.
    void setFaults(const FaultProfile& faults);
    void addOutage(uint64_t startUs, uint64_t durationUs);

    LoopbackStats stats() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

}   // namespace shockwave::net
