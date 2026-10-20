// Wire protocol v1 (Network/Wire.h, Network/Messages.h): golden bytes for every message,
// header and input command layouts, truncation at every length, bad headers, counts,
// ranges, strings, floats and quaternions, delivery and direction, endian vectors, tick
// wraparound, bounded fuzzing, no allocation while encoding or decoding, and that
// docs/NetworkProtocol.md matches the catalog.
//
// The golden bytes below come from Tests/fixtures/network_golden.py, which builds them
// with Python's struct module straight from the protocol tables, so the C++ writer isn't
// checking itself.
#include <Network/Loopback.h>
#include <Network/Messages.h>
#include <Network/Wire.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

using namespace shockwave::net;

// Counts heap allocations, to show encoding and decoding make none.
static std::atomic<uint64_t> g_allocations{0};
void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

struct GoldenMessage {
    const char* name;
    const char* hex;
};

// Golden bytes (see the comment at the top of this file).
const GoldenMessage kGolden[] = {
    {"Hello", "53574d500100010000000000000000000000000000000000000000006400000008006172656e612e763101080f161d24"
              "2b323940474e555c636a71787f868d949ba2a9b0b7bec5ccd3da030e19242f3a45505b66717c87929da8b3bec9d4dfea"
              "f5000b16212c37424d5801000100000001001001c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"},
    {"Challenge", "53574d5001000200101112131415161718191a1b1c1d1e1f000000006c000000c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
                  "08006172656e612e763101080f161d242b323940474e555c636a71787f868d949ba2a9b0b7bec5ccd3da030e19242f3a"
                  "45505b66717c87929da8b3bec9d4dfeaf5000b16212c37424d580100020000000100100178001e006400c800"},
    {"AuthTicket", "53574d5001000300101112131415161718191a1b1c1d1e1f00000000070000000500deadbeef01"},
    {"Welcome", "53574d5001000400101112131415161718191a1b1c1d1e1f000000000c000000030007000000efcdab000002"},
    {"BaselineBegin", "53574d5001000500101112131415161718191a1b1c1d1e1f000000000e00000007000000efcdab0002008a000000"},
    {"Spawn", "53574d5001000600101112131415161718191a1b1c1d1e1f000000001d00000001000000020000000100000001000000"
              "03000100000020000300010203"},
    {"Despawn", "53574d5001000700101112131415161718191a1b1c1d1e1f000000000d000000020000002a0000000000000001"},
    {"Ownership", "53574d5001000800101112131415161718191a1b1c1d1e1f0000000012000000030000002a0000000000000002000000"
                  "0400"},
    {"InitialState", "53574d5001000900101112131415161718191a1b1c1d1e1f000000004b0000000700000001002a000000000000000200"
                     "0000010035000000c03f000010c000007a440000000000000000000000000000803f0000003f00000000000040c00000"
                     "00000000a03f0000000002"},
    {"BaselineEnd", "53574d5001000a00101112131415161718191a1b1c1d1e1f0000000028000000070000000300000005121f2c39465360"
                    "6d7a8794a1aebbc8d5e2effc091623303d4a5764717e8b98"},
    {"Ready", "53574d5001000b00101112131415161718191a1b1c1d1e1f00000000240000000700000005121f2c394653606d7a8794"
              "a1aebbc8d5e2effc091623303d4a5764717e8b98"},
    {"Active", "53574d5001000c00101112131415161718191a1b1c1d1e1fe803000014000000e8030000ec0300002a00000000000000"
               "02000000"},
    {"Inputs", "53574d5001000d00101112131415161718191a1b1c1d1e1fec0300003100000002ec030000020000005aa6008050fb01"
               "000000e60300000040ed03000002000000007fffffff7f03000300e70300000000"},
    {"Snapshot", "53574d5001000e00101112131415161718191a1b1c1d1e1ff20300006b00000037000000f20300000300000000000100"
                 "02002a0000000000000002000000010035000000c03f000010c000007a440000000000000000000000000000803f0000"
                 "003f00000000000040c0000000000000a03f0000000002010000000100100100000000210004000a0b0c0d"},
    {"Command", "53574d5001000f00101112131415161718191a1b1c1d1e1fed0300001100000009000000000000000000000020000100"
                "01"},
    {"GameEvent", "53574d5001001000101112131415161718191a1b1c1d1e1ff40300001c00000004000000f40300002a00000000000000"
                  "210008000001020304050607"},
    {"Ping", "53574d5001001100101112131415161718191a1b1c1d1e1ff50300000c0000004d0000009078563412000000"},
    {"Pong", "53574d5001001200101112131415161718191a1b1c1d1e1ff5030000200000004d0000009078563412000000404b4c00"
             "000000003a4c4c0000000000f5030000"},
    {"Reject", "53574d5001001300101112131415161718191a1b1c1d1e1f000000001200000003000e00736572766572206973206675"
               "6c6c"},
    {"Goodbye", "53574d5001001400101112131415161718191a1b1c1d1e1ff6030000020000000a00"},
    {"ResyncRequest", "53574d5001001500101112131415161718191a1b1c1d1e1ff103000005000000f103000002"},
    {"DeltaSnapshot", "53574d5001001600101112131415161718191a1b1c1d1e1ffc0300001a000000fc030000f60300000300000001000205"
                      "000c0002000300a53c0f"},
    {"SnapshotAck", "53574d5001001700101112131415161718191a1b1c1d1e1ffd03000008000000fc03000001000080"},
};
const char* kPoseHex =
    "0000c03f000010c000007a440000000000000000000000000000803f0000003f00000000000040c0000000000000a03f0000000002";

static std::vector<uint8_t> unhex(const char* hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        out.push_back(uint8_t(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    }
    return out;
}

static Nonce sequence16(uint8_t first) {
    Nonce n;
    for (int i = 0; i < 16; ++i) n[i] = uint8_t(first + i);
    return n;
}

static Digest digest(int mul, int add) {
    Digest d;
    for (int i = 0; i < 32; ++i) d[i] = uint8_t((i * mul + add) & 0xFF);
    return d;
}

static const Nonce kSession = sequence16(0x10);
static const Nonce kClientNonce = sequence16(0xC0);
static const Digest kSchema = digest(7, 1), kContent = digest(11, 3), kBaseline = digest(13, 5);
static constexpr uint64_t kSteam1 = 76561197960265729ull, kSteam2 = 76561197960265730ull;

static PoseState goldenPose() {
    PoseState pose;
    pose.position = {1.5f, -2.25f, 1000.0f};
    pose.rotation = {0, 0, 0, 1};
    pose.linearVelocity = {0.5f, 0, -3};
    pose.angularVelocity = {0, 1.25f, 0};
    pose.flags = PoseState::kTeleport;
    return pose;
}

// Storage the golden messages' spans point into.
static std::vector<uint8_t> g_poseBytes, g_initialRecords, g_snapshotRecords;
static const uint8_t kTicket[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
static const uint8_t kSpawnState[] = {1, 2, 3};
static const uint8_t kCommandPayload[] = {1};
static const uint8_t kEventPayload[] = {0, 1, 2, 3, 4, 5, 6, 7};
static const uint8_t kDeltaPayload[] = {0xA5, 0x3C, 0x0F};
static const uint8_t kPropState[] = {10, 11, 12, 13};

static void buildRecords() {
    g_poseBytes.assign(kPoseStateBytes, 0);
    uint32_t written = 0;
    CHECK(bool(encodePose(goldenPose(), g_poseBytes, written)));
    CHECK(written == kPoseStateBytes);
    CHECK(g_poseBytes == unhex(kPoseHex));

    auto records = [](std::vector<uint8_t>& out, std::initializer_list<StateRecord> list) {
        out.assign(2048, 0);
        WireWriter writer(out);
        for (const StateRecord& r : list) CHECK(writeStateRecord(writer, r));
        out.resize(writer.size());
    };
    records(g_initialRecords, {{42, 2, kProfilePose, g_poseBytes}});
    records(g_snapshotRecords, {{42, 2, kProfilePose, g_poseBytes}, {kSteam1, 0, 33, kPropState}});
}

struct Built {
    MessageBody body;
    Nonce session;
    uint32_t tick;
};

static Built goldenMessage(size_t index) {
    switch (index + 1) {
    case 1: {
        Hello m;
        m.gameProtocol = "arena.v1";
        m.schemaDigest = kSchema;
        m.contentDigest = kContent;
        m.sceneId = 1;
        m.lobbyId = kSteam1;
        m.clientNonce = kClientNonce;
        return {m, Nonce{}, 0};
    }
    case 2: {
        Challenge m;
        m.clientNonce = kClientNonce;
        m.gameProtocol = "arena.v1";
        m.schemaDigest = kSchema;
        m.contentDigest = kContent;
        m.sceneId = 1;
        m.hostPeer = kSteam2;
        m.tickRate = 120;
        m.snapshotRate = 30;
        m.interpolationMs = 100;
        m.maxRewindMs = 200;
        return {m, kSession, 0};
    }
    case 3: return {AuthTicket{kTicket}, kSession, 0};
    case 4: return {Welcome{3, 7, 0xABCDEF, 512}, kSession, 0};
    case 5: return {BaselineBegin{7, 0xABCDEF, 2, 138}, kSession, 0};
    case 6: return {Spawn{1, 0x100000002ull, 1, 3, 1, "", 32, kSpawnState}, kSession, 0};
    case 7: return {Despawn{2, 42, 1}, kSession, 0};
    case 8: return {Ownership{3, 42, 2, 4}, kSession, 0};
    case 9: return {InitialState{7, 1, g_initialRecords}, kSession, 0};
    case 10: return {BaselineEnd{7, 3, kBaseline}, kSession, 0};
    case 11: return {Ready{7, kBaseline}, kSession, 0};
    case 12: return {Active{1000, 1004, 42, 2}, kSession, 1000};
    case 13: {
        Inputs m;
        m.count = 2;
        m.commands[0] = {1004, 2, 90, -90, 0x8000, -1200, 1, 0, 998, 0x4000};
        m.commands[1] = {1005, 2, 0, 127, 0xFFFF, 32767, 3, 3, 999, 0};
        return {m, kSession, 1004};
    }
    case 14: return {Snapshot{55, 1010, 3, 0, 1, 2, g_snapshotRecords}, kSession, 1010};
    case 15: return {Command{9, 0, 32, kCommandPayload}, kSession, 1005};
    case 16: return {GameEvent{4, 1012, 42, 33, kEventPayload}, kSession, 1012};
    case 17: return {Ping{77, 0x1234567890ull}, kSession, 1013};
    case 18: return {Pong{77, 0x1234567890ull, 5000000, 5000250, 1013}, kSession, 1013};
    case 19: return {Reject{ReasonCode::ServerFull, "server is full"}, kSession, 0};
    case 20: return {Goodbye{ReasonCode::HostLeft}, kSession, 1014};
    case 21: return {ResyncRequest{1009, 2}, kSession, 1009};
    case 22:
        return {
            DeltaSnapshot{1020, 1014, 3, DeltaSnapshot::kHasBaseline, 0, 2, 5, 12, 2, kDeltaPayload}, kSession, 1020};
    case 23: return {SnapshotAck{1020, 0x80000001u}, kSession, 1021};
    }
    std::abort();
}

static DecodeOptions optionsFor(MessageKind kind) {
    const MessageDescriptor& d = *findMessage(uint8_t(kind));
    DecodeOptions options;
    options.delivery = d.delivery;
    options.receiver = d.direction == MessageDirection::HostToClient ? PeerRole::Client : PeerRole::Host;
    return options;
}

static std::vector<uint8_t> encode(const Built& built) {
    std::vector<uint8_t> out(kMaxReliableMessageBytes);
    uint32_t written = 0;
    const WireStatus status = encodeMessage(built.body, built.session, built.tick, out, written);
    if (!status) std::fprintf(stderr, "  encode failed: %s (%s)\n", wireStatusName(status.code), status.field);
    out.resize(status ? written : 0);
    return out;
}

static void testGolden() {
    CHECK(std::size(kGolden) == kLastMessageKind);
    std::vector<uint8_t> out(kMaxReliableMessageBytes);
    for (size_t i = 0; i < std::size(kGolden); ++i) {
        const std::vector<uint8_t> golden = unhex(kGolden[i].hex);
        const MessageDescriptor& d = messageCatalog()[i];
        CHECK(std::string(d.name) == kGolden[i].name);
        CHECK(uint8_t(d.kind) == i + 1);

        const Built built = goldenMessage(i);
        CHECK(kindOf(built.body) == d.kind);
        const std::vector<uint8_t> encoded = encode(built);
        if (encoded != golden) std::fprintf(stderr, "  %s: encoded bytes differ from golden\n", d.name);
        CHECK(encoded == golden);

        DecodedMessage decoded;
        const WireStatus status = decodeMessage(golden, optionsFor(d.kind), decoded);
        if (!status)
            std::fprintf(
                stderr, "  %s: decode failed: %s at %u (%s)\n", d.name, wireStatusName(status.code), status.offset,
                status.field);
        CHECK(bool(status));
        CHECK(decoded.header.kind == d.kind);
        CHECK(decoded.header.session == built.session);
        CHECK(decoded.header.senderTick == built.tick);
        CHECK(decoded.header.payloadBytes == golden.size() - kHeaderBytes);
        // Decoding then encoding gives the same bytes: the format is canonical.
        uint32_t written = 0;
        CHECK(bool(encodeMessage(decoded.body, decoded.header.session, decoded.header.senderTick, out, written)));
        CHECK(std::vector<uint8_t>(out.begin(), out.begin() + written) == golden);
    }

    // Spot checks of decoded values.
    {
        const std::vector<uint8_t> bytes = unhex(kGolden[0].hex);
        DecodedMessage m;
        CHECK(bool(decodeMessage(bytes, optionsFor(MessageKind::Hello), m)));
        const Hello& hello = std::get<Hello>(m.body);
        CHECK(hello.gameProtocol == "arena.v1");
        CHECK(hello.lobbyId == kSteam1);
        CHECK(hello.schemaDigest == kSchema && hello.contentDigest == kContent);
        CHECK(hello.clientNonce == kClientNonce);
    }
    {
        const std::vector<uint8_t> bytes = unhex(kGolden[13].hex);
        DecodedMessage m;
        CHECK(bool(decodeMessage(bytes, optionsFor(MessageKind::Snapshot), m)));
        const Snapshot& snapshot = std::get<Snapshot>(m.body);
        StateRecordReader records(snapshot.records, snapshot.recordCount);
        StateRecord record;
        CHECK(records.next(record));
        CHECK(record.entity == 42 && record.incarnation == 2 && record.profileId == kProfilePose);
        PoseState pose;
        CHECK(bool(decodePose(record.state, pose)));
        CHECK(pose == goldenPose());
        CHECK(records.next(record));
        CHECK(record.entity == kSteam1 && record.profileId == 33 && record.state.size() == 4);
        CHECK(!records.next(record));
    }
}

static void testHeaderLayout() {
    const std::vector<uint8_t> bytes = unhex(kGolden[19].hex);   // Goodbye
    CHECK(kHeaderBytes == 32);
    CHECK(bytes.size() == 34);
    CHECK(std::memcmp(bytes.data(), "SWMP", 4) == 0);
    CHECK(bytes[4] == 1 && bytes[5] == 0);   // version 1, little endian
    CHECK(bytes[6] == 20);   // kind
    CHECK(bytes[7] == 0);   // flags
    CHECK(std::memcmp(bytes.data() + 8, kSession.data(), 16) == 0);
    CHECK(loadU32(bytes.data() + 24) == 1014);   // sender tick
    CHECK(loadU32(bytes.data() + 28) == 2);   // payload bytes
    CHECK(loadU16(bytes.data() + 32) == uint16_t(ReasonCode::HostLeft));
}

static void testInputCommandLayout() {
    CHECK(kInputCommandBytes == 24);
    const std::vector<uint8_t> bytes = unhex(kGolden[12].hex);   // Inputs, two commands
    CHECK(bytes.size() == kHeaderBytes + 1 + 2 * 24);
    const uint8_t* c = bytes.data() + kHeaderBytes + 1;
    CHECK(loadU32(c + 0) == 1004);   // target_tick
    CHECK(loadU32(c + 4) == 2);   // incarnation
    CHECK(int8_t(c[8]) == 90 && int8_t(c[9]) == -90);   // move_x, move_z
    CHECK(loadU16(c + 10) == 0x8000);   // yaw
    CHECK(int16_t(loadU16(c + 12)) == -1200);   // pitch
    CHECK(loadU16(c + 14) == 1);   // held_bits
    CHECK(loadU16(c + 16) == 0);   // pressed_bits
    CHECK(loadU32(c + 18) == 998);   // view_tick
    CHECK(loadU16(c + 22) == 0x4000);   // view_fraction
}

static WireStatus decodeAs(const std::vector<uint8_t>& bytes, MessageKind kind) {
    DecodedMessage m;
    return decodeMessage(bytes, optionsFor(kind), m);
}

static void fixLength(std::vector<uint8_t>& bytes) {
    if (bytes.size() >= kHeaderBytes) storeU32(bytes.data() + 28, uint32_t(bytes.size() - kHeaderBytes));
}

static void testTruncation() {
    for (size_t i = 0; i < std::size(kGolden); ++i) {
        const std::vector<uint8_t> golden = unhex(kGolden[i].hex);
        const MessageKind kind = MessageKind(i + 1);
        for (size_t length = 0; length < golden.size(); ++length) {
            std::vector<uint8_t> cut(golden.begin(), golden.begin() + length);
            CHECK(!decodeAs(cut, kind));
            // Even with the header's length made to agree, a cut message never decodes.
            fixLength(cut);
            const WireStatus status = decodeAs(cut, kind);
            if (status) std::fprintf(stderr, "  %s decoded when cut to %zu bytes\n", kGolden[i].name, length);
            CHECK(!status);
        }
        // One byte too many is never allowed either.
        std::vector<uint8_t> longer = golden;
        longer.push_back(0);
        fixLength(longer);
        const WireStatus status = decodeAs(longer, kind);
        CHECK(status.code == WireStatusCode::TrailingBytes);
    }
}

static void testBadHeaders() {
    const std::vector<uint8_t> golden = unhex(kGolden[19].hex);   // Goodbye
    auto with = [&](size_t offset, uint8_t value) {
        std::vector<uint8_t> b = golden;
        b[offset] = value;
        return b;
    };
    CHECK(decodeAs(with(0, 'X'), MessageKind::Goodbye).code == WireStatusCode::BadMagic);
    WireStatus version = decodeAs(with(4, 2), MessageKind::Goodbye);
    CHECK(version.code == WireStatusCode::BadVersion);
    CHECK(version.reason() == ReasonCode::ProtocolMismatch);
    WireStatus kind0 = decodeAs(with(6, 0), MessageKind::Goodbye);
    CHECK(kind0.code == WireStatusCode::UnknownKind && kind0.reason() == ReasonCode::ProtocolMismatch);
    CHECK(decodeAs(with(6, kLastMessageKind + 1), MessageKind::Goodbye).code == WireStatusCode::UnknownKind);
    CHECK(decodeAs(with(6, 255), MessageKind::Goodbye).code == WireStatusCode::UnknownKind);
    for (int bit = 0; bit < 8; ++bit) {
        const WireStatus flags = decodeAs(with(7, uint8_t(1 << bit)), MessageKind::Goodbye);
        CHECK(flags.code == WireStatusCode::BadFlags && flags.reason() == ReasonCode::MalformedPacket);
    }
    CHECK(decodeAs(with(28, 3), MessageKind::Goodbye).code == WireStatusCode::LengthMismatch);
    CHECK(decodeAs(with(31, 1), MessageKind::Goodbye).code == WireStatusCode::LengthMismatch);
    // An unknown reason code.
    CHECK(decodeAs(with(32, 15), MessageKind::Goodbye).code == WireStatusCode::OutOfRange);

    // Size limits: unreliable messages are capped at 1152 bytes, reliable at 16384.
    std::vector<uint8_t> ping = unhex(kGolden[16].hex);
    ping.resize(kMaxUnreliableMessageBytes + 1);
    fixLength(ping);
    CHECK(decodeAs(ping, MessageKind::Ping).code == WireStatusCode::TooLarge);
    std::vector<uint8_t> goodbye = golden;
    goodbye.resize(kMaxReliableMessageBytes + 1);
    fixLength(goodbye);
    CHECK(decodeAs(goodbye, MessageKind::Goodbye).code == WireStatusCode::TooLarge);
}

static void testDeliveryAndDirection() {
    DecodedMessage m;
    const std::vector<uint8_t> snapshot = unhex(kGolden[13].hex);
    DecodeOptions options = optionsFor(MessageKind::Snapshot);
    options.delivery = Delivery::ReliableOrdered;
    CHECK(decodeMessage(snapshot, options, m).code == WireStatusCode::WrongDelivery);
    options = optionsFor(MessageKind::Snapshot);
    options.receiver = PeerRole::Host;   // hosts never receive snapshots
    CHECK(decodeMessage(snapshot, options, m).code == WireStatusCode::WrongDirection);

    const std::vector<uint8_t> hello = unhex(kGolden[0].hex);
    options = optionsFor(MessageKind::Hello);
    options.delivery = Delivery::Unreliable;
    CHECK(decodeMessage(hello, options, m).code == WireStatusCode::WrongDelivery);
    options = optionsFor(MessageKind::Hello);
    options.receiver = PeerRole::Client;
    CHECK(decodeMessage(hello, options, m).code == WireStatusCode::WrongDirection);

    const std::vector<uint8_t> inputs = unhex(kGolden[12].hex);
    options = optionsFor(MessageKind::Inputs);
    options.receiver = PeerRole::Client;
    CHECK(decodeMessage(inputs, options, m).code == WireStatusCode::WrongDirection);

    // Messages that go either way are accepted by both.
    const std::vector<uint8_t> ping = unhex(kGolden[16].hex);
    options = optionsFor(MessageKind::Ping);
    options.receiver = PeerRole::Client;
    CHECK(bool(decodeMessage(ping, options, m)));
    options.receiver = PeerRole::Host;
    CHECK(bool(decodeMessage(ping, options, m)));
}

static WireStatus encodeStatus(const MessageBody& body) {
    std::vector<uint8_t> out(kMaxReliableMessageBytes);
    uint32_t written = 0;
    return encodeMessage(body, kSession, 0, out, written);
}

static void testRanges() {
    // Inputs: count, ranges, reserved bits and the diagonal rule, on both encode and decode.
    Inputs inputs = std::get<Inputs>(goldenMessage(12).body);
    auto inputsFail = [&](auto change, WireStatusCode expect) {
        Inputs bad = inputs;
        change(bad);
        CHECK(encodeStatus(bad).code == expect);
    };
    inputsFail([](Inputs& m) { m.count = 0; }, WireStatusCode::OutOfRange);
    inputsFail([](Inputs& m) { m.count = 5; }, WireStatusCode::OutOfRange);
    inputsFail([](Inputs& m) { m.commands[0].moveX = -128; }, WireStatusCode::OutOfRange);
    inputsFail([](Inputs& m) { m.commands[1].pitch = -32768; }, WireStatusCode::OutOfRange);
    inputsFail([](Inputs& m) { m.commands[0].heldBits = 4; }, WireStatusCode::OutOfRange);
    inputsFail([](Inputs& m) { m.commands[0].pressedBits = 0x8000; }, WireStatusCode::OutOfRange);
    inputsFail(
        [](Inputs& m) {
            m.commands[0].moveX = 127;
            m.commands[0].moveZ = 127;
        },
        WireStatusCode::Invalid);
    {
        Inputs four = inputs;
        four.count = 4;
        four.commands[2] = four.commands[3] = four.commands[1];
        CHECK(bool(encodeStatus(four)));
    }

    // The same rules when decoding: patch the golden bytes.
    const std::vector<uint8_t> golden = unhex(kGolden[12].hex);
    auto patched = [&](size_t offset, uint8_t value) {
        std::vector<uint8_t> b = golden;
        b[offset] = value;
        return decodeAs(b, MessageKind::Inputs);
    };
    const size_t first = kHeaderBytes + 1;
    CHECK(patched(kHeaderBytes, 0).code == WireStatusCode::OutOfRange);   // count 0
    CHECK(patched(kHeaderBytes, 5).code == WireStatusCode::OutOfRange);   // count 5
    CHECK(patched(kHeaderBytes, 3).code == WireStatusCode::Truncated);   // count 3, two present
    CHECK(patched(kHeaderBytes, 1).code == WireStatusCode::TrailingBytes);
    CHECK(patched(first + 8, 0x80).code == WireStatusCode::OutOfRange);   // move_x -128
    {
        std::vector<uint8_t> b = golden;
        storeU16(b.data() + first + 12, 0x8000);   // pitch -32768
        CHECK(decodeAs(b, MessageKind::Inputs).code == WireStatusCode::OutOfRange);
        b = golden;
        storeU16(b.data() + first + 14, 4);   // held bit 2
        CHECK(decodeAs(b, MessageKind::Inputs).code == WireStatusCode::OutOfRange);
    }

    // Challenge: tick rates 60..120, snapshot rates from the list that divide the tick rate.
    Challenge challenge = std::get<Challenge>(goldenMessage(1).body);
    auto challengeWith = [&](uint16_t tick, uint16_t snapshot) {
        Challenge c = challenge;
        c.tickRate = tick;
        c.snapshotRate = snapshot;
        return encodeStatus(c);
    };
    CHECK(bool(challengeWith(60, 20)));
    CHECK(bool(challengeWith(60, 60)));
    CHECK(bool(challengeWith(120, 20)));
    CHECK(bool(challengeWith(120, 60)));
    CHECK(bool(challengeWith(90, 30)));
    CHECK(challengeWith(59, 20).code == WireStatusCode::OutOfRange);
    CHECK(challengeWith(121, 20).code == WireStatusCode::OutOfRange);
    CHECK(challengeWith(240, 20).code == WireStatusCode::OutOfRange);
    CHECK(challengeWith(120, 25).code == WireStatusCode::Invalid);
    CHECK(challengeWith(90, 20).code == WireStatusCode::Invalid);   // 20 doesn't divide 90
    CHECK(challengeWith(100, 30).code == WireStatusCode::Invalid);
    {
        std::vector<uint8_t> b = unhex(kGolden[1].hex);
        const size_t tickAt = b.size() - 8;
        storeU16(b.data() + tickAt, 121);
        CHECK(decodeAs(b, MessageKind::Challenge).code == WireStatusCode::OutOfRange);
        storeU16(b.data() + tickAt, 60);
        CHECK(bool(decodeAs(b, MessageKind::Challenge)));   // 30 divides 60
        storeU16(b.data() + tickAt + 2, 25);
        CHECK(decodeAs(b, MessageKind::Challenge).code == WireStatusCode::Invalid);
    }

    // Other cross-field and range rules.
    CHECK(encodeStatus(Spawn{1, 5, 0, 0, 0, "", 1, {}}).code == WireStatusCode::Invalid);   // authored needs a key
    CHECK(encodeStatus(Spawn{1, 5, 0, 0, 2, "door", 1, {}}).code == WireStatusCode::Invalid);
    CHECK(bool(encodeStatus(Spawn{1, 5, 0, 0, 0, "door-west", 1, {}})));
    CHECK(bool(encodeStatus(Spawn{1, 5, 0, 64, 2, "", 1, {}})));   // 64 players
    CHECK(encodeStatus(Spawn{1, 5, 0, 65, 2, "", 1, {}}).code == WireStatusCode::OutOfRange);   // owner slot 65
    CHECK(encodeStatus(Spawn{0, 5, 0, 0, 2, "", 1, {}}).code == WireStatusCode::OutOfRange);   // serial 0
    CHECK(encodeStatus(Despawn{1, 0, 0}).code == WireStatusCode::OutOfRange);   // entity 0
    CHECK(encodeStatus(Welcome{1, 7, 0, 512}).code == WireStatusCode::OutOfRange);   // slot 1 is the host's
    CHECK(bool(encodeStatus(Welcome{64, 7, 0, 512})));
    CHECK(encodeStatus(Welcome{65, 7, 0, 512}).code == WireStatusCode::OutOfRange);
    CHECK(encodeStatus(Snapshot{1, 1, 1, 1, 1, 0, {}}).code == WireStatusCode::Invalid);
    CHECK(encodeStatus(Snapshot{1, 1, 1, 0, 257, 0, {}}).code == WireStatusCode::OutOfRange);
    CHECK(encodeStatus(Pong{1, 0, 10, 9, 0}).code == WireStatusCode::Invalid);
    CHECK(encodeStatus(Command{1, 0, 0, {}}).code == WireStatusCode::OutOfRange);   // command 0
    const std::vector<uint8_t> big(kMaxCommandPayloadBytes + 1, 7);
    CHECK(encodeStatus(Command{1, 0, 32, big}).code == WireStatusCode::OutOfRange);
    CHECK(encodeStatus(AuthTicket{}).code == WireStatusCode::OutOfRange);   // empty ticket
    const std::vector<uint8_t> ticket(kMaxAuthTicketBytes + 1, 1);
    CHECK(encodeStatus(AuthTicket{ticket}).code == WireStatusCode::OutOfRange);
    CHECK(bool(encodeStatus(AuthTicket{std::span(ticket).first(kMaxAuthTicketBytes)})));
    CHECK(encodeStatus(Reject{ReasonCode(15), ""}).code == WireStatusCode::OutOfRange);

    // A Snapshot that doesn't fit an unreliable message.
    std::vector<uint8_t> many(4096);
    WireWriter writer(many);
    uint16_t count = 0;
    while (writer.size() + kStateRecordHeaderBytes + kPoseStateBytes <= many.size()) {
        CHECK(writeStateRecord(writer, {uint64_t(count) + 1, 0, kProfilePose, g_poseBytes}));
        ++count;
    }
    many.resize(writer.size());
    CHECK(encodeStatus(Snapshot{1, 1, 1, 0, 1, count, many}).code == WireStatusCode::TooLarge);
    CHECK(bool(encodeStatus(InitialState{1, count, many})));   // reliable: fits
    // Records that don't add up to their count.
    CHECK(encodeStatus(InitialState{1, uint16_t(count + 1), many}).code == WireStatusCode::Truncated);
    CHECK(encodeStatus(InitialState{1, uint16_t(count - 1), many}).code == WireStatusCode::TrailingBytes);

    // Records with bad headers are rejected inside a decoded message.
    {
        std::vector<uint8_t> b = unhex(kGolden[8].hex);   // InitialState
        const size_t record = kHeaderBytes + 4 + 2;
        std::vector<uint8_t> zeroEntity = b;
        std::memset(zeroEntity.data() + record, 0, 8);
        CHECK(decodeAs(zeroEntity, MessageKind::InitialState).code == WireStatusCode::OutOfRange);
        std::vector<uint8_t> profile = b;
        storeU16(profile.data() + record + 12, 0);
        CHECK(decodeAs(profile, MessageKind::InitialState).code == WireStatusCode::OutOfRange);
        std::vector<uint8_t> length = b;
        storeU16(length.data() + record + 14, 513);
        CHECK(decodeAs(length, MessageKind::InitialState).code == WireStatusCode::OutOfRange);
        storeU16(length.data() + record + 14, 54);
        CHECK(decodeAs(length, MessageKind::InitialState).code == WireStatusCode::Truncated);
        storeU16(length.data() + record + 14, 52);
        CHECK(decodeAs(length, MessageKind::InitialState).code == WireStatusCode::TrailingBytes);
    }
}

static void testStrings() {
    CHECK(validUtf8(""));
    CHECK(validUtf8("arena.v1"));
    CHECK(validUtf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xAE"));
    CHECK(!validUtf8(std::string_view("a\0b", 3)));
    CHECK(!validUtf8("\xC0\xAF"));   // overlong '/'
    CHECK(!validUtf8("\xE0\x80\xAF"));   // overlong
    CHECK(!validUtf8("\xED\xA0\x80"));   // surrogate
    CHECK(!validUtf8("\xF4\x90\x80\x80"));   // past U+10FFFF
    CHECK(!validUtf8("\xC3"));   // cut short
    CHECK(!validUtf8("\x80"));   // stray continuation
    CHECK(!validUtf8("\xFF"));

    Hello hello = std::get<Hello>(goldenMessage(0).body);
    auto withProtocol = [&](std::string_view text) {
        Hello h = hello;
        h.gameProtocol = text;
        return encodeStatus(h);
    };
    CHECK(withProtocol("").code == WireStatusCode::OutOfRange);
    CHECK(withProtocol(std::string(64, 'a')));
    CHECK(withProtocol(std::string(65, 'a')).code == WireStatusCode::OutOfRange);
    CHECK(withProtocol("bad\xC0\xAF").code == WireStatusCode::BadString);
    CHECK(withProtocol(std::string_view("nul\0", 4)).code == WireStatusCode::BadString);

    // Decoding: a string with a bad byte, a NUL, or a length past its limit.
    const std::vector<uint8_t> golden = unhex(kGolden[0].hex);
    const size_t text = kHeaderBytes + 2;
    std::vector<uint8_t> b = golden;
    b[text + 2] = 0xFF;
    CHECK(decodeAs(b, MessageKind::Hello).code == WireStatusCode::BadString);
    b = golden;
    b[text + 2] = 0;
    CHECK(decodeAs(b, MessageKind::Hello).code == WireStatusCode::BadString);
    b = golden;
    storeU16(b.data() + kHeaderBytes, 65);
    CHECK(decodeAs(b, MessageKind::Hello).code == WireStatusCode::BadString);
    b = golden;
    storeU16(b.data() + kHeaderBytes, 0);   // empty, and everything after shifts
    CHECK(!decodeAs(b, MessageKind::Hello));
}

static void testFloatsAndPose() {
    std::vector<uint8_t> pose = unhex(kPoseHex);
    PoseState decoded;
    CHECK(bool(decodePose(pose, decoded)));
    CHECK(decoded == goldenPose());

    auto withFloat = [&](size_t index, uint32_t bits) {
        std::vector<uint8_t> b = pose;
        storeU32(b.data() + 4 * index, bits);
        PoseState out;
        return decodePose(b, out);
    };
    CHECK(withFloat(0, 0x7FC00000).code == WireStatusCode::NonFinite);   // NaN
    CHECK(withFloat(1, 0x7F800000).code == WireStatusCode::NonFinite);   // +inf
    CHECK(withFloat(2, 0xFF800000).code == WireStatusCode::NonFinite);   // -inf
    CHECK(withFloat(6, 0x7FC00001).code == WireStatusCode::NonFinite);   // NaN in the rotation
    CHECK(withFloat(0, std::bit_cast<uint32_t>(40000.0f)).code == WireStatusCode::OutOfRange);
    CHECK(withFloat(7, std::bit_cast<uint32_t>(600.0f)).code == WireStatusCode::OutOfRange);   // linear velocity
    CHECK(withFloat(10, std::bit_cast<uint32_t>(1100.0f)).code == WireStatusCode::OutOfRange);   // angular velocity
    CHECK(withFloat(6, std::bit_cast<uint32_t>(0.5f)).code == WireStatusCode::Invalid);   // short quaternion
    CHECK(bool(withFloat(6, std::bit_cast<uint32_t>(1.005f))));   // within 0.99..1.01
    CHECK(withFloat(6, std::bit_cast<uint32_t>(1.02f)).code == WireStatusCode::OutOfRange);
    {
        std::vector<uint8_t> b = pose;
        b[52] = 8;   // undefined flag bit
        PoseState out;
        CHECK(decodePose(b, out).code == WireStatusCode::OutOfRange);
        b.push_back(0);
        CHECK(decodePose(b, out).code == WireStatusCode::TrailingBytes);
        b.resize(52);
        CHECK(decodePose(b, out).code == WireStatusCode::Truncated);
    }

    PoseState p = goldenPose();
    p.rotation = {0, 0, 0, 1.005f};
    CHECK(std::fabs(p.normalizedRotation().w - 1.0f) < 1e-6f);
    p.position.x = std::numeric_limits<float>::quiet_NaN();
    std::vector<uint8_t> out(64);
    uint32_t written;
    CHECK(encodePose(p, out, written).code == WireStatusCode::NonFinite);
    p = goldenPose();
    p.rotation = {0, 0, 0, 0.5f};
    CHECK(encodePose(p, out, written).code == WireStatusCode::Invalid);
}

static void testEndian() {
    uint8_t buffer[64];
    WireWriter w(buffer);
    CHECK(w.u8(0xAB));
    CHECK(w.u16(0x1234));
    CHECK(w.u32(0x89ABCDEF));
    CHECK(w.u64(0x0123456789ABCDEFull));
    CHECK(w.i16(-2));
    CHECK(w.i32(-1));
    CHECK(w.f32(1.0f));
    CHECK(w.f32(-0.0f));
    CHECK(!w.f32(std::numeric_limits<float>::infinity()));
    CHECK(w.error() == WireError::NonFinite);
    const uint8_t expected[] = {0xAB, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x89, 0xEF, 0xCD, 0xAB,
                                0x89, 0x67, 0x45, 0x23, 0x01, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF,
                                0xFF, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x80};
    CHECK(w.size() == sizeof(expected));
    CHECK(std::memcmp(buffer, expected, sizeof(expected)) == 0);

    WireReader r(std::span<const uint8_t>(expected, sizeof(expected)));
    uint8_t a;
    uint16_t b;
    uint32_t c;
    uint64_t d;
    int16_t e;
    int32_t f;
    float g, h;
    CHECK(r.u8(a) && a == 0xAB);
    CHECK(r.u16(b) && b == 0x1234);
    CHECK(r.u32(c) && c == 0x89ABCDEF);
    CHECK(r.u64(d) && d == 0x0123456789ABCDEFull);
    CHECK(r.i16(e) && e == -2);
    CHECK(r.i32(f) && f == -1);
    CHECK(r.f32(g) && g == 1.0f);
    CHECK(r.f32(h) && h == 0.0f && std::signbit(h));
    CHECK(r.remaining() == 0);
    // A failed read doesn't move.
    CHECK(!r.u8(a));
    CHECK(r.error() == WireError::Truncated && r.offset() == sizeof(expected));
    WireReader partial(std::span<const uint8_t>(expected, 3));
    CHECK(partial.u8(a));
    CHECK(!partial.u32(c));
    CHECK(partial.offset() == 1);
    CHECK(partial.u16(b) && b == 0x1234);

    // A full writer stays failed.
    uint8_t small[3];
    WireWriter tiny(small);
    CHECK(tiny.u16(1));
    CHECK(!tiny.u16(2));
    CHECK(tiny.error() == WireError::Overflow);
    CHECK(!tiny.u8(3));
    CHECK(tiny.size() == 2);
}

static void testWraparound() {
    CHECK(tickNewer(1, 0));
    CHECK(!tickNewer(0, 1));
    CHECK(!tickNewer(5, 5));
    CHECK(tickNewer(0, 0xFFFFFFFFu));
    CHECK(!tickNewer(0xFFFFFFFFu, 0));
    CHECK(tickNewer(10, 0xFFFFFFF0u));
    CHECK(tickNewer(0x7FFFFFFFu, 0));
    CHECK(!tickNewer(0x80000000u, 0));   // exactly half the range away: not newer either way
    CHECK(!tickNewer(0, 0x80000000u));
    CHECK(tickNewer(0x80000000u, 1));
    // Across the wrap, a run of ticks keeps getting newer.
    uint32_t tick = 0xFFFFFF00u;
    for (int i = 0; i < 512; ++i, ++tick) CHECK(tickNewer(tick + 1, tick));
}

static void testAim() {
    CHECK(encodeYaw(0.0f) == 32768);
    CHECK(encodeYaw(float(-std::numbers::pi)) == 0);
    CHECK(encodeYaw(float(std::numbers::pi)) == 0);   // pi wraps to -pi
    CHECK(std::fabs(decodeYaw(0) + float(std::numbers::pi)) < 1e-6f);
    CHECK(std::fabs(decodeYaw(32768)) < 1e-6f);
    for (uint32_t yaw = 0; yaw < 65536; yaw += 97) CHECK(encodeYaw(decodeYaw(uint16_t(yaw))) == yaw);
    const float maxPitch = float(89.0 * std::numbers::pi / 180.0);
    CHECK(encodePitch(maxPitch) == 32767);
    CHECK(encodePitch(-maxPitch) == -32767);
    CHECK(encodePitch(2.0f) == 32767);   // clamped
    CHECK(encodePitch(0.0f) == 0);
    CHECK(std::fabs(decodePitch(32767) - maxPitch) < 1e-6f);
    CHECK(encodeYaw(std::numeric_limits<float>::quiet_NaN()) == 32768);
    CHECK(encodePitch(std::numeric_limits<float>::infinity()) == 0);
}

static void testNoAllocation() {
    std::vector<std::vector<uint8_t>> golden;
    for (const GoldenMessage& g : kGolden) golden.push_back(unhex(g.hex));
    std::vector<uint8_t> out(kMaxReliableMessageBytes);
    const uint64_t before = g_allocations.load();
    for (int round = 0; round < 100; ++round) {
        for (size_t i = 0; i < golden.size(); ++i) {
            DecodedMessage m;
            const WireStatus status = decodeMessage(golden[i], optionsFor(MessageKind(i + 1)), m);
            uint32_t written = 0;
            const WireStatus again = encodeMessage(m.body, m.header.session, m.header.senderTick, out, written);
            if (!status || !again) ++failures;
        }
    }
    const uint64_t allocations = g_allocations.load() - before;
    if (allocations)
        std::fprintf(stderr, "  %llu allocations while encoding and decoding\n", (unsigned long long)allocations);
    CHECK(allocations == 0);
}

// Bounded random input and mutated valid messages: nothing crashes or reads out of
// bounds (run under sanitizers to see that), nothing allocates, and anything accepted
// re-encodes to exactly the bytes it came from.
static void testFuzz() {
    DeterministicRandom random(123);
    std::vector<std::vector<uint8_t>> golden;
    for (const GoldenMessage& g : kGolden) golden.push_back(unhex(g.hex));
    std::vector<uint8_t> input(kMaxReliableMessageBytes + 64);
    std::vector<uint8_t> out(kMaxReliableMessageBytes);
    uint64_t accepted = 0;
    const uint64_t before = g_allocations.load();
    for (int round = 0; round < 200000; ++round) {
        size_t size;
        if (round % 4 == 0) {
            // Random bytes behind a plausible header.
            size = size_t(random.range(0, 1300));
            for (size_t i = 0; i < size; ++i) input[i] = uint8_t(random.next());
            if (size >= kHeaderBytes && random.unit() < 0.9) {
                std::memcpy(input.data(), "SWMP", 4);
                storeU16(input.data() + 4, 1);
                input[6] = uint8_t(random.range(0, 23));
                input[7] = random.unit() < 0.9 ? 0 : uint8_t(random.next());
                storeU32(input.data() + 28, uint32_t(size - kHeaderBytes));
            }
        } else {
            // A valid message with a few bytes changed, cut, or grown.
            const std::vector<uint8_t>& base = golden[size_t(random.range(0, int64_t(golden.size()) - 1))];
            size = base.size();
            std::memcpy(input.data(), base.data(), size);
            const int edits = int(random.range(1, 4));
            for (int e = 0; e < edits; ++e) {
                const double what = random.unit();
                if (what < 0.6 && size > 0) {
                    input[size_t(random.range(0, int64_t(size) - 1))] = uint8_t(random.next());
                } else if (what < 0.8 && size > 0) {
                    size = size_t(random.range(0, int64_t(size) - 1));
                } else if (size < input.size() - 8) {
                    const size_t grow = size_t(random.range(1, 8));
                    for (size_t i = 0; i < grow; ++i) input[size + i] = uint8_t(random.next());
                    size += grow;
                }
            }
            if (random.unit() < 0.7 && size >= kHeaderBytes) storeU32(input.data() + 28, uint32_t(size - kHeaderBytes));
        }
        const std::span<const uint8_t> bytes(input.data(), size);
        DecodeOptions options;
        options.checkDelivery = random.unit() < 0.5;
        options.delivery = random.unit() < 0.5 ? Delivery::ReliableOrdered : Delivery::Unreliable;
        options.checkDirection = random.unit() < 0.5;
        options.receiver = random.unit() < 0.5 ? PeerRole::Host : PeerRole::Client;
        DecodedMessage m;
        if (!decodeMessage(bytes, options, m)) continue;
        ++accepted;
        uint32_t written = 0;
        const WireStatus again = encodeMessage(m.body, m.header.session, m.header.senderTick, out, written);
        if (!again || written != size || std::memcmp(out.data(), bytes.data(), size) != 0) {
            std::fprintf(stderr, "  fuzz round %d: accepted message didn't re-encode identically\n", round);
            ++failures;
        }
    }
    const uint64_t allocations = g_allocations.load() - before;
    CHECK(allocations == 0);
    CHECK(accepted > 1000);   // the mutations do reach the field decoders
}

static std::string normalized(std::string text) {
    std::string out;
    for (const char c : text)
        if (c != '\r') out += c;
    return out;
}

int main(int argc, char** argv) {
    // network_wire_test --write-reference docs/NetworkProtocol.md regenerates the reference.
    if (argc == 3 && std::string(argv[1]) == "--write-reference") {
        std::ofstream file(argv[2], std::ios::binary);
        file << protocolReferenceMarkdown();
        return file ? 0 : 1;
    }

    buildRecords();
    testGolden();
    testHeaderLayout();
    testInputCommandLayout();
    testTruncation();
    testBadHeaders();
    testDeliveryAndDirection();
    testRanges();
    testStrings();
    testFloatsAndPose();
    testEndian();
    testWraparound();
    testAim();
    testNoAllocation();
    testFuzz();

    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        if (!file || normalized(text.str()) != protocolReferenceMarkdown()) {
            std::fprintf(
                stderr, "%s is out of date; regenerate it with\n  network_wire_test --write-reference %s\n", argv[1],
                argv[1]);
            ++failures;
        }
    }

    if (failures) {
        std::fprintf(stderr, "network_wire: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("network_wire: all checks passed\n");
    return 0;
}
