#include <Network/Wire.h>

#include <cmath>
#include <cstring>

namespace shockwave::net {

const char* wireErrorName(WireError error) {
    switch (error) {
    case WireError::None: return "none";
    case WireError::Overflow: return "overflow";
    case WireError::Truncated: return "truncated";
    case WireError::NonFinite: return "non-finite float";
    case WireError::BadString: return "invalid string";
    case WireError::TooLong: return "too long";
    }
    return "unknown";
}

bool validUtf8(std::string_view text) {
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c == 0) return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t length;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            length = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            length = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            length = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (n - i < length) return false;
        for (size_t k = 1; k < length; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        static constexpr uint32_t smallest[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < smallest[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += length;
    }
    return true;
}

// Writer

bool WireWriter::fail(WireError error) {
    if (m_error == WireError::None) m_error = error;
    return false;
}

uint8_t* WireWriter::reserve(size_t count) {
    if (m_error != WireError::None) return nullptr;
    if (m_buffer.size() - m_size < count) {
        fail(WireError::Overflow);
        return nullptr;
    }
    uint8_t* out = m_buffer.data() + m_size;
    m_size += count;
    return out;
}

bool WireWriter::u8(uint8_t value) {
    uint8_t* out = reserve(1);
    if (!out) return false;
    out[0] = value;
    return true;
}

bool WireWriter::u16(uint16_t value) {
    uint8_t* out = reserve(2);
    if (!out) return false;
    storeU16(out, value);
    return true;
}

bool WireWriter::u32(uint32_t value) {
    uint8_t* out = reserve(4);
    if (!out) return false;
    storeU32(out, value);
    return true;
}

bool WireWriter::u64(uint64_t value) {
    uint8_t* out = reserve(8);
    if (!out) return false;
    storeU64(out, value);
    return true;
}

bool WireWriter::f32(float value) {
    if (!std::isfinite(value)) return fail(WireError::NonFinite);
    return u32(std::bit_cast<uint32_t>(value));
}

bool WireWriter::raw(std::span<const uint8_t> bytes) {
    uint8_t* out = reserve(bytes.size());
    if (!out) return false;
    if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
    return true;
}

bool WireWriter::string(std::string_view text, uint16_t maxBytes) {
    if (m_error != WireError::None) return false;
    if (text.size() > maxBytes || !validUtf8(text)) return fail(WireError::BadString);
    if (m_buffer.size() - m_size < 2 + text.size()) return fail(WireError::Overflow);
    u16(uint16_t(text.size()));
    return raw({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

bool WireWriter::bytes(std::span<const uint8_t> data, uint16_t maxBytes) {
    if (m_error != WireError::None) return false;
    if (data.size() > maxBytes) return fail(WireError::TooLong);
    if (m_buffer.size() - m_size < 2 + data.size()) return fail(WireError::Overflow);
    u16(uint16_t(data.size()));
    return raw(data);
}

bool WireWriter::patchU32(size_t offset, uint32_t value) {
    if (m_error != WireError::None) return false;
    if (offset > m_size || m_size - offset < 4) return fail(WireError::Overflow);
    storeU32(m_buffer.data() + offset, value);
    return true;
}

// Reader

bool WireReader::fail(WireError error) {
    m_error = error;
    return false;
}

const uint8_t* WireReader::take(size_t count) {
    if (remaining() < count) {
        fail(WireError::Truncated);
        return nullptr;
    }
    const uint8_t* in = m_bytes.data() + m_offset;
    m_offset += count;
    return in;
}

bool WireReader::u8(uint8_t& out) {
    const uint8_t* in = take(1);
    if (!in) return false;
    out = in[0];
    return true;
}

bool WireReader::u16(uint16_t& out) {
    const uint8_t* in = take(2);
    if (!in) return false;
    out = loadU16(in);
    return true;
}

bool WireReader::u32(uint32_t& out) {
    const uint8_t* in = take(4);
    if (!in) return false;
    out = loadU32(in);
    return true;
}

bool WireReader::u64(uint64_t& out) {
    const uint8_t* in = take(8);
    if (!in) return false;
    out = loadU64(in);
    return true;
}

bool WireReader::i8(int8_t& out) {
    uint8_t value;
    if (!u8(value)) return false;
    out = std::bit_cast<int8_t>(value);
    return true;
}

bool WireReader::i16(int16_t& out) {
    uint16_t value;
    if (!u16(value)) return false;
    out = std::bit_cast<int16_t>(value);
    return true;
}

bool WireReader::i32(int32_t& out) {
    uint32_t value;
    if (!u32(value)) return false;
    out = std::bit_cast<int32_t>(value);
    return true;
}

bool WireReader::f32(float& out) {
    if (remaining() < 4) return fail(WireError::Truncated);
    const float value = std::bit_cast<float>(loadU32(m_bytes.data() + m_offset));
    if (!std::isfinite(value)) return fail(WireError::NonFinite);
    m_offset += 4;
    out = value;
    return true;
}

bool WireReader::raw(std::span<uint8_t> out) {
    const uint8_t* in = take(out.size());
    if (!in) return false;
    if (!out.empty()) std::memcpy(out.data(), in, out.size());
    return true;
}

bool WireReader::view(size_t count, std::span<const uint8_t>& out) {
    const uint8_t* in = take(count);
    if (!in) return false;
    out = {in, count};
    return true;
}

bool WireReader::string(std::string_view& out, uint16_t maxBytes) {
    if (remaining() < 2) return fail(WireError::Truncated);
    const uint16_t count = loadU16(m_bytes.data() + m_offset);
    if (count > maxBytes) return fail(WireError::BadString);
    if (remaining() - 2 < count) return fail(WireError::Truncated);
    const std::string_view text(reinterpret_cast<const char*>(m_bytes.data() + m_offset + 2), count);
    if (!validUtf8(text)) return fail(WireError::BadString);
    m_offset += 2 + count;
    out = text;
    return true;
}

bool WireReader::bytes(std::span<const uint8_t>& out, uint16_t maxBytes) {
    if (remaining() < 2) return fail(WireError::Truncated);
    const uint16_t count = loadU16(m_bytes.data() + m_offset);
    if (count > maxBytes) return fail(WireError::TooLong);
    if (remaining() - 2 < count) return fail(WireError::Truncated);
    out = m_bytes.subspan(m_offset + 2, count);
    m_offset += 2 + count;
    return true;
}

}   // namespace shockwave::net
