#pragma once

// SHA-256 (FIPS 180-4), for the compatibility digests peers compare before a session
// starts (docs/Networking.md). It tells two builds' content apart; it isn't anti-cheat.
// Tested against the FIPS known-answer vectors in network_settings.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace shockwave::net {

using Sha256Digest = std::array<uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void update(std::span<const uint8_t> bytes);
    void update(std::string_view text);
    // Explicit little-endian integers, so a digest doesn't depend on the machine.
    void updateU16(uint16_t value);
    void updateU32(uint32_t value);
    void updateU64(uint64_t value);
    // The digest of everything added. The object can't be updated afterwards.
    Sha256Digest finish();

private:
    void block(const uint8_t* data);
    std::array<uint32_t, 8> m_state;
    std::array<uint8_t, 64> m_buffer{};
    size_t m_buffered = 0;
    uint64_t m_length = 0;   // bytes
    bool m_finished = false;
};

Sha256Digest sha256(std::span<const uint8_t> bytes);
Sha256Digest sha256(std::string_view text);
// Lowercase hexadecimal, 64 characters.
std::string toHex(const Sha256Digest& digest);

}   // namespace shockwave::net
