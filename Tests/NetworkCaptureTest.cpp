// Packet captures (Network/NetworkCapture.h): the binary format, dump to JSON and encode
// back to identical bytes, redaction of authentication tickets, malformed and truncated
// files, the file size cap, and that capturing and dumping never send anything.
#include <Network/Loopback.h>
#include <Network/Messages.h>
#include <Network/NetworkCapture.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace shockwave::net;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static const uint8_t kTicket[] = {0x5E, 0xC2, 0xE7, 0x71, 0xC4, 0xE7, 0x5E, 0xC2};   // must never reach a file
static constexpr uint64_t kSteam = 76561197960265729ull;   // over 2^53: JSON doubles would lose it

static std::vector<uint8_t> encoded(const MessageBody& body, uint32_t tick = 0) {
    Nonce session;
    for (int i = 0; i < 16; ++i) session[i] = uint8_t(0x30 + i);
    std::vector<uint8_t> out(kMaxReliableMessageBytes);
    uint32_t written = 0;
    const WireStatus status = encodeMessage(body, session, tick, out, written);
    CHECK(bool(status));
    out.resize(written);
    return out;
}

static bool contains(const std::vector<uint8_t>& haystack, std::span<const uint8_t> needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

// A capture with every interesting kind of record.
static std::vector<uint8_t> sampleCapture() {
    std::vector<uint8_t> pose(kPoseStateBytes);
    PoseState state;
    state.position = {0.1f, -1e-30f, 3.0e38f / 1e34f};
    state.rotation = {0, 0.70710677f, 0, 0.70710677f};
    uint32_t written = 0;
    CHECK(bool(encodePose(state, pose, written)));
    std::vector<uint8_t> records(256);
    WireWriter writer(records);
    CHECK(writeStateRecord(writer, {kSteam, 3, kProfilePose, pose}));
    CHECK(writeStateRecord(writer, {7, 0, 40, {}}));
    records.resize(writer.size());

    Hello hello;
    hello.gameProtocol = "caf\xC3\xA9 \"quoted\" \\ tab\t";
    hello.sceneId = 3;
    hello.lobbyId = kSteam;
    Inputs inputs;
    inputs.count = 3;
    inputs.commands[0] = {100, 1, -127, 0, 0, -32767, 3, 3, 98, 65535};
    inputs.commands[1] = {101, 1, 90, 90, 65535, 32767, 0, 0, 99, 0};
    inputs.commands[2] = {102, 1, 0, 0, 1, 0, 1, 0, 100, 1};

    std::vector<uint8_t> file;
    appendCaptureHeader(file);
    const std::vector<uint8_t> messages[] = {
        encoded(hello),
        encoded(AuthTicket{kTicket}),
        encoded(Snapshot{9, 1234, 2, 0, 1, 2, records}, 1234),
        encoded(inputs, 102),
        encoded(Reject{ReasonCode::ContentMismatch, "content differs"}),
        encoded(Goodbye{ReasonCode::Normal}, 4000000000u),
    };
    uint64_t time = 0;
    for (size_t i = 0; i < std::size(messages); ++i) {
        const MessageDescriptor* d = findMessage(messages[i][6]);
        CaptureRecord record;
        record.timeUs = time += 16667;
        record.direction = i % 2 ? CaptureDirection::Sent : CaptureDirection::Received;
        record.delivery = d->delivery;
        record.peer = kSteam + i;
        record.wire = messages[i];
        CHECK(appendCaptureRecord(file, record));
    }
    // Bytes that aren't a valid message survive as raw data.
    const uint8_t garbage[] = {'S', 'W', 'M', 'P', 1, 0, 99, 0, 1, 2, 3};
    CHECK(appendCaptureRecord(file, {time + 1, CaptureDirection::Received, Delivery::Unreliable, 0, 5, garbage}));
    return file;
}

static void testFormat() {
    const std::vector<uint8_t> file = sampleCapture();
    CHECK(std::memcmp(file.data(), "SWNC", 4) == 0);
    CHECK(file[4] == 1 && file[5] == 0 && file[6] == 0 && file[7] == 0);
    CHECK(!contains(file, kTicket));

    CaptureReader reader(file);
    CaptureRecord record;
    int count = 0, redacted = 0;
    while (reader.next(record)) {
        ++count;
        if (record.redacted()) {
            ++redacted;
            CHECK(record.wire.empty());
        }
    }
    CHECK(!reader.failed());
    CHECK(count == 7 && redacted == 1);

    // The first record's fixed header, byte by byte.
    const uint8_t* r = file.data() + kCaptureFileHeaderBytes;
    CHECK(loadU64(r) == 16667);
    CHECK(r[8] == 0 && r[9] == 0);   // received, reliable
    CHECK(loadU16(r + 10) == 0);
    CHECK(loadU64(r + 12) == kSteam);
    CHECK(loadU32(r + 20) == loadU32(r + kCaptureRecordHeaderBytes + 28) + kHeaderBytes);
}

static void testJsonRoundTrip() {
    const std::vector<uint8_t> file = sampleCapture();
    std::string json, error;
    CHECK(captureToJson(file, json, error));
    CHECK(json.find("\"message\": \"Hello\"") != std::string::npos);
    CHECK(json.find("\"lobby_id\": \"76561197960265729\"") != std::string::npos);
    CHECK(json.find("\"peer\": \"76561197960265729\"") != std::string::npos);
    CHECK(json.find("\"redacted\": true") != std::string::npos);
    CHECK(json.find("\"reason_code_name\": \"ContentMismatch\"") != std::string::npos);
    CHECK(json.find("\"raw\": ") != std::string::npos);
    CHECK(json.find("5ec2e771") == std::string::npos);   // no ticket bytes in the dump either

    std::vector<uint8_t> back;
    CHECK(captureFromJson(json, back, error));
    if (back != file) std::fprintf(stderr, "  dump -> encode changed the capture (%s)\n", error.c_str());
    CHECK(back == file);
    std::string again;
    CHECK(captureToJson(back, again, error));
    CHECK(again == json);

    // An empty capture.
    std::vector<uint8_t> empty;
    appendCaptureHeader(empty);
    CHECK(captureToJson(empty, json, error));
    CHECK(captureFromJson(json, back, error) && back == empty);
}

static void testJsonRejects() {
    std::string json, error;
    const std::vector<uint8_t> file = sampleCapture();
    CHECK(captureToJson(file, json, error));
    std::vector<uint8_t> out;
    auto rejects = [&](std::string text) {
        error.clear();
        const bool ok = captureFromJson(text, out, error);
        if (ok) std::fprintf(stderr, "  accepted: %.80s\n", text.c_str());
        return !ok && !error.empty();
    };
    auto replaced = [&](std::string_view from, std::string_view to) {
        std::string text = json;
        const size_t at = text.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) text.replace(at, from.size(), to);
        return text;
    };
    CHECK(rejects("not json"));
    CHECK(rejects("{}"));
    CHECK(rejects(replaced("\"version\": 1", "\"version\": 2")));
    CHECK(rejects(replaced("\"lobby_id\": \"76561197960265729\"", "\"lobby_id\": 76561197960265729")));
    CHECK(rejects(replaced("\"scene_id\": 3", "\"scene_id\": 70000")));
    CHECK(rejects(replaced("\"scene_id\": 3", "\"scene_id\": 0")));   // out of the message's range
    CHECK(rejects(replaced("\"scene_id\": 3", "\"scene_id\": 3.5")));
    CHECK(rejects(replaced("\"scene_id\": 3", "\"scene_id\": 3, \"extra\": 1")));
    CHECK(rejects(replaced("\"message\": \"Hello\"", "\"message\": \"Howdy\"")));
    CHECK(rejects(replaced(
        "\"redacted\": true", "\"message\": \"AuthTicket\", \"session\": \"00\", \"sender_tick\": 0, "
                              "\"fields\": {\"ticket\": \"5ec2e771\"}")));
    CHECK(rejects(replaced("\"direction\": \"sent\"", "\"direction\": \"sideways\"")));
    CHECK(rejects(replaced("\"time_us\": \"16667\"", "\"time_us\": 16667")));
    // Raw bytes that are really an authentication ticket can't sneak in either.
    const std::vector<uint8_t> ticket = encoded(AuthTicket{kTicket});
    std::string hex;
    for (const uint8_t b : ticket) {
        static const char* digits = "0123456789abcdef";
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }
    CHECK(rejects(replaced("\"raw\": \"", "\"raw\": \"" + hex + "\", \"was\": \"")));
}

static void testBadFiles() {
    const std::vector<uint8_t> file = sampleCapture();
    auto fails = [](std::vector<uint8_t> bytes) {
        CaptureReader reader(bytes);
        CaptureRecord record;
        while (reader.next(record)) {}
        std::string json, error;
        const bool dumped = captureToJson(bytes, json, error);
        return reader.failed() && !dumped && !error.empty();
    };
    auto patched = [&](size_t offset, uint8_t value) {
        std::vector<uint8_t> b = file;
        b[offset] = value;
        return b;
    };
    CHECK(fails({}));
    CHECK(fails({'S', 'W', 'N'}));
    CHECK(fails(patched(0, 'X')));
    CHECK(fails(patched(4, 2)));   // version 2
    CHECK(fails(patched(6, 1)));   // reserved
    const size_t r = kCaptureFileHeaderBytes;
    CHECK(fails(patched(r + 8, 2)));   // direction
    CHECK(fails(patched(r + 9, 2)));   // delivery
    CHECK(fails(patched(r + 10, 2)));   // unknown flag
    CHECK(fails(patched(r + 10, 1)));   // "redacted" but holding bytes
    {
        std::vector<uint8_t> b = file;
        storeU32(b.data() + r + 20, kMaxCaptureRecordBytes + 1);
        CHECK(fails(b));
    }
    // Cut anywhere inside a record: never read past the end, always reported.
    CaptureReader whole(file);
    CaptureRecord record;
    std::vector<size_t> ends = {kCaptureFileHeaderBytes};
    size_t at = kCaptureFileHeaderBytes;
    while (whole.next(record)) ends.push_back(at += kCaptureRecordHeaderBytes + record.wire.size());
    for (size_t length = kCaptureFileHeaderBytes; length < file.size(); ++length) {
        if (std::find(ends.begin(), ends.end(), length) != ends.end()) continue;   // a clean record boundary
        CHECK(fails(std::vector<uint8_t>(file.begin(), file.begin() + length)));
    }
    // Ending exactly on a record boundary is a valid, shorter capture.
    CHECK(!fails(std::vector<uint8_t>(file.begin(), file.begin() + ends[3])));

    // Oversized records can't be written.
    std::vector<uint8_t> out;
    appendCaptureHeader(out);
    const std::vector<uint8_t> huge(kMaxCaptureRecordBytes + 1, 1);
    CHECK(!appendCaptureRecord(out, {0, CaptureDirection::Sent, Delivery::ReliableOrdered, 0, 0, huge}));
    CHECK(!appendCaptureRecord(out, {0, CaptureDirection::Sent, Delivery::ReliableOrdered, 2, 0, {}}));
}

static std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Captures a real loopback exchange to a file, with a small size cap.
static void testWriterAndCap(const fs::path& dir) {
    LoopbackHub hub;
    auto host = hub.createTransport(PeerId{1});
    auto client = hub.createTransport(PeerId{kSteam});
    CHECK(bool(host->listen({TransportAddress::Loopback, 1, 0}, {})));

    struct CaptureSink : TransportSink {
        ITransport* host;
        CaptureWriter* writer;
        uint64_t now = 0;
        int written = 0, redacted = 0, full = 0;
        void onEvent(const TransportEvent& e) override {
            if (e.kind == TransportEventKind::Incoming) (void)host->accept(e.connection);
        }
        void onMessage(const ReceivedMessage& m) override {
            switch (writer->write({m.receivedUs, CaptureDirection::Received, m.delivery, 0, m.peer.value, m.bytes})) {
            case CaptureWrite::Written: ++written; break;
            case CaptureWrite::Redacted: ++redacted; break;
            case CaptureWrite::Full: ++full; break;
            case CaptureWrite::Failed: CHECK(false); break;
            }
        }
    };
    const fs::path path = dir / "session.swnetcap";
    CaptureWriter writer;
    const uint64_t cap = 400;
    CHECK(bool(writer.open(path, cap)));
    CaptureSink sink;
    sink.host = host.get();
    sink.writer = &writer;
    struct Quiet : TransportSink {
        void onEvent(const TransportEvent&) override {}
        void onMessage(const ReceivedMessage&) override {}
    } quiet;
    const ConnectionId id = client->connect({TransportAddress::Loopback, 1, 0}).value();
    host->poll(sink, {});
    client->poll(quiet, {});

    const std::vector<uint8_t> ping = encoded(Ping{1, 2});
    const std::vector<uint8_t> ticket = encoded(AuthTicket{kTicket});
    CHECK(client->send(id, ticket, Delivery::ReliableOrdered) == SendResult::Queued);
    for (int i = 0; i < 20; ++i) CHECK(client->send(id, ping, Delivery::Unreliable) == SendResult::Queued);
    host->poll(sink, {});
    writer.close();
    CHECK(sink.redacted == 1);
    // 8-byte header, a 24-byte redacted record, then 24 + 44 byte pings until 400 bytes.
    CHECK(sink.written == 5);
    CHECK(sink.full == 15);
    CHECK(writer.full());

    const std::vector<uint8_t> bytes = readFile(path);
    CHECK(bytes.size() <= cap);
    CHECK(bytes.size() == writer.bytesWritten());
    CHECK(!contains(bytes, kTicket));
    CaptureReader reader(bytes);
    CaptureRecord record;
    int records = 0;
    while (reader.next(record)) {
        ++records;
        CHECK(record.peer == kSteam);
    }
    CHECK(!reader.failed() && records == 6);

    // Dumping and re-encoding a capture sends nothing.
    const LoopbackStats before = hub.stats();
    std::string json, error;
    std::vector<uint8_t> back;
    CHECK(captureToJson(bytes, json, error));
    CHECK(captureFromJson(json, back, error));
    CHECK(back == bytes);
    const LoopbackStats after = hub.stats();
    CHECK(after.packetsInFlight == before.packetsInFlight && after.delivered == before.delivered);

    // Opening somewhere impossible fails cleanly.
    CaptureWriter bad;
    CHECK(bad.open(dir / "missing" / "nested" / "x.swnetcap").error().code == NetErrorCode::Io);
    CHECK(bad.write({}) == CaptureWrite::Failed);
}

int main() {
    testFormat();
    testJsonRoundTrip();
    testJsonRejects();
    testBadFiles();

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / ("shockwave-netcap-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir, ec);
    CHECK(!ec);
    testWriterAndCap(dir);
    fs::remove_all(dir, ec);   // every file is closed by now (Windows can't delete open ones)

    if (failures) {
        std::fprintf(stderr, "network_capture: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("network_capture: all checks passed\n");
    return 0;
}
