#pragma once

// Bounded byte writer and reader for the wire format: every integer is written byte by
// byte in little endian, and every float as its IEEE 754 bits. Nothing is ever copied
// from a C++ object's memory, so the bytes are the same on every compiler and CPU.
//
// Both work on a caller's buffer and never allocate. A writer that runs out of room, or
// is handed a value it can't encode, fails and stays failed. A reader method that fails
// returns false and doesn't move past the data it couldn't read.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace shockwave::net {

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4, "the wire format needs IEEE 754 floats");

enum class WireError : uint8_t {
    None,
    Overflow,   // writer: the buffer is full
    Truncated,   // reader: fewer bytes left than the field needs
    NonFinite,   // a float is NaN or infinite
    BadString,   // a string isn't valid UTF-8, holds a NUL, or is too long
    TooLong,   // a counted byte field is longer than its limit
};

const char* wireErrorName(WireError error);

// Strict UTF-8: no overlong forms, surrogates or code points past U+10FFFF; no NUL.
bool validUtf8(std::string_view text);

// True if tick (or sequence) a comes after b, allowing for wraparound: the two must be
// less than 2^31 apart.
constexpr bool tickNewer(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }

class WireWriter {
public:
    explicit WireWriter(std::span<uint8_t> buffer) : m_buffer(buffer) {}

    bool u8(uint8_t value);
    bool u16(uint16_t value);
    bool u32(uint32_t value);
    bool u64(uint64_t value);
    bool i8(int8_t value) { return u8(std::bit_cast<uint8_t>(value)); }
    bool i16(int16_t value) { return u16(std::bit_cast<uint16_t>(value)); }
    bool i32(int32_t value) { return u32(std::bit_cast<uint32_t>(value)); }
    // Refuses NaN and infinity.
    bool f32(float value);
    // Raw bytes, no count.
    bool raw(std::span<const uint8_t> bytes);
    // A u16 byte count, then the bytes; at most maxBytes, and valid UTF-8.
    bool string(std::string_view text, uint16_t maxBytes);
    // A u16 byte count, then the bytes; at most maxBytes.
    bool bytes(std::span<const uint8_t> data, uint16_t maxBytes);

    // Overwrites a u32 already written at `offset` (a length filled in afterwards).
    bool patchU32(size_t offset, uint32_t value);

    bool ok() const { return m_error == WireError::None; }
    WireError error() const { return m_error; }
    size_t size() const { return m_size; }
    std::span<const uint8_t> written() const { return m_buffer.first(m_size); }

private:
    uint8_t* reserve(size_t count);
    bool fail(WireError error);

    std::span<uint8_t> m_buffer;
    size_t m_size = 0;
    WireError m_error = WireError::None;
};

class WireReader {
public:
    explicit WireReader(std::span<const uint8_t> bytes) : m_bytes(bytes) {}

    bool u8(uint8_t& out);
    bool u16(uint16_t& out);
    bool u32(uint32_t& out);
    bool u64(uint64_t& out);
    bool i8(int8_t& out);
    bool i16(int16_t& out);
    bool i32(int32_t& out);
    bool f32(float& out);
    // Copies exactly out.size() bytes.
    bool raw(std::span<uint8_t> out);
    // Points `out` at the next `count` bytes, without copying.
    bool view(size_t count, std::span<const uint8_t>& out);
    // As the writer's string() and bytes(); the results point into the reader's bytes.
    bool string(std::string_view& out, uint16_t maxBytes);
    bool bytes(std::span<const uint8_t>& out, uint16_t maxBytes);

    size_t offset() const { return m_offset; }
    size_t remaining() const { return m_bytes.size() - m_offset; }
    // What made the last failing call fail.
    WireError error() const { return m_error; }

private:
    const uint8_t* take(size_t count);
    bool fail(WireError error);

    std::span<const uint8_t> m_bytes;
    size_t m_offset = 0;
    WireError m_error = WireError::None;
};

// Single-value helpers for code that patches or inspects fixed offsets.
inline void storeU16(uint8_t* out, uint16_t value) {
    out[0] = uint8_t(value);
    out[1] = uint8_t(value >> 8);
}
inline void storeU32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = uint8_t(value >> (8 * i));
}
inline void storeU64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out[i] = uint8_t(value >> (8 * i));
}
inline uint16_t loadU16(const uint8_t* in) { return uint16_t(in[0] | (in[1] << 8)); }
inline uint32_t loadU32(const uint8_t* in) {
    return uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}
inline uint64_t loadU64(const uint8_t* in) { return uint64_t(loadU32(in)) | (uint64_t(loadU32(in + 4)) << 32); }

}   // namespace shockwave::net
