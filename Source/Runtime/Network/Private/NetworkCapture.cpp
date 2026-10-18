#include <Network/NetworkCapture.h>

#include "Json.h"
#include "MessageVisitors.h"

#include <Network/Messages.h>
#include <Network/Wire.h>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>

namespace shockwave::net {

bool captureMustRedact(std::span<const uint8_t> wire) {
    if (wire.empty()) return false;
    if (wire.size() < 7) return true;
    return std::memcmp(wire.data(), kMagic.data(), 4) == 0 && wire[6] == uint8_t(MessageKind::AuthTicket);
}

namespace {

void writeRecordHeader(uint8_t* out, const CaptureRecord& record, uint16_t flags, uint32_t length) {
    storeU64(out, record.timeUs);
    out[8] = uint8_t(record.direction);
    out[9] = uint8_t(record.delivery == Delivery::Unreliable ? 1 : 0);
    storeU16(out + 10, flags);
    storeU64(out + 12, record.peer);
    storeU32(out + 20, length);
}

// What a record looks like once redaction has been applied.
bool prepare(const CaptureRecord& record, uint16_t& flags, std::span<const uint8_t>& wire) {
    if (record.flags & ~kCaptureRedacted) return false;
    flags = record.flags;
    wire = record.wire;
    if (record.redacted() || captureMustRedact(record.wire)) {
        flags = kCaptureRedacted;
        wire = {};
    }
    return wire.size() <= kMaxCaptureRecordBytes;
}

}   // namespace

void appendCaptureHeader(std::vector<uint8_t>& out) {
    const uint8_t header[kCaptureFileHeaderBytes] = {'S', 'W', 'N', 'C', uint8_t(kCaptureVersion), 0, 0, 0};
    out.insert(out.end(), header, header + sizeof(header));
}

bool appendCaptureRecord(std::vector<uint8_t>& out, const CaptureRecord& record) {
    uint16_t flags;
    std::span<const uint8_t> wire;
    if (!prepare(record, flags, wire)) return false;
    uint8_t header[kCaptureRecordHeaderBytes];
    writeRecordHeader(header, record, flags, uint32_t(wire.size()));
    out.insert(out.end(), header, header + sizeof(header));
    out.insert(out.end(), wire.begin(), wire.end());
    return true;
}

// Writer

NetResult<void> CaptureWriter::open(const std::filesystem::path& path, uint64_t maxFileBytes) {
    close();
    if (maxFileBytes < kCaptureFileHeaderBytes)
        return NetError{NetErrorCode::InvalidArgument, "capture size cap too small"};
    m_file.open(path, std::ios::binary | std::ios::trunc);
    if (!m_file) return NetError{NetErrorCode::Io, "couldn't create the capture file"};
    std::vector<uint8_t> header;
    appendCaptureHeader(header);
    m_file.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    if (!m_file) {
        close();
        return NetError{NetErrorCode::Io, "couldn't write the capture file"};
    }
    m_max = maxFileBytes;
    m_bytes = header.size();
    m_records = 0;
    m_full = false;
    return {};
}

CaptureWrite CaptureWriter::write(const CaptureRecord& record) {
    if (!m_file.is_open()) return CaptureWrite::Failed;
    if (m_full) return CaptureWrite::Full;
    uint16_t flags;
    std::span<const uint8_t> wire;
    if (!prepare(record, flags, wire)) return CaptureWrite::Failed;
    const uint64_t size = kCaptureRecordHeaderBytes + wire.size();
    if (m_bytes + size > m_max) {
        m_full = true;
        SW_LOG(LogNetwork, Warning, "Packet capture reached its {} byte limit; later messages aren't recorded", m_max);
        return CaptureWrite::Full;
    }
    uint8_t header[kCaptureRecordHeaderBytes];
    writeRecordHeader(header, record, flags, uint32_t(wire.size()));
    m_file.write(reinterpret_cast<const char*>(header), sizeof(header));
    if (!wire.empty()) m_file.write(reinterpret_cast<const char*>(wire.data()), std::streamsize(wire.size()));
    if (!m_file) return CaptureWrite::Failed;
    m_bytes += size;
    ++m_records;
    return flags & kCaptureRedacted ? CaptureWrite::Redacted : CaptureWrite::Written;
}

void CaptureWriter::close() {
    if (m_file.is_open()) m_file.close();
}

// Reader

CaptureReader::CaptureReader(std::span<const uint8_t> file) : m_file(file) {
    if (file.size() < kCaptureFileHeaderBytes) {
        fail(0, "too short for a capture header");
    } else if (std::memcmp(file.data(), kCaptureMagic.data(), 4) != 0) {
        fail(0, "not a capture file (no SWNC magic)");
    } else if (loadU16(file.data() + 4) != kCaptureVersion) {
        fail(4, "unsupported capture version");
    } else if (loadU16(file.data() + 6) != 0) {
        fail(6, "reserved header field isn't zero");
    } else {
        m_at = kCaptureFileHeaderBytes;
    }
}

bool CaptureReader::fail(uint64_t offset, const char* message) {
    m_failed = true;
    m_error = {offset, message};
    return false;
}

bool CaptureReader::next(CaptureRecord& out) {
    if (m_failed || m_at >= m_file.size()) return false;
    if (m_file.size() - m_at < kCaptureRecordHeaderBytes) return fail(m_at, "truncated record header");
    const uint8_t* in = m_file.data() + m_at;
    CaptureRecord record;
    record.timeUs = loadU64(in);
    if (in[8] > 1) return fail(m_at + 8, "bad direction");
    record.direction = CaptureDirection(in[8]);
    if (in[9] > 1) return fail(m_at + 9, "bad delivery");
    record.delivery = in[9] ? Delivery::Unreliable : Delivery::ReliableOrdered;
    record.flags = loadU16(in + 10);
    if (record.flags & ~kCaptureRedacted) return fail(m_at + 10, "unknown record flags");
    record.peer = loadU64(in + 12);
    const uint32_t length = loadU32(in + 20);
    if (length > kMaxCaptureRecordBytes) return fail(m_at + 20, "record longer than 16384 bytes");
    if (record.redacted() && length != 0) return fail(m_at + 20, "redacted record holds bytes");
    if (m_file.size() - m_at - kCaptureRecordHeaderBytes < length) return fail(m_at + 20, "truncated record");
    record.wire = m_file.subspan(m_at + kCaptureRecordHeaderBytes, length);
    m_at += kCaptureRecordHeaderBytes + length;
    out = record;
    return true;
}

// JSON

namespace {

std::string hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

bool unhex(std::string_view text, std::vector<uint8_t>& out) {
    if (text.size() % 2) return false;
    out.clear();
    out.reserve(text.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

std::string floatText(float value) {
    // Nine significant digits always read back as the same float.
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", double(value));
    return buffer;
}

// Writes a message's fields as JSON members.
class DumpFields {
public:
    std::string out;

    void u8(const char* name, uint8_t value, uint8_t = 0, uint8_t = 0xFF) { member(name, std::to_string(value)); }
    void u16(const char* name, uint16_t value, uint16_t = 0, uint16_t = 0xFFFF) { member(name, std::to_string(value)); }
    void u32(const char* name, uint32_t value, uint32_t = 0, uint32_t = 0xFFFFFFFFu) {
        member(name, std::to_string(value));
    }
    void u64(const char* name, uint64_t value, uint64_t = 0, uint64_t = ~uint64_t(0)) {
        member(name, "\"" + std::to_string(value) + "\"");
    }
    void i8(const char* name, int8_t value, int8_t, int8_t) { member(name, std::to_string(value)); }
    void i16(const char* name, int16_t value, int16_t, int16_t) { member(name, std::to_string(value)); }
    void bits8(const char* name, uint8_t value, uint8_t) { member(name, std::to_string(value)); }
    void bits16(const char* name, uint16_t value, uint16_t) { member(name, std::to_string(value)); }
    void reason(const char* name, ReasonCode value) {
        member(name, std::to_string(uint16_t(value)));
        if (const char* text = reasonName(value)) member((std::string(name) + "_name").c_str(), quote(text));
    }
    void f32(const char* name, float value, float) { member(name, floatText(value)); }
    void vec3(const char* name, const NetVec3& v, float) {
        member(name, "[" + floatText(v.x) + ", " + floatText(v.y) + ", " + floatText(v.z) + "]");
    }
    void quat(const char* name, const NetQuat& q) {
        member(
            name, "[" + floatText(q.x) + ", " + floatText(q.y) + ", " + floatText(q.z) + ", " + floatText(q.w) + "]");
    }
    void nonce(const char* name, const Nonce& value) { member(name, "\"" + hex(value) + "\""); }
    void digest(const char* name, const Digest& value) { member(name, "\"" + hex(value) + "\""); }
    void string(const char* name, std::string_view value, uint16_t, uint16_t) { member(name, quote(value)); }
    void bytes(const char* name, std::span<const uint8_t> value, uint16_t, uint16_t) {
        member(name, "\"" + hex(value) + "\"");
    }
    template <size_t N>
    void inputs(const char* name, uint8_t count, const std::array<InputCommand, N>& commands) {
        std::string list = "[";
        for (uint8_t i = 0; i < count; ++i) {
            DumpFields item;
            InputCommand::fields(commands[i], item);
            list += (i ? ", {" : "{") + item.out + "}";
        }
        member(name, list + "]");
    }
    void records(const char* name, uint16_t count, std::span<const uint8_t> bytes) {
        std::string list = "[";
        StateRecordReader reader(bytes, count);
        StateRecord record;
        bool first = true;
        while (reader.next(record)) {
            list += first ? "{" : ", {";
            first = false;
            list += std::format(
                "\"entity\": \"{}\", \"incarnation\": {}, \"profile_id\": {}, \"state\": \"{}\"}}", record.entity,
                record.incarnation, record.profileId, hex(record.state));
        }
        member(name, list + "]");
    }

private:
    static std::string quote(std::string_view text) {
        std::string q;
        json::appendString(q, text);
        return q;
    }
    void member(const char* name, const std::string& value) {
        if (!out.empty()) out += ", ";
        json::appendString(out, name);
        out += ": " + value;
    }
};

// Reads a message's fields from a JSON object. Strings point into the JSON document;
// byte fields into buffers this owns, so it must outlive encoding.
class LoadFields {
public:
    explicit LoadFields(const json::Value& object) : m_object(object) {}

    bool ok() const { return m_error.empty(); }
    // Also fails on members the message doesn't have.
    bool finish() {
        if (ok() && m_used != m_object.keys.size()) m_error = "unexpected member in fields";
        return ok();
    }
    const std::string& error() const { return m_error; }

    template <class T>
    void u8(const char* name, T& v, uint8_t = 0, uint8_t = 0xFF) {
        integer(name, v);
    }
    template <class T>
    void u16(const char* name, T& v, uint16_t = 0, uint16_t = 0xFFFF) {
        integer(name, v);
    }
    template <class T>
    void u32(const char* name, T& v, uint32_t = 0, uint32_t = 0xFFFFFFFFu) {
        integer(name, v);
    }
    void u64(const char* name, uint64_t& v, uint64_t = 0, uint64_t = ~uint64_t(0)) {
        const json::Value* value = get(name, json::Value::String);
        if (value && !parseInt(value->text, v)) fail(name, "isn't a decimal 64-bit integer");
    }
    void i8(const char* name, int8_t& v, int8_t, int8_t) { integer(name, v); }
    void i16(const char* name, int16_t& v, int16_t, int16_t) { integer(name, v); }
    void bits8(const char* name, uint8_t& v, uint8_t) { integer(name, v); }
    void bits16(const char* name, uint16_t& v, uint16_t) { integer(name, v); }
    void reason(const char* name, ReasonCode& v) {
        uint16_t raw = 0;
        integer(name, raw);
        v = ReasonCode(raw);
        if (m_object.find(std::string(name) + "_name")) ++m_used;   // informational
    }
    void f32(const char* name, float& v, float) {
        if (const json::Value* value = get(name, json::Value::Number)) readFloat(name, *value, v);
    }
    void vec3(const char* name, NetVec3& v, float) {
        float* parts[] = {&v.x, &v.y, &v.z};
        floats(name, parts);
    }
    void quat(const char* name, NetQuat& q) {
        float* parts[] = {&q.x, &q.y, &q.z, &q.w};
        floats(name, parts);
    }
    template <size_t N>
    void fixed(const char* name, std::array<uint8_t, N>& v) {
        const json::Value* value = get(name, json::Value::String);
        std::vector<uint8_t> bytes;
        if (!value) return;
        if (!unhex(value->text, bytes) || bytes.size() != N) return fail(name, "isn't the right length of hex");
        std::memcpy(v.data(), bytes.data(), N);
    }
    void nonce(const char* name, Nonce& v) { fixed(name, v); }
    void digest(const char* name, Digest& v) { fixed(name, v); }
    void string(const char* name, std::string_view& v, uint16_t, uint16_t) {
        if (const json::Value* value = get(name, json::Value::String)) v = value->text;
    }
    void bytes(const char* name, std::span<const uint8_t>& v, uint16_t, uint16_t) {
        const json::Value* value = get(name, json::Value::String);
        if (!value) return;
        auto& storage = keep();
        if (!unhex(value->text, storage)) return fail(name, "isn't hex");
        v = storage;
    }
    template <size_t N>
    void inputs(const char* name, uint8_t& count, std::array<InputCommand, N>& commands) {
        const json::Value* value = get(name, json::Value::Array);
        if (!value) return;
        if (value->items.empty() || value->items.size() > N) return fail(name, "has the wrong number of commands");
        for (size_t i = 0; i < value->items.size() && ok(); ++i) {
            if (value->items[i].type != json::Value::Object) return fail(name, "holds a non-object");
            LoadFields item(value->items[i]);
            InputCommand::fields(commands[i], item);
            if (!item.finish()) m_error = std::string(name) + ": " + item.error();
        }
        count = uint8_t(value->items.size());
    }
    void records(const char* name, uint16_t& count, std::span<const uint8_t>& bytes) {
        const json::Value* value = get(name, json::Value::Array);
        if (!value) return;
        if (value->items.size() > 0xFFFF) return fail(name, "has too many records");
        auto& storage = keep();
        for (const json::Value& item : value->items) {
            if (item.type != json::Value::Object || item.keys.size() != 4) return fail(name, "holds a bad record");
            const json::Value* entity = item.find("entity");
            const json::Value* incarnation = item.find("incarnation");
            const json::Value* profile = item.find("profile_id");
            const json::Value* state = item.find("state");
            StateRecord record;
            std::vector<uint8_t> stateBytes;
            if (!entity || entity->type != json::Value::String || !parseInt(entity->text, record.entity) ||
                !incarnation || incarnation->type != json::Value::Number ||
                !parseInt(incarnation->text, record.incarnation) || !profile || profile->type != json::Value::Number ||
                !parseInt(profile->text, record.profileId) || !state || state->type != json::Value::String ||
                !unhex(state->text, stateBytes))
                return fail(name, "holds a bad record");
            record.state = stateBytes;
            std::vector<uint8_t> encoded(kStateRecordHeaderBytes + stateBytes.size());
            WireWriter writer(encoded);
            if (!writeStateRecord(writer, record)) return fail(name, "holds an out-of-range record");
            storage.insert(storage.end(), encoded.begin(), encoded.end());
        }
        count = uint16_t(value->items.size());
        bytes = storage;
    }

private:
    template <class T>
    static bool parseInt(const std::string& text, T& out) {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
        return ec == std::errc() && end == text.data() + text.size();
    }
    void fail(const char* name, const char* what) {
        if (ok()) m_error = std::format("field {} {}", name, what);
    }
    const json::Value* get(const char* name, json::Value::Type type) {
        if (!ok()) return nullptr;
        const json::Value* value = m_object.find(name);
        if (!value) {
            fail(name, "is missing");
            return nullptr;
        }
        ++m_used;
        if (value->type != type) {
            fail(name, "has the wrong type");
            return nullptr;
        }
        return value;
    }
    template <class T>
    void integer(const char* name, T& v) {
        const json::Value* value = get(name, json::Value::Number);
        if (value && !parseInt(value->text, v)) fail(name, "isn't an integer in range");
    }
    void readFloat(const char* name, const json::Value& value, float& v) {
        char* end = nullptr;
        const float parsed = std::strtof(value.text.c_str(), &end);
        if (end != value.text.c_str() + value.text.size()) return fail(name, "isn't a number");
        v = parsed;
    }
    template <size_t N>
    void floats(const char* name, float* (&parts)[N]) {
        const json::Value* value = get(name, json::Value::Array);
        if (!value) return;
        if (value->items.size() != N) return fail(name, "has the wrong number of components");
        for (size_t i = 0; i < N; ++i) {
            if (value->items[i].type != json::Value::Number) return fail(name, "holds a non-number");
            readFloat(name, value->items[i], *parts[i]);
        }
    }
    std::vector<uint8_t>& keep() { return *m_storage.emplace_back(std::make_unique<std::vector<uint8_t>>()); }

    const json::Value& m_object;
    size_t m_used = 0;
    std::string m_error;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> m_storage;
};

template <size_t I = 0>
bool emplaceNamed(MessageBody& body, std::string_view name) {
    if constexpr (I < std::variant_size_v<MessageBody>) {
        if (name == messageCatalog()[I].name) {
            body.emplace<I>();
            return true;
        }
        return emplaceNamed<I + 1>(body, name);
    } else {
        return false;
    }
}

}   // namespace

bool captureToJson(std::span<const uint8_t> file, std::string& json, std::string& error) {
    CaptureReader reader(file);
    std::string out = "{\n  \"format\": \"shockwave-network-capture\",\n  \"version\": 1,\n  \"records\": [";
    CaptureRecord record;
    bool first = true;
    while (reader.next(record)) {
        out += first ? "\n    {" : ",\n    {";
        first = false;
        out += std::format(
            "\"time_us\": \"{}\", \"direction\": \"{}\", \"delivery\": \"{}\", \"peer\": \"{}\"", record.timeUs,
            record.direction == CaptureDirection::Sent ? "sent" : "received", deliveryName(record.delivery),
            record.peer);
        if (record.redacted()) {
            out += ", \"redacted\": true}";
            continue;
        }
        DecodeOptions options;
        options.delivery = record.delivery;
        options.checkDirection = false;
        DecodedMessage message;
        const WireStatus status = decodeMessage(record.wire, options, message);
        if (!status) {
            out += std::format(
                ", \"error\": \"{} at byte {} ({})\", \"raw\": \"{}\"}}", wireStatusName(status.code), status.offset,
                status.field, hex(record.wire));
            continue;
        }
        out += std::format(
            ", \"message\": \"{}\", \"session\": \"{}\", \"sender_tick\": {}, \"fields\": {{",
            findMessage(uint8_t(message.header.kind))->name, hex(message.header.session), message.header.senderTick);
        std::visit(
            [&](const auto& body) {
                DumpFields dump;
                std::decay_t<decltype(body)>::fields(body, dump);
                out += dump.out;
            },
            message.body);
        out += "}}";
    }
    if (reader.failed()) {
        error = std::format("capture byte {}: {}", reader.error().offset, reader.error().message);
        return false;
    }
    out += first ? "]\n}\n" : "\n  ]\n}\n";
    json = std::move(out);
    return true;
}

bool captureFromJson(std::string_view text, std::vector<uint8_t>& file, std::string& error) {
    json::Value root;
    if (!json::parse(text, root, error)) return false;
    const json::Value* format = root.find("format");
    const json::Value* version = root.find("version");
    const json::Value* records = root.find("records");
    if (root.type != json::Value::Object || !format || format->text != "shockwave-network-capture" || !version ||
        version->text != "1" || !records || records->type != json::Value::Array) {
        error = "not a version 1 shockwave-network-capture document";
        return false;
    }
    std::vector<uint8_t> out;
    appendCaptureHeader(out);
    std::vector<uint8_t> wire(kMaxReliableMessageBytes);
    for (size_t i = 0; i < records->items.size(); ++i) {
        const json::Value& item = records->items[i];
        auto bad = [&](const std::string& why) {
            error = std::format("record {}: {}", i, why);
            return false;
        };
        if (item.type != json::Value::Object) return bad("isn't an object");
        const json::Value* time = item.find("time_us");
        const json::Value* direction = item.find("direction");
        const json::Value* delivery = item.find("delivery");
        const json::Value* peer = item.find("peer");
        CaptureRecord record;
        auto parse64 = [](const json::Value* v, uint64_t& out) {
            if (!v || v->type != json::Value::String) return false;
            const auto [end, ec] = std::from_chars(v->text.data(), v->text.data() + v->text.size(), out);
            return ec == std::errc() && end == v->text.data() + v->text.size();
        };
        if (!parse64(time, record.timeUs)) return bad("time_us must be a decimal string");
        if (!parse64(peer, record.peer)) return bad("peer must be a decimal string");
        if (!direction || (direction->text != "sent" && direction->text != "received"))
            return bad("direction must be sent or received");
        record.direction = direction->text == "sent" ? CaptureDirection::Sent : CaptureDirection::Received;
        if (!delivery || (delivery->text != "reliable" && delivery->text != "unreliable"))
            return bad("delivery must be reliable or unreliable");
        record.delivery = delivery->text == "unreliable" ? Delivery::Unreliable : Delivery::ReliableOrdered;

        // Only the members a dump writes for this kind of record.
        const bool isRedacted = item.find("redacted"), isRaw = item.find("raw");
        for (const std::string& key : item.keys) {
            static constexpr std::string_view common[] = {"time_us", "direction", "delivery", "peer"};
            static constexpr std::string_view decoded[] = {"message", "session", "sender_tick", "fields"};
            bool known = std::find(std::begin(common), std::end(common), key) != std::end(common);
            if (isRedacted)
                known |= key == "redacted";
            else if (isRaw)
                known |= key == "raw" || key == "error";
            else
                known |= std::find(std::begin(decoded), std::end(decoded), key) != std::end(decoded);
            if (!known) return bad("unexpected member " + key);
        }

        std::vector<uint8_t> raw;
        if (const json::Value* redacted = item.find("redacted")) {
            if (redacted->type != json::Value::Bool || !redacted->boolean || item.keys.size() != 5)
                return bad("a redacted record has only its header fields");
            record.flags = kCaptureRedacted;
        } else if (const json::Value* rawHex = item.find("raw")) {
            if (rawHex->type != json::Value::String || !unhex(rawHex->text, raw)) return bad("raw must be hex");
            if (captureMustRedact(raw)) return bad("raw bytes would hold an authentication ticket");
            record.wire = raw;
        } else {
            const json::Value* name = item.find("message");
            const json::Value* session = item.find("session");
            const json::Value* tick = item.find("sender_tick");
            const json::Value* fields = item.find("fields");
            if (!name || !session || !tick || !fields || fields->type != json::Value::Object || item.keys.size() != 8)
                return bad("needs message, session, sender_tick and fields");
            if (name->text == "AuthTicket") return bad("authentication tickets can't be encoded from a dump");
            DecodedMessage message;
            if (!emplaceNamed(message.body, name->text)) return bad("unknown message " + name->text);
            std::vector<uint8_t> nonceBytes;
            Nonce nonce{};
            if (!unhex(session->text, nonceBytes) || nonceBytes.size() != nonce.size())
                return bad("session must be 32 hex digits");
            std::memcpy(nonce.data(), nonceBytes.data(), nonce.size());
            uint32_t senderTick = 0;
            const auto [end, ec] =
                std::from_chars(tick->text.data(), tick->text.data() + tick->text.size(), senderTick);
            if (tick->type != json::Value::Number || ec != std::errc() || end != tick->text.data() + tick->text.size())
                return bad("sender_tick must be an unsigned 32-bit integer");
            LoadFields load(*fields);
            std::visit([&](auto& body) { std::decay_t<decltype(body)>::fields(body, load); }, message.body);
            if (!load.finish()) return bad(load.error());
            uint32_t written = 0;
            const WireStatus status = encodeMessage(message.body, nonce, senderTick, wire, written);
            if (!status) return bad(std::format("{} ({})", wireStatusName(status.code), status.field));
            raw.assign(wire.begin(), wire.begin() + written);
            record.wire = raw;
        }
        if (!appendCaptureRecord(out, record)) return bad("record is too large");
    }
    file = std::move(out);
    return true;
}

}   // namespace shockwave::net
