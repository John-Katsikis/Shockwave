#pragma once

// The visitors that run a message's fields() list: one writes the fields, one reads and
// checks them. NetworkCapture.cpp and the reference generator add their own.

#include <Network/Messages.h>

#include <cmath>
#include <type_traits>

namespace shockwave::net::detail {

inline WireStatus statusAt(WireStatusCode code, size_t offset, const char* field) {
    return {code, uint32_t(offset), field};
}

inline WireStatusCode statusFor(WireError error) {
    switch (error) {
    case WireError::None: return WireStatusCode::Ok;
    case WireError::Overflow: return WireStatusCode::Overflow;
    case WireError::Truncated: return WireStatusCode::Truncated;
    case WireError::NonFinite: return WireStatusCode::NonFinite;
    case WireError::BadString: return WireStatusCode::BadString;
    case WireError::TooLong: return WireStatusCode::OutOfRange;
    }
    return WireStatusCode::Invalid;
}

// Reads past `count` state records at the front of `reader`, checking each one.
inline bool scanRecords(WireReader& reader, uint16_t count, WireStatus& status) {
    for (uint16_t i = 0; i < count; ++i) {
        const size_t at = reader.offset();
        uint64_t entity;
        uint32_t incarnation;
        uint16_t profile, bytes;
        if (!reader.u64(entity) || !reader.u32(incarnation) || !reader.u16(profile) || !reader.u16(bytes)) {
            status = statusAt(WireStatusCode::Truncated, reader.offset(), "records");
            return false;
        }
        if (entity == 0) {
            status = statusAt(WireStatusCode::OutOfRange, at, "records.entity");
            return false;
        }
        if (profile == 0 || profile > 1023) {
            status = statusAt(WireStatusCode::OutOfRange, at + 12, "records.profile_id");
            return false;
        }
        if (bytes > kMaxStateBytes) {
            status = statusAt(WireStatusCode::OutOfRange, at + 14, "records.state_bytes");
            return false;
        }
        std::span<const uint8_t> state;
        if (!reader.view(bytes, state)) {
            status = statusAt(WireStatusCode::Truncated, reader.offset(), "records.state");
            return false;
        }
    }
    return true;
}

class WriteFields {
public:
    explicit WriteFields(WireWriter& writer) : m_w(writer) {}
    const WireStatus& status() const { return m_status; }
    bool ok() const { return bool(m_status); }

    void u8(const char* name, uint8_t value, uint8_t lo = 0, uint8_t hi = 0xFF) {
        if (range(name, value, lo, hi)) check(name, m_w.u8(value));
    }
    void u16(const char* name, uint16_t value, uint16_t lo = 0, uint16_t hi = 0xFFFF) {
        if (range(name, value, lo, hi)) check(name, m_w.u16(value));
    }
    void u32(const char* name, uint32_t value, uint32_t lo = 0, uint32_t hi = 0xFFFFFFFFu) {
        if (range(name, value, lo, hi)) check(name, m_w.u32(value));
    }
    void u64(const char* name, uint64_t value, uint64_t lo = 0, uint64_t hi = ~uint64_t(0)) {
        if (range(name, value, lo, hi)) check(name, m_w.u64(value));
    }
    void i8(const char* name, int8_t value, int8_t lo, int8_t hi) {
        if (range(name, value, lo, hi)) check(name, m_w.i8(value));
    }
    void i16(const char* name, int16_t value, int16_t lo, int16_t hi) {
        if (range(name, value, lo, hi)) check(name, m_w.i16(value));
    }
    void bits8(const char* name, uint8_t value, uint8_t mask) {
        if (bits(name, value, mask)) check(name, m_w.u8(value));
    }
    void bits16(const char* name, uint16_t value, uint16_t mask) {
        if (bits(name, value, mask)) check(name, m_w.u16(value));
    }
    void reason(const char* name, ReasonCode value) { u16(name, uint16_t(value), 0, kLastReasonCode); }
    void f32(const char* name, float value, float absMax) {
        if (!m_status) return;
        if (!std::isfinite(value)) return fail(WireStatusCode::NonFinite, name);
        if (std::fabs(value) > absMax) return fail(WireStatusCode::OutOfRange, name);
        check(name, m_w.f32(value));
    }
    void vec3(const char* name, const NetVec3& value, float absMax) {
        f32(name, value.x, absMax);
        f32(name, value.y, absMax);
        f32(name, value.z, absMax);
    }
    void quat(const char* name, const NetQuat& value) {
        f32(name, value.x, 1.01f);
        f32(name, value.y, 1.01f);
        f32(name, value.z, 1.01f);
        f32(name, value.w, 1.01f);
    }
    template <size_t N>
    void fixed(const char* name, const std::array<uint8_t, N>& value) {
        if (m_status) check(name, m_w.raw(value));
    }
    void nonce(const char* name, const Nonce& value) { fixed(name, value); }
    void digest(const char* name, const Digest& value) { fixed(name, value); }
    void string(const char* name, std::string_view value, uint16_t minBytes, uint16_t maxBytes) {
        if (!m_status) return;
        if (value.size() < minBytes || value.size() > maxBytes) return fail(WireStatusCode::OutOfRange, name);
        check(name, m_w.string(value, maxBytes));
    }
    void bytes(const char* name, std::span<const uint8_t> value, uint16_t minBytes, uint16_t maxBytes) {
        if (!m_status) return;
        if (value.size() < minBytes || value.size() > maxBytes) return fail(WireStatusCode::OutOfRange, name);
        check(name, m_w.bytes(value, maxBytes));
    }
    template <size_t N>
    void inputs(const char* name, uint8_t count, const std::array<InputCommand, N>& commands) {
        if (!m_status) return;
        if (count < 1 || count > N) return fail(WireStatusCode::OutOfRange, name);
        check(name, m_w.u8(count));
        for (uint8_t i = 0; i < count && m_status; ++i) {
            InputCommand::fields(commands[i], *this);
            if (const char* bad = commands[i].invalid(); bad && m_status) fail(WireStatusCode::Invalid, bad);
        }
    }
    void records(const char* name, uint16_t count, std::span<const uint8_t> bytes) {
        if (!m_status) return;
        // The records are already encoded; check they're whole and valid before copying.
        WireReader reader(bytes);
        WireStatus scan;
        if (!scanRecords(reader, count, scan)) return fail(scan.code, scan.field);
        if (reader.remaining() != 0) return fail(WireStatusCode::TrailingBytes, name);
        check(name, m_w.u16(count));
        check(name, m_w.raw(bytes));
    }

private:
    template <class T>
    bool range(const char* name, T value, T lo, T hi) {
        if (!m_status) return false;
        if (value < lo || value > hi) {
            fail(WireStatusCode::OutOfRange, name);
            return false;
        }
        return true;
    }
    template <class T>
    bool bits(const char* name, T value, T mask) {
        if (!m_status) return false;
        if (value & ~mask) {
            fail(WireStatusCode::OutOfRange, name);
            return false;
        }
        return true;
    }
    void check(const char* name, bool written) {
        if (!written && m_status) fail(statusFor(m_w.error()), name);
    }
    void fail(WireStatusCode code, const char* name) {
        if (m_status) m_status = statusAt(code, m_w.size(), name);
    }

    WireWriter& m_w;
    WireStatus m_status;
};

class ReadFields {
public:
    explicit ReadFields(std::span<const uint8_t> bytes) : m_bytes(bytes), m_r(bytes) {}
    WireReader& reader() { return m_r; }
    const WireStatus& status() const { return m_status; }
    bool ok() const { return bool(m_status); }

    void u8(const char* name, uint8_t& value, uint8_t lo = 0, uint8_t hi = 0xFF) { read(name, value, lo, hi); }
    void u16(const char* name, uint16_t& value, uint16_t lo = 0, uint16_t hi = 0xFFFF) { read(name, value, lo, hi); }
    void u32(const char* name, uint32_t& value, uint32_t lo = 0, uint32_t hi = 0xFFFFFFFFu) {
        read(name, value, lo, hi);
    }
    void u64(const char* name, uint64_t& value, uint64_t lo = 0, uint64_t hi = ~uint64_t(0)) {
        read(name, value, lo, hi);
    }
    void i8(const char* name, int8_t& value, int8_t lo, int8_t hi) { read(name, value, lo, hi); }
    void i16(const char* name, int16_t& value, int16_t lo, int16_t hi) { read(name, value, lo, hi); }
    void bits8(const char* name, uint8_t& value, uint8_t mask) { readBits(name, value, mask); }
    void bits16(const char* name, uint16_t& value, uint16_t mask) { readBits(name, value, mask); }
    void reason(const char* name, ReasonCode& value) {
        uint16_t raw = 0;
        u16(name, raw, 0, kLastReasonCode);
        value = ReasonCode(raw);
    }
    void f32(const char* name, float& value, float absMax) {
        if (!m_status) return;
        const size_t at = m_r.offset();
        float v;
        if (!m_r.f32(v)) return fail(statusFor(m_r.error()), at, name);
        if (std::fabs(v) > absMax) return fail(WireStatusCode::OutOfRange, at, name);
        value = v;
    }
    void vec3(const char* name, NetVec3& value, float absMax) {
        f32(name, value.x, absMax);
        f32(name, value.y, absMax);
        f32(name, value.z, absMax);
    }
    void quat(const char* name, NetQuat& value) {
        f32(name, value.x, 1.01f);
        f32(name, value.y, 1.01f);
        f32(name, value.z, 1.01f);
        f32(name, value.w, 1.01f);
    }
    template <size_t N>
    void fixed(const char* name, std::array<uint8_t, N>& value) {
        if (!m_status) return;
        const size_t at = m_r.offset();
        if (!m_r.raw(value)) fail(statusFor(m_r.error()), at, name);
    }
    void nonce(const char* name, Nonce& value) { fixed(name, value); }
    void digest(const char* name, Digest& value) { fixed(name, value); }
    void string(const char* name, std::string_view& value, uint16_t minBytes, uint16_t maxBytes) {
        if (!m_status) return;
        const size_t at = m_r.offset();
        std::string_view v;
        if (!m_r.string(v, maxBytes)) return fail(statusFor(m_r.error()), at, name);
        if (v.size() < minBytes) return fail(WireStatusCode::OutOfRange, at, name);
        value = v;
    }
    void bytes(const char* name, std::span<const uint8_t>& value, uint16_t minBytes, uint16_t maxBytes) {
        if (!m_status) return;
        const size_t at = m_r.offset();
        std::span<const uint8_t> v;
        if (!m_r.bytes(v, maxBytes)) return fail(statusFor(m_r.error()), at, name);
        if (v.size() < minBytes) return fail(WireStatusCode::OutOfRange, at, name);
        value = v;
    }
    template <size_t N>
    void inputs(const char* name, uint8_t& count, std::array<InputCommand, N>& commands) {
        if (!m_status) return;
        const size_t at = m_r.offset();
        uint8_t n;
        if (!m_r.u8(n)) return fail(WireStatusCode::Truncated, at, name);
        if (n < 1 || n > N) return fail(WireStatusCode::OutOfRange, at, name);
        for (uint8_t i = 0; i < n && m_status; ++i) {
            const size_t commandAt = m_r.offset();
            InputCommand::fields(commands[i], *this);
            if (!m_status) return;
            if (const char* bad = commands[i].invalid()) return fail(WireStatusCode::Invalid, commandAt, bad);
        }
        count = n;
    }
    void records(const char* name, uint16_t& count, std::span<const uint8_t>& bytes) {
        if (!m_status) return;
        const size_t at = m_r.offset();
        uint16_t n;
        if (!m_r.u16(n)) return fail(WireStatusCode::Truncated, at, name);
        const size_t start = m_r.offset();
        WireStatus scan;
        if (!scanRecords(m_r, n, scan)) {
            m_status = scan;
            return;
        }
        count = n;
        bytes = m_bytes.subspan(start, m_r.offset() - start);
    }

private:
    template <class T>
    void read(const char* name, T& value, T lo, T hi) {
        if (!m_status) return;
        const size_t at = m_r.offset();
        T v{};
        bool ok;
        if constexpr (std::is_same_v<T, uint8_t>)
            ok = m_r.u8(v);
        else if constexpr (std::is_same_v<T, uint16_t>)
            ok = m_r.u16(v);
        else if constexpr (std::is_same_v<T, uint32_t>)
            ok = m_r.u32(v);
        else if constexpr (std::is_same_v<T, uint64_t>)
            ok = m_r.u64(v);
        else if constexpr (std::is_same_v<T, int8_t>)
            ok = m_r.i8(v);
        else
            ok = m_r.i16(v);
        if (!ok) return fail(statusFor(m_r.error()), at, name);
        if (v < lo || v > hi) return fail(WireStatusCode::OutOfRange, at, name);
        value = v;
    }
    template <class T>
    void readBits(const char* name, T& value, T mask) {
        T v{};
        read(name, v, T(0), T(~T(0)));
        if (!m_status) return;
        if (v & ~mask) return fail(WireStatusCode::OutOfRange, m_r.offset() - sizeof(T), name);
        value = v;
    }
    void fail(WireStatusCode code, size_t at, const char* name) {
        if (m_status) m_status = statusAt(code, at, name);
    }

    std::span<const uint8_t> m_bytes;
    WireReader m_r;
    WireStatus m_status;
};

}   // namespace shockwave::net::detail
