#include <Network/Messages.h>

#include "MessageVisitors.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

namespace shockwave::net {

using detail::ReadFields;
using detail::statusAt;
using detail::WriteFields;

namespace {

constexpr Delivery R = Delivery::ReliableOrdered;
constexpr Delivery U = Delivery::Unreliable;
constexpr MessageDirection ToHost = MessageDirection::ClientToHost;
constexpr MessageDirection ToClient = MessageDirection::HostToClient;
constexpr MessageDirection Either = MessageDirection::Either;

constexpr MessageDescriptor kCatalog[] = {
    {MessageKind::Hello, "Hello", R, ToHost, "first message on a new connection"},
    {MessageKind::Challenge, "Challenge", R, ToClient, "answers a compatible Hello"},
    {MessageKind::AuthTicket, "AuthTicket", R, Either, "authenticating"},
    {MessageKind::Welcome, "Welcome", R, ToClient, "after mutual authentication"},
    {MessageKind::BaselineBegin, "BaselineBegin", R, ToClient, "starts a baseline"},
    {MessageKind::Spawn, "Spawn", R, ToClient, "baseline and active"},
    {MessageKind::Despawn, "Despawn", R, ToClient, "baseline and active"},
    {MessageKind::Ownership, "Ownership", R, ToClient, "baseline and active"},
    {MessageKind::InitialState, "InitialState", R, ToClient, "baseline or resync"},
    {MessageKind::BaselineEnd, "BaselineEnd", R, ToClient, "ends a baseline"},
    {MessageKind::Ready, "Ready", R, ToHost, "acknowledges a baseline"},
    {MessageKind::Active, "Active", R, ToClient, "starts play"},
    {MessageKind::Inputs, "Inputs", U, ToHost, "active"},
    {MessageKind::Snapshot, "Snapshot", U, ToClient, "active"},
    {MessageKind::Command, "Command", R, ToHost, "active"},
    {MessageKind::GameEvent, "GameEvent", R, ToClient, "active"},
    {MessageKind::Ping, "Ping", U, Either, "after Challenge"},
    {MessageKind::Pong, "Pong", U, Either, "answers a Ping"},
    {MessageKind::Reject, "Reject", R, ToClient, "any time; the host closes after it"},
    {MessageKind::Goodbye, "Goodbye", R, Either, "any time; the sender closes after it"},
    {MessageKind::ResyncRequest, "ResyncRequest", R, ToHost, "active"},
};
static_assert(std::size(kCatalog) == kLastMessageKind);

bool directionAllowed(MessageDirection direction, PeerRole receiver) {
    switch (direction) {
    case MessageDirection::ClientToHost: return receiver == PeerRole::Host;
    case MessageDirection::HostToClient: return receiver == PeerRole::Client;
    case MessageDirection::Either: return true;
    }
    return false;
}

template <size_t I = 0>
bool emplaceKind(MessageBody& body, uint8_t kind) {
    if constexpr (I < std::variant_size_v<MessageBody>) {
        if (kind == I + 1) {
            body.emplace<I>();
            return true;
        }
        return emplaceKind<I + 1>(body, kind);
    } else {
        return false;
    }
}

}   // namespace

std::span<const MessageDescriptor> messageCatalog() { return kCatalog; }

const MessageDescriptor* findMessage(uint8_t kind) {
    if (kind < 1 || kind > kLastMessageKind) return nullptr;
    return &kCatalog[kind - 1];
}

const char* wireStatusName(WireStatusCode code) {
    switch (code) {
    case WireStatusCode::Ok: return "ok";
    case WireStatusCode::Truncated: return "truncated";
    case WireStatusCode::BadMagic: return "bad magic";
    case WireStatusCode::BadVersion: return "unsupported protocol version";
    case WireStatusCode::UnknownKind: return "unknown message kind";
    case WireStatusCode::BadFlags: return "unsupported flags";
    case WireStatusCode::LengthMismatch: return "payload length mismatch";
    case WireStatusCode::TooLarge: return "message too large";
    case WireStatusCode::WrongDelivery: return "wrong delivery";
    case WireStatusCode::WrongDirection: return "wrong direction";
    case WireStatusCode::OutOfRange: return "value out of range";
    case WireStatusCode::NonFinite: return "non-finite float";
    case WireStatusCode::BadString: return "invalid string";
    case WireStatusCode::TrailingBytes: return "trailing bytes";
    case WireStatusCode::Invalid: return "invalid field combination";
    case WireStatusCode::Overflow: return "output buffer too small";
    }
    return "unknown";
}

ReasonCode WireStatus::reason() const {
    if (code == WireStatusCode::Ok) return ReasonCode::Normal;
    if (code == WireStatusCode::BadVersion || code == WireStatusCode::UnknownKind) return ReasonCode::ProtocolMismatch;
    return ReasonCode::MalformedPacket;
}

// Cross-field rules

const char* InputCommand::invalid() const {
    if (int(moveX) * moveX + int(moveZ) * moveZ > 128 * 128) return "move_x";
    return nullptr;
}

const char* Challenge::invalid() const {
    if (std::find(kSnapshotRates.begin(), kSnapshotRates.end(), snapshotRate) == kSnapshotRates.end())
        return "snapshot_rate";
    if (tickRate % snapshotRate != 0) return "snapshot_rate";
    return nullptr;
}

const char* Spawn::invalid() const {
    if (prefabId == 0 && authoredKey.empty()) return "authored_key";
    if (prefabId != 0 && !authoredKey.empty()) return "authored_key";
    return nullptr;
}

const char* Snapshot::invalid() const { return partIndex < partCount ? nullptr : "part_index"; }

const char* Pong::invalid() const { return responderSendUs < responderReceiveUs ? "responder_send_us" : nullptr; }

const char* PoseState::invalid() const {
    const double length = std::sqrt(
        double(rotation.x) * rotation.x + double(rotation.y) * rotation.y + double(rotation.z) * rotation.z +
        double(rotation.w) * rotation.w);
    return length >= 0.99 && length <= 1.01 ? nullptr : "rotation";
}

NetQuat PoseState::normalizedRotation() const {
    const float length = std::sqrt(
        rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w);
    if (!(length > 0.0f)) return {};
    return {rotation.x / length, rotation.y / length, rotation.z / length, rotation.w / length};
}

// Aim

uint16_t encodeYaw(float radians) {
    constexpr double twoPi = 2.0 * std::numbers::pi;
    if (!std::isfinite(radians)) return 32768;   // straight ahead
    double turns = (double(radians) + std::numbers::pi) / twoPi;
    turns -= std::floor(turns);
    return uint16_t(uint32_t(std::lround(turns * 65536.0)) & 0xFFFF);
}

float decodeYaw(uint16_t yaw) { return float(-std::numbers::pi + yaw * (2.0 * std::numbers::pi / 65536.0)); }

namespace {
constexpr double kMaxPitch = 89.0 * std::numbers::pi / 180.0;
}

int16_t encodePitch(float radians) {
    if (!std::isfinite(radians)) return 0;
    const double clamped = std::clamp(double(radians), -kMaxPitch, kMaxPitch);
    return int16_t(std::lround(clamped / kMaxPitch * 32767.0));
}

float decodePitch(int16_t pitch) { return float(std::clamp(int(pitch), -32767, 32767) / 32767.0 * kMaxPitch); }

// Messages

WireStatus decodeHeader(std::span<const uint8_t> bytes, MessageHeader& out) {
    if (bytes.size() < kHeaderBytes) return statusAt(WireStatusCode::Truncated, bytes.size(), "header");
    if (std::memcmp(bytes.data(), kMagic.data(), 4) != 0) return statusAt(WireStatusCode::BadMagic, 0, "magic");
    if (loadU16(bytes.data() + 4) != kProtocolVersion) return statusAt(WireStatusCode::BadVersion, 4, "version");
    const MessageDescriptor* descriptor = findMessage(bytes[6]);
    if (!descriptor) return statusAt(WireStatusCode::UnknownKind, 6, "kind");
    if (bytes[7] != 0) return statusAt(WireStatusCode::BadFlags, 7, "flags");
    const uint32_t payload = loadU32(bytes.data() + 28);
    if (payload != bytes.size() - kHeaderBytes) return statusAt(WireStatusCode::LengthMismatch, 28, "payload_bytes");
    if (bytes.size() > maxMessageBytes(descriptor->delivery))
        return statusAt(WireStatusCode::TooLarge, 28, "payload_bytes");
    out.kind = descriptor->kind;
    out.flags = 0;
    std::memcpy(out.session.data(), bytes.data() + 8, out.session.size());
    out.senderTick = loadU32(bytes.data() + 24);
    out.payloadBytes = payload;
    return {};
}

WireStatus decodeMessage(std::span<const uint8_t> bytes, const DecodeOptions& options, DecodedMessage& out) {
    MessageHeader header;
    if (WireStatus status = decodeHeader(bytes, header); !status) return status;
    const MessageDescriptor& descriptor = kCatalog[uint8_t(header.kind) - 1];
    if (options.checkDelivery && options.delivery != descriptor.delivery)
        return statusAt(WireStatusCode::WrongDelivery, 6, "kind");
    if (options.checkDirection && !directionAllowed(descriptor.direction, options.receiver))
        return statusAt(WireStatusCode::WrongDirection, 6, "kind");

    DecodedMessage decoded;
    decoded.header = header;
    emplaceKind(decoded.body, uint8_t(header.kind));
    WireStatus status;
    std::visit(
        [&](auto& message) {
            ReadFields read(bytes.subspan(kHeaderBytes));
            std::decay_t<decltype(message)>::fields(message, read);
            status = read.status();
            if (status) {
                if (read.reader().remaining() != 0)
                    status = statusAt(WireStatusCode::TrailingBytes, uint32_t(read.reader().offset()), "payload");
                else if (const char* bad = message.invalid())
                    status = statusAt(WireStatusCode::Invalid, 0, bad);
            }
        },
        decoded.body);
    if (!status) {
        status.offset += kHeaderBytes;
        return status;
    }
    out = decoded;
    return {};
}

WireStatus encodeMessage(
    const MessageBody& body, const Nonce& session, uint32_t senderTick, std::span<uint8_t> out, uint32_t& written) {
    written = 0;
    const MessageDescriptor& descriptor = kCatalog[body.index()];
    WireWriter writer(out.first(std::min<size_t>(out.size(), maxMessageBytes(descriptor.delivery))));
    writer.raw(kMagic);
    writer.u16(kProtocolVersion);
    writer.u8(uint8_t(descriptor.kind));
    writer.u8(0);
    writer.raw(session);
    writer.u32(senderTick);
    writer.u32(0);   // payload length, filled in below
    if (!writer.ok()) {
        const bool roomy = out.size() >= maxMessageBytes(descriptor.delivery);
        return statusAt(roomy ? WireStatusCode::TooLarge : WireStatusCode::Overflow, writer.size(), "header");
    }
    WireStatus status;
    std::visit(
        [&](const auto& message) {
            WriteFields write(writer);
            std::decay_t<decltype(message)>::fields(message, write);
            status = write.status();
            if (status)
                if (const char* bad = message.invalid()) status = statusAt(WireStatusCode::Invalid, 0, bad);
        },
        body);
    if (!status) {
        // Out of room within the delivery's limit means the message is too big to send.
        if (status.code == WireStatusCode::Overflow && out.size() >= maxMessageBytes(descriptor.delivery))
            status.code = WireStatusCode::TooLarge;
        return status;
    }
    writer.patchU32(28, uint32_t(writer.size() - kHeaderBytes));
    written = uint32_t(writer.size());
    return {};
}

// State records

bool writeStateRecord(WireWriter& writer, const StateRecord& record) {
    if (record.entity == 0 || record.profileId == 0 || record.profileId > 1023 || record.state.size() > kMaxStateBytes)
        return false;
    return writer.u64(record.entity) && writer.u32(record.incarnation) && writer.u16(record.profileId) &&
           writer.u16(uint16_t(record.state.size())) && writer.raw(record.state);
}

bool StateRecordReader::next(StateRecord& out) {
    if (m_left == 0) return false;
    uint16_t bytes;
    StateRecord record;
    if (!m_reader.u64(record.entity) || !m_reader.u32(record.incarnation) || !m_reader.u16(record.profileId) ||
        !m_reader.u16(bytes) || !m_reader.view(bytes, record.state)) {
        m_left = 0;
        return false;
    }
    --m_left;
    out = record;
    return true;
}

// Pose

WireStatus encodePose(const PoseState& pose, std::span<uint8_t> out, uint32_t& written) {
    written = 0;
    WireWriter writer(out);
    WriteFields write(writer);
    PoseState::fields(pose, write);
    WireStatus status = write.status();
    if (status)
        if (const char* bad = pose.invalid()) status = statusAt(WireStatusCode::Invalid, 0, bad);
    if (!status) return status;
    written = uint32_t(writer.size());
    return {};
}

WireStatus decodePose(std::span<const uint8_t> bytes, PoseState& out) {
    if (bytes.size() > kPoseStateBytes) return statusAt(WireStatusCode::TrailingBytes, kPoseStateBytes, "pose");
    PoseState pose;
    ReadFields read(bytes);
    PoseState::fields(pose, read);
    WireStatus status = read.status();
    if (status)
        if (const char* bad = pose.invalid()) status = statusAt(WireStatusCode::Invalid, 0, bad);
    if (!status) return status;
    out = pose;
    return {};
}

// Reference

namespace {

// Lists fields as Markdown table rows, with byte offsets while the layout is fixed.
class SchemaFields {
public:
    std::string rows;
    size_t offset = 0;
    bool fixed = true;

    void u8(const char* name, uint8_t, uint8_t lo = 0, uint8_t hi = 0xFF) {
        row(name, "u8", 1, range<uint8_t>(lo, hi, 0xFF));
    }
    void u16(const char* name, uint16_t, uint16_t lo = 0, uint16_t hi = 0xFFFF) {
        row(name, "u16", 2, range<uint16_t>(lo, hi, 0xFFFF));
    }
    void u32(const char* name, uint32_t, uint32_t lo = 0, uint32_t hi = 0xFFFFFFFFu) {
        row(name, "u32", 4, range<uint32_t>(lo, hi, 0xFFFFFFFFu));
    }
    void u64(const char* name, uint64_t, uint64_t lo = 0, uint64_t hi = ~uint64_t(0)) {
        row(name, "u64", 8, range(lo, hi, ~uint64_t(0)));
    }
    void i8(const char* name, int8_t, int8_t lo, int8_t hi) { row(name, "i8", 1, std::format("{}..{}", lo, hi)); }
    void i16(const char* name, int16_t, int16_t lo, int16_t hi) { row(name, "i16", 2, std::format("{}..{}", lo, hi)); }
    void bits8(const char* name, uint8_t, uint8_t mask) { row(name, "u8 bits", 1, std::format("only 0x{:02X}", mask)); }
    void bits16(const char* name, uint16_t, uint16_t mask) {
        row(name, "u16 bits", 2, std::format("only 0x{:04X}", mask));
    }
    void reason(const char* name, ReasonCode) { row(name, "u16 reason", 2, std::format("0..{}", kLastReasonCode)); }
    void f32(const char* name, float, float absMax) { row(name, "f32", 4, std::format("finite, abs <= {}", absMax)); }
    void vec3(const char* name, NetVec3, float absMax) {
        row(name, "vec3 (3 x f32)", 12, std::format("finite, abs <= {}", absMax));
    }
    void quat(const char* name, NetQuat) { row(name, "quat (4 x f32, xyzw)", 16, "length 0.99..1.01"); }
    void nonce(const char* name, const Nonce&) { row(name, "16 bytes", 16, ""); }
    void digest(const char* name, const Digest&) { row(name, "digest (32 bytes, SHA-256)", 32, ""); }
    void string(const char* name, std::string_view, uint16_t minBytes, uint16_t maxBytes) {
        row(name, std::format("string({})", maxBytes), 0,
            std::format("{}..{} bytes UTF-8, no NUL", minBytes, maxBytes));
    }
    void bytes(const char* name, std::span<const uint8_t>, uint16_t minBytes, uint16_t maxBytes) {
        row(name, std::format("bytes({})", maxBytes), 0, std::format("{}..{} bytes", minBytes, maxBytes));
    }
    template <size_t N>
    void inputs(const char* name, uint8_t, const std::array<InputCommand, N>&) {
        row(name, "u8 count, then InputCommand x count", 0, std::format("1..{} commands", N));
    }
    void records(const char* name, uint16_t, std::span<const uint8_t>) {
        row(name, "u16 count, then state records", 0, "each record self-contained");
    }

private:
    template <class T>
    static std::string range(T lo, T hi, T max) {
        if (lo == 0 && hi == max) return "";
        if (hi == max) return std::format(">= {}", lo);
        return std::format("{}..{}", lo, hi);
    }
    void row(const char* name, std::string type, size_t size, std::string rule) {
        const std::string at = fixed ? std::to_string(offset) : "";
        rows += std::format("| {} | `{}` | {} | {} |\n", at, name, type, rule);
        if (size == 0) fixed = false;
        offset += size;
    }
};

template <class M>
std::string fieldTable(size_t startOffset = 0) {
    SchemaFields schema;
    schema.offset = startOffset;
    const M message{};
    M::fields(message, schema);
    return "| Offset | Field | Type | Allowed |\n|---|---|---|---|\n" + schema.rows;
}

const char* directionText(MessageDirection direction) {
    switch (direction) {
    case MessageDirection::ClientToHost: return "client to host";
    case MessageDirection::HostToClient: return "host to client";
    case MessageDirection::Either: return "either way";
    }
    return "";
}

}   // namespace

std::string protocolReferenceMarkdown() {
    std::string md;
    md += "# Network protocol reference\n\n";
    md += "Generated from `Source/Runtime/Network/Public/Network/Messages.h`; don't edit it by hand. ";
    md += "Regenerate it with `build/bin/network_wire_test --write-reference docs/NetworkProtocol.md`.\n\n";
    md += std::format(
        "Protocol version {}. Every integer is little endian, every float is IEEE 754 single ", kProtocolVersion);
    md += "precision written as its bits, and every length counts bytes. A `string(N)` or `bytes(N)` is a u16 byte ";
    md += "count followed by that many bytes. Offsets are from the start of the payload, and stop once a field has a ";
    md += "variable length. No message may have bytes after its last field.\n\n";
    md += std::format(
        "Reliable messages are at most {} bytes and unreliable ones {}, header included. ", kMaxReliableMessageBytes,
        kMaxUnreliableMessageBytes);
    md += "Each kind's delivery and direction are fixed: a message that arrives another way is rejected. ";
    md += "An unknown version or kind closes the connection with ProtocolMismatch; any other bad message with ";
    md += "MalformedPacket.\n\n";

    md += "## Header (32 bytes)\n\n";
    md += "| Offset | Field | Type | Allowed |\n|---|---|---|---|\n";
    md += "| 0 | `magic` | 4 bytes | ASCII `SWMP` |\n";
    md += std::format("| 4 | `version` | u16 | {} |\n", kProtocolVersion);
    md += std::format("| 6 | `kind` | u8 | 1..{}, below |\n", kLastMessageKind);
    md += "| 7 | `flags` | u8 | 0 |\n";
    md += "| 8 | `session` | 16 bytes | the host's session nonce; zero only in a developer direct-join Hello |\n";
    md += "| 24 | `sender_tick` | u32 | 0 before play |\n";
    md += "| 28 | `payload_bytes` | u32 | message size minus 32 |\n\n";

    md += "## Messages\n\n";
    md += "| Kind | Name | Delivery | Direction | When |\n|---|---|---|---|---|\n";
    for (const MessageDescriptor& m : kCatalog)
        md += std::format(
            "| {} | {} | {} | {} | {} |\n", int(m.kind), m.name, deliveryName(m.delivery), directionText(m.direction),
            m.when);
    md += "\n";

    auto section = [&](const MessageDescriptor& m, std::string table, const char* note) {
        md += std::format("### {} {}\n\n", int(m.kind), m.name);
        if (*note) md += std::string(note) + "\n\n";
        md += table + "\n";
    };
    const auto& c = kCatalog;
    section(c[0], fieldTable<Hello>(), "");
    section(c[1], fieldTable<Challenge>(), "`snapshot_rate` must be 10, 15, 20, 30 or 60, and divide `tick_rate`.");
    section(
        c[2], fieldTable<AuthTicket>(),
        "The sender's identity comes from the transport, never from this message. "
        "Captures never store these bytes.");
    section(c[3], fieldTable<Welcome>(), "");
    section(c[4], fieldTable<BaselineBegin>(), "");
    section(
        c[5], fieldTable<Spawn>(),
        "An authored object (`prefab_id` 0) has an `authored_key`; a spawned prefab "
        "has an empty one.");
    section(c[6], fieldTable<Despawn>(), "");
    section(c[7], fieldTable<Ownership>(), "");
    section(c[8], fieldTable<InitialState>(), "");
    section(c[9], fieldTable<BaselineEnd>(), "");
    section(c[10], fieldTable<Ready>(), "");
    section(c[11], fieldTable<Active>(), "");
    section(c[12], fieldTable<Inputs>(), "");
    section(c[13], fieldTable<Snapshot>(), "`part_index` must be less than `part_count`.");
    section(c[14], fieldTable<Command>(), "");
    section(c[15], fieldTable<GameEvent>(), "");
    section(c[16], fieldTable<Ping>(), "");
    section(c[17], fieldTable<Pong>(), "`responder_send_us` can't be before `responder_receive_us`.");
    section(c[18], fieldTable<Reject>(), "");
    section(c[19], fieldTable<Goodbye>(), "");
    section(c[20], fieldTable<ResyncRequest>(), "");

    md += std::format("## Input command ({} bytes)\n\n", kInputCommandBytes);
    md += "`yaw` 0..65535 maps to -pi..pi (exclusive), `pitch` -32767..32767 to -89..89 degrees. ";
    md += "`move_x` and `move_z` together may be no longer than 128.\n\n";
    md += fieldTable<InputCommand>() + "\n";

    md += std::format("## State record ({}-byte header)\n\n", kStateRecordHeaderBytes);
    md += "| Offset | Field | Type | Allowed |\n|---|---|---|---|\n";
    md += "| 0 | `entity` | u64 | >= 1 |\n| 8 | `incarnation` | u32 |  |\n";
    md += "| 12 | `profile_id` | u16 | 1..1023 |\n";
    md += std::format(
        "| 14 | `state_bytes` | u16 | 0..{} |\n| 16 | `state` | state_bytes bytes |  |\n\n", kMaxStateBytes);
    md += "Profiles 1..31 belong to the engine and 32..1023 to games.\n\n";

    md += std::format("### Profiles 1 engine.pose.v1 and 2 engine.rigidbody.v1 ({} bytes)\n\n", kPoseStateBytes);
    md += fieldTable<PoseState>() + "\n";

    md += "## Reason codes\n\n| Code | Name |\n|---|---|\n";
    for (uint16_t code = 0; code <= kLastReasonCode; ++code)
        md += std::format("| {} | {} |\n", code, reasonName(ReasonCode(code)));
    return md;
}

}   // namespace shockwave::net
