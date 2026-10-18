#pragma once

// Results, identities and reason codes shared by every part of networking.
//
// Routine failures (a peer that is gone, a full queue, malformed bytes) are values, not
// exceptions: functions return NetResult<T>, which holds either the value or a NetError.

#include <Core/Log.h>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace shockwave {
SW_LOG_CATEGORY(LogNetwork);
}   // namespace shockwave

namespace shockwave::net {

enum class NetErrorCode : uint8_t {
    None,
    Unavailable,   // the feature or service isn't in this build, or isn't running
    InvalidArgument,
    InvalidState,   // e.g. accept() on a connection that isn't waiting to be accepted
    NotConnected,
    CapacityExceeded,   // a fixed pool or limit is full
    AddressInUse,
    Closed,
    Malformed,
    Io,
};

const char* netErrorName(NetErrorCode code);

// `message` is a static string: errors never allocate.
struct NetError {
    NetErrorCode code = NetErrorCode::None;
    const char* message = "";
};

template <class T>
class [[nodiscard]] NetResult {
public:
    NetResult(T value) : m_value(std::move(value)) {}
    NetResult(NetError error) : m_error(error) {}

    bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }
    T& value() { return *m_value; }
    const T& value() const { return *m_value; }
    T* operator->() { return &*m_value; }
    const T* operator->() const { return &*m_value; }
    const NetError& error() const { return m_error; }

private:
    std::optional<T> m_value;
    NetError m_error{};
};

template <>
class [[nodiscard]] NetResult<void> {
public:
    NetResult() = default;
    NetResult(NetError error) : m_error(error) {}

    bool ok() const { return m_error.code == NetErrorCode::None; }
    explicit operator bool() const { return ok(); }
    const NetError& error() const { return m_error; }

private:
    NetError m_error{};
};

// Opaque identity of a remote machine, as the transport authenticated it (a SteamID64,
// or a loopback test identity). Never a player slot, and never taken from a message.
struct PeerId {
    uint64_t value = 0;
    bool valid() const { return value != 0; }
    friend bool operator==(PeerId, PeerId) = default;
};

// Why a session or connection ended. These numbers are part of wire protocol v1 (the
// Reject and Goodbye messages): append new codes, never renumber.
enum class ReasonCode : uint16_t {
    Normal = 0,
    ProtocolMismatch = 1,
    ContentMismatch = 2,
    ServerFull = 3,
    NotLobbyMember = 4,
    AuthenticationFailed = 5,
    AuthUnavailable = 6,
    Timeout = 7,
    MalformedPacket = 8,
    RateLimited = 9,
    HostLeft = 10,
    MapNotLoaded = 11,
    SimulationOverrun = 12,
    ServiceUnavailable = 13,
    UnsupportedFeature = 14,
};
inline constexpr uint16_t kLastReasonCode = 14;

// "ProtocolMismatch", or nullptr for a number with no name.
const char* reasonName(ReasonCode reason);

// Fills `out` from the operating system's cryptographic random source (getrandom,
// arc4random_buf, BCryptGenRandom). False if that source fails; there's no fallback to a
// weaker generator.
bool secureRandomBytes(std::span<uint8_t> out);

}   // namespace shockwave::net
