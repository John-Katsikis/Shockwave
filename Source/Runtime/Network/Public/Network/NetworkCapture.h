#pragma once

// Packet captures (.swnetcap): a record of application messages a peer sent and
// received, for offline inspection. Recording is always an explicit user action, and
// nothing here can send traffic.
//
// Format version 1, little endian:
//
//     file:    "SWNC", u16 version = 1, u16 reserved = 0, then records
//     record:  u64 microseconds since the capture started, u8 direction (0 received,
//              1 sent), u8 delivery (0 reliable, 1 unreliable), u16 flags, u64 opaque
//              peer, u32 wire_length, then wire_length bytes of the message
//
// Flag bit 0 marks a redacted record, whose bytes were never stored (wire_length is 0):
// AuthTicket messages are always redacted. No other flag bits exist. A record holds at
// most 16384 bytes, and a writer stops at a file size cap (64 MiB by default).
//
// captureToJson() decodes every record with the message catalog (Messages.h);
// captureFromJson() encodes that JSON back to the identical capture. A record that
// doesn't decode is kept as raw hex, so it survives the round trip too.

#include <Network/NetworkTypes.h>
#include <Network/Transport.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave::net {

inline constexpr std::array<uint8_t, 4> kCaptureMagic = {'S', 'W', 'N', 'C'};
inline constexpr uint16_t kCaptureVersion = 1;
inline constexpr uint32_t kCaptureFileHeaderBytes = 8;
inline constexpr uint32_t kCaptureRecordHeaderBytes = 24;
inline constexpr uint32_t kMaxCaptureRecordBytes = 16384;
inline constexpr uint64_t kMaxCaptureFileBytes = 64ull << 20;
inline constexpr uint16_t kCaptureRedacted = 1;

enum class CaptureDirection : uint8_t { Received = 0, Sent = 1 };

struct CaptureRecord {
    uint64_t timeUs = 0;
    CaptureDirection direction = CaptureDirection::Received;
    Delivery delivery = Delivery::ReliableOrdered;
    uint16_t flags = 0;
    uint64_t peer = 0;
    std::span<const uint8_t> wire;   // empty when redacted
    bool redacted() const { return flags & kCaptureRedacted; }
};

// True for bytes that must never be stored: an AuthTicket message, or anything too
// short to tell.
bool captureMustRedact(std::span<const uint8_t> wire);

// In-memory form, for tests and tools.
void appendCaptureHeader(std::vector<uint8_t>& out);
// Redacts by itself when captureMustRedact() says so. False if the record is too big.
bool appendCaptureRecord(std::vector<uint8_t>& out, const CaptureRecord& record);

enum class CaptureWrite : uint8_t { Written, Redacted, Full, Failed };

// Writes a capture file as messages happen. After open(), writes don't allocate.
class CaptureWriter {
public:
    CaptureWriter() = default;
    ~CaptureWriter() { close(); }
    CaptureWriter(const CaptureWriter&) = delete;
    CaptureWriter& operator=(const CaptureWriter&) = delete;

    NetResult<void> open(const std::filesystem::path& path, uint64_t maxFileBytes = kMaxCaptureFileBytes);
    // Full once the next record would pass the size cap: that and every later record is
    // dropped, and the first time it says so in the log.
    CaptureWrite write(const CaptureRecord& record);
    void close();

    bool isOpen() const { return m_file.is_open(); }
    bool full() const { return m_full; }
    uint64_t bytesWritten() const { return m_bytes; }
    uint64_t recordsWritten() const { return m_records; }

private:
    std::ofstream m_file;
    uint64_t m_max = 0;
    uint64_t m_bytes = 0;
    uint64_t m_records = 0;
    bool m_full = false;
};

struct CaptureError {
    uint64_t offset = 0;
    const char* message = "";
};

// Walks a capture in memory without copying it.
class CaptureReader {
public:
    explicit CaptureReader(std::span<const uint8_t> file);
    // False at the end, or when the rest is malformed (failed() then says so).
    bool next(CaptureRecord& out);
    bool failed() const { return m_failed; }
    const CaptureError& error() const { return m_error; }

private:
    bool fail(uint64_t offset, const char* message);

    std::span<const uint8_t> m_file;
    size_t m_at = 0;
    bool m_failed = false;
    CaptureError m_error;
};

// Decoded JSON. Peer IDs and times are decimal strings, so 64-bit values survive JSON
// readers that use doubles.
bool captureToJson(std::span<const uint8_t> file, std::string& json, std::string& error);
// The reverse. Refuses records that would need secret bytes it doesn't have (an
// AuthTicket given as fields).
bool captureFromJson(std::string_view json, std::vector<uint8_t>& file, std::string& error);

}   // namespace shockwave::net
