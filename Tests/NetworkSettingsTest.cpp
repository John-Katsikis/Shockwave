// Multiplayer configuration (Batch 2 of docs/SteamMultiplayerImplementation.md):
// Config/Network.swnetwork parsing, writing and every range (tick_rate 60..120, max_players
// 2..64); the network manifest; SHA-256 and the content digest against independently
// computed values; the replication registry (ranges, duplicates, freeze, lookups) and its
// generic codec against the wire module's pose encoding; scene rules; readyForSession
// failing before any transport exists; and no allocation in capture, encode, decode,
// apply or lookups once frozen. No socket, Steam or display.
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkManifest.h>
#include <Multiplayer/NetworkSchema.h>
#include <Multiplayer/NetworkSettings.h>
#include <Multiplayer/NetworkValidation.h>
#include <Multiplayer/Replication.h>

#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Engine/PhysicsComponents.h>
#include <Network/Messages.h>
#include <Network/Sha256.h>
#include <Projects/Project.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <vector>

using namespace shockwave;
using namespace shockwave::net;
namespace fs = std::filesystem;

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

namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool anyProblem(const std::vector<NetProblem>& problems, const std::string& part, bool warning = false) {
    for (const NetProblem& p : problems)
        if (p.warning == warning && contains(p.text, part)) return true;
    return false;
}

void dump(const std::vector<NetProblem>& problems) {
    for (const NetProblem& p : problems) std::fprintf(stderr, "  %s%s\n", p.warning ? "warning " : "", p.text.c_str());
}

// Parses `body` after the header line; returns the error ("" when it parsed).
std::string parseError(const std::string& body, NetworkSettingsFile* out = nullptr) {
    NetworkSettingsFile file;
    std::string error;
    if (parseNetworkSettings("shockwave_network 1\n" + body, "Network.swnetwork", file, error)) {
        if (out) *out = file;
        return "";
    }
    return error;
}

void writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// ---- Settings ---------------------------------------------------------------------------

void testSettingsRoundTrip() {
    NetworkSettings s;
    s.enabled = true;
    s.transport = NetTransportKind::Loopback;
    s.steamAppId = 4294967295u;
    s.lobbyVisibility = LobbyVisibility::Public;
    s.maxPlayers = 64;
    s.tickRate = 120;
    s.inputRate = 120;
    s.snapshotRate = 30;
    s.connectTimeoutSeconds = 2.5;
    s.manifest = "Config/Other Name.swnetmanifest";
    const std::string text = networkSettingsText(s);
    NetworkSettingsFile read;
    std::string error;
    CHECK(parseNetworkSettings(text, "x", read, error));
    CHECK(read.settings == s);
    CHECK(networkSettingsText(read.settings) == text);
    CHECK(read.line("tick_rate") > 1);
    CHECK(read.line("nonexistent") == 0);

    // Defaults: an empty file is the documented defaults, and they validate.
    NetworkSettingsFile defaults;
    CHECK(parseError("", &defaults).empty());
    CHECK(defaults.settings == NetworkSettings{});
    std::vector<NetProblem> problems;
    validateNetworkSettings(defaults.settings, 60, "x", nullptr, problems);
    CHECK(!hasErrors(problems));

    // Comments, blank lines, CRLF and # inside quotes.
    NetworkSettingsFile commented;
    CHECK(parseError(
              "# a comment\r\n\r\nmax_players 4   # trailing\r\nmanifest \"Config/a#b.swnetmanifest\"\r\n", &commented)
              .empty());
    CHECK(commented.settings.maxPlayers == 4);
    CHECK(commented.settings.manifest == "Config/a#b.swnetmanifest");
    CHECK(commented.line("max_players") == 4);

    // Atomic write, read back.
    const fs::path dir = fs::temp_directory_path() / "shockwave_network_settings_test";
    fs::remove_all(dir);
    const fs::path file = dir / "Config" / "Network.swnetwork";
    CHECK(writeNetworkSettings(file, s, error));
    NetworkSettingsFile again;
    CHECK(readNetworkSettings(file, again, error));
    CHECK(again.settings == s);
    for (const auto& entry : fs::directory_iterator(file.parent_path()))
        CHECK(entry.path().extension() != ".tmp");   // no temporary left behind
    // An invalid value is refused and the file is untouched.
    NetworkSettings bad = s;
    bad.maxPlayers = 65;
    CHECK(!writeNetworkSettings(file, bad, error));
    CHECK(contains(error, "max_players"));
    CHECK(readNetworkSettings(file, again, error) && again.settings == s);
    fs::remove_all(dir);
}

void testSettingsRanges() {
    // Override 1: tick_rate 60..120, input_rate too.
    for (const char* ok : {"tick_rate 60", "tick_rate 61", "tick_rate 90", "tick_rate 120", "input_rate 120"})
        CHECK(parseError(std::string(ok) + "\n").empty());
    for (const char* bad : {"tick_rate 59", "tick_rate 121", "tick_rate 0", "tick_rate -60", "tick_rate 60.5"})
        CHECK(contains(parseError(std::string(bad) + "\n"), "Network.swnetwork:2: tick_rate must be"));
    // Override 2: max_players 2..64.
    for (int players : {2, 3, 12, 33, 64}) CHECK(parseError("max_players " + std::to_string(players) + "\n").empty());
    for (const char* bad : {"max_players 1", "max_players 0", "max_players 65", "max_players 1000"})
        CHECK(contains(parseError(std::string(bad) + "\n"), "max_players must be a whole number from 2 to 64"));
    // The rest of the table's numeric ranges, both ends.
    struct Range {
        const char* key;
        long long min, max;
    };
    for (const Range r :
         {Range{"snapshot_rate", 10, 120}, Range{"virtual_port", 0, 999}, Range{"interpolation_ms", 25, 200},
          Range{"max_extrapolation_ms", 0, 100}, Range{"max_rewind_ms", 0, 250},
          Range{"max_replicated_entities", 12, 4096}, Range{"max_snapshot_bytes", 512, 1152},
          Range{"outbound_bytes_per_second", 16384, 262144}, Range{"max_pending_reliable_bytes", 16384, 262144},
          Range{"max_receive_bytes_per_pump", 16384, 1048576}, Range{"max_receive_messages_per_pump", 16, 1024},
          Range{"connect_timeout_seconds", 1, 120}, Range{"handshake_timeout_seconds", 1, 120},
          Range{"idle_timeout_seconds", 1, 120}, Range{"steam_app_id", 0, 4294967295LL}}) {
        const std::string key = r.key;
        CHECK(parseError(key + " " + std::to_string(r.min) + "\n").empty());
        CHECK(parseError(key + " " + std::to_string(r.max) + "\n").empty());
        if (r.min > 0) CHECK(!parseError(key + " " + std::to_string(r.min - 1) + "\n").empty());
        CHECK(!parseError(key + " " + std::to_string(r.max + 1) + "\n").empty());
    }
    // Non-finite and malformed numbers are errors, never clamped.
    for (const char* bad :
         {"connect_timeout_seconds nan", "connect_timeout_seconds inf", "idle_timeout_seconds -inf",
          "idle_timeout_seconds 1e400", "max_players 12abc", "max_players", "max_players 12 13", "enabled 2",
          "enabled yes", "transport udp", "lobby_visibility everyone", "manifest Config/x.swnetmanifest",
          "manifest \"Config/x.txt\"", "manifest \"../x.swnetmanifest\"", "manifest \"/abs.swnetmanifest\"",
          "manifest \"Config\\\\x.swnetmanifest\"", "manifest \"C:/x.swnetmanifest\"", "manifest \"\""})
        CHECK(!parseError(std::string(bad) + "\n").empty());
    // Unknown and duplicate keys say where.
    CHECK(contains(parseError("\n\nbogus 1\n"), "Network.swnetwork:4: unknown setting bogus"));
    const std::string duplicate = parseError("max_players 4\nmax_players 5\n");
    CHECK(contains(duplicate, ":3: duplicate setting max_players (first on line 2)"));
    // The header.
    NetworkSettingsFile file;
    std::string error;
    CHECK(!parseNetworkSettings("max_players 4\n", "x", file, error));
    CHECK(!parseNetworkSettings("shockwave_network 2\n", "x", file, error));
    CHECK(!parseNetworkSettings("", "x", file, error));
    // Transactional: a failed parse leaves the output as it was.
    NetworkSettingsFile kept;
    kept.settings.maxPlayers = 7;
    CHECK(!parseNetworkSettings("shockwave_network 1\nmax_players 9\nbogus 1\n", "x", kept, error));
    CHECK(kept.settings.maxPlayers == 7);
    // setSettingText refuses without changing anything.
    NetworkSettings s;
    std::string why;
    CHECK(!setSettingText(s, *findNetworkSettingField("max_players"), "65", why) && s.maxPlayers == 12);
    CHECK(setSettingText(s, *findNetworkSettingField("max_players"), "64", why) && s.maxPlayers == 64);
    CHECK(findNetworkSettingField("bogus") == nullptr);
}

void testSettingsRules() {
    auto problems = [](const NetworkSettings& s, double projectTick = 60) {
        std::vector<NetProblem> out;
        validateNetworkSettings(s, projectTick, "N", nullptr, out);
        return out;
    };
    NetworkSettings s;
    s.tickRate = 120;
    CHECK(anyProblem(problems(s), "doesn't match the project's tick_rate 60"));
    CHECK(anyProblem(problems(s), "input_rate must equal tick_rate"));
    s.inputRate = 120;
    CHECK(!hasErrors(problems(s, 120)));
    s.snapshotRate = 25;
    CHECK(anyProblem(problems(s, 120), "snapshot_rate must be 10, 15, 20, 30, 60 or 120"));
    s.tickRate = s.inputRate = 90;
    s.snapshotRate = 20;   // doesn't divide 90
    CHECK(anyProblem(problems(s, 90), "divide"));
    s.snapshotRate = 30;
    CHECK(!hasErrors(problems(s, 90)));

    NetworkSettings steam;
    steam.enabled = true;
    CHECK(anyProblem(problems(steam), "steam_app_id is required"));
    steam.steamAppId = 480;
    CHECK(!hasErrors(problems(steam)));
    CHECK(anyProblem(problems(steam), "480", true));
    NetworkSettings loopback;
    loopback.enabled = true;
    loopback.transport = NetTransportKind::Loopback;
    CHECK(!hasErrors(problems(loopback)));
    loopback.steamRequired = true;
    CHECK(anyProblem(problems(loopback), "steam_required"));

    NetworkSettings entities;
    entities.maxPlayers = 64;
    entities.maxReplicatedEntities = 32;
    CHECK(anyProblem(problems(entities), "max_replicated_entities"));
    NetworkSettings rewind;
    rewind.maxRewindMs = 50;
    CHECK(!hasErrors(problems(rewind)));
    CHECK(anyProblem(problems(rewind), "max_rewind_ms", true));
    // Less than two snapshot intervals of interpolation: a warning; 120 Hz snapshots with
    // 25 ms (three ticks) are fine.
    NetworkSettings jitter;
    jitter.snapshotRate = 30;
    jitter.interpolationMs = 50;
    CHECK(!hasErrors(problems(jitter)));
    CHECK(anyProblem(problems(jitter), "less than two snapshot intervals at snapshot_rate 30 (67 ms)", true));
    jitter.tickRate = 120;
    jitter.snapshotRate = 120;
    jitter.interpolationMs = 25;
    CHECK(!anyProblem(problems(jitter), "two snapshot intervals", true));
    jitter.interpolationMs = 24;
    CHECK(anyProblem(problems(jitter), "interpolation_ms"));
}

// ---- SHA-256 and the content digest -----------------------------------------------------

void testSha256() {
    // FIPS 180-2 vectors.
    CHECK(toHex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(
        toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million a's, fed in uneven pieces across block boundaries.
    Sha256 hash;
    const std::string chunk(997, 'a');
    size_t fed = 0;
    while (fed < 1000000) {
        const size_t n = std::min(chunk.size(), size_t(1000000) - fed);
        hash.update(std::string_view(chunk.data(), n));
        fed += n;
    }
    CHECK(toHex(hash.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void testContentDigest() {
    // Golden value computed with Python's hashlib from the documented layout.
    const std::vector<ContentFile> files = {
        {"Scenes/s.swscene", "x\r\n"},
        {"Content/b.bin", std::string("\x00\x01\r\n", 4)},
        {"Content/a.txt", "hi\r\nthere\n"},
    };
    const std::string golden = "8b2ee475d6a4791b6b6f3a8442be33b3a3ea676fbf832411742d234a2967a00d";
    CHECK(toHex(contentDigestOf("test.v1", files)) == golden);
    // Order doesn't matter; CRLF in text files doesn't either; binary bytes and the
    // protocol name do.
    std::vector<ContentFile> lf = {files[2], files[0], files[1]};
    lf[0].bytes = "hi\nthere\n";
    lf[1].bytes = "x\n";
    CHECK(toHex(contentDigestOf("test.v1", lf)) == golden);
    std::vector<ContentFile> binary = files;
    binary[1].bytes = std::string("\x00\x01\n", 3);
    CHECK(toHex(contentDigestOf("test.v1", binary)) != golden);
    CHECK(toHex(contentDigestOf("test.v2", files)) != golden);
    CHECK(contentTextFile("a.swscene") && contentTextFile("a.swprefab") && !contentTextFile("a.png"));
}

// ---- The manifest -----------------------------------------------------------------------

struct TempProject {
    fs::path dir;
    Project project;
    explicit TempProject(const char* name) {
        dir = fs::temp_directory_path() / name;
        fs::remove_all(dir);
        project.root = dir;
        project.file = dir / "Test.swproject";
        project.assetFolder = dir / "Content";
        project.sceneFolder = dir / "Scenes";
        writeFile(project.file, "shockwave_project 1\n");
        writeFile(dir / "Scenes" / "a.swscene", "shockwave_scene 1\r\n");
        writeFile(dir / "Scenes" / "b.swscene", "shockwave_scene 1\n");
        writeFile(dir / "Content" / "Prefabs" / "P.swprefab", "shockwave_prefab 1\n");
        writeFile(dir / "Content" / ".hidden", "skipped");
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void testManifest() {
    const std::string text =
        "shockwave_network_manifest 1\r\n# comment\r\ngame_protocol \"test.v1\"\r\nscene 2 \"Scenes/b.swscene\"\r\n"
        "scene 1 \"Scenes/a.swscene\"\r\nprefab 7 \"Prefabs/P.swprefab\" \"engine.pose.v1\"\r\n";
    NetworkManifest m;
    std::string error;
    CHECK(parseNetworkManifest(text, "M", m, error));
    CHECK(m.gameProtocol == "test.v1" && m.scenes.size() == 2 && m.prefabs.size() == 1);
    CHECK(m.scene(1) && m.scene(1)->path == "Scenes/a.swscene" && m.scene(1)->line == 5);
    CHECK(m.prefab(7) && m.prefab(7)->profile == "engine.pose.v1");
    CHECK(!m.scene(3));
    // Canonical text sorts by ID and parses back to the same.
    NetworkManifest again;
    CHECK(parseNetworkManifest(networkManifestText(m), "M", again, error));
    CHECK(networkManifestText(again) == networkManifestText(m));
    CHECK(networkManifestText(m).find("scene 1") < networkManifestText(m).find("scene 2"));

    for (const char* bad :
         {"game_protocol \"Test\"\n", "game_protocol test\n", "scene 0 \"Scenes/a.swscene\"\n",
          "scene 65536 \"Scenes/a.swscene\"\n", "scene 1\n", "prefab 1 \"P.swprefab\"\n", "bogus 1\n",
          "game_protocol \"a\"\ngame_protocol \"b\"\n"}) {
        NetworkManifest kept;
        kept.gameProtocol = "kept";
        CHECK(!parseNetworkManifest(std::string("shockwave_network_manifest 1\n") + bad, "M", kept, error));
        CHECK(kept.gameProtocol == "kept");   // transactional
        CHECK(error.rfind("M:", 0) == 0);
    }

    TempProject temp("shockwave_network_manifest_test");
    ReplicationRegistry registry;
    registerEngineReplication(registry);
    std::vector<std::string> freezeProblems;
    CHECK(registry.freeze(freezeProblems));
    std::vector<NetProblem> problems;
    validateNetworkManifest(m, temp.project, &registry, "M", problems);
    if (!problems.empty()) dump(problems);
    CHECK(problems.empty());

    NetworkManifest broken = m;
    broken.scenes.push_back({1, "Scenes/a.swscene", 9});   // duplicate ID and path
    broken.scenes.push_back({3, "Scenes/missing.swscene", 10});
    broken.scenes.push_back({4, "../outside.swscene", 11});
    broken.prefabs.push_back({8, "Prefabs/P.swprefab", "game.unknown.v1", 12});
    broken.gameProtocol.clear();
    problems.clear();
    validateNetworkManifest(broken, temp.project, &registry, "M", problems);
    CHECK(anyProblem(problems, "M:9: scene ID 1 is already used on line 5"));
    CHECK(anyProblem(problems, "is listed twice"));
    CHECK(anyProblem(problems, "M:10: scene Scenes/missing.swscene doesn't exist"));
    CHECK(anyProblem(problems, "M:11: scene path"));
    CHECK(anyProblem(problems, "profile game.unknown.v1 isn't registered"));
    CHECK(anyProblem(problems, "no game_protocol"));

    // The content digest of a project: every Content file but dotfiles, the manifest's
    // scenes, and the manifest; a CRLF checkout gives the same digest.
    writeFile(temp.dir / "Config" / "NetworkManifest.swnetmanifest", text);
    ContentDigest digest;
    problems.clear();
    CHECK(computeContentDigest(temp.project, m, "Config/NetworkManifest.swnetmanifest", digest, problems));
    CHECK(problems.empty());
    CHECK(digest.files.size() == 4);
    for (const std::string& f : digest.files) CHECK(!contains(f, ".hidden"));
    writeFile(temp.dir / "Scenes" / "a.swscene", "shockwave_scene 1\n");   // LF instead of CRLF
    ContentDigest lf;
    CHECK(computeContentDigest(temp.project, m, "Config/NetworkManifest.swnetmanifest", lf, problems));
    CHECK(lf.digest == digest.digest);
    writeFile(temp.dir / "Content" / "Prefabs" / "P.swprefab", "shockwave_prefab 1\nentity \"x\"\nend\n");
    ContentDigest changed;
    CHECK(computeContentDigest(temp.project, m, "Config/NetworkManifest.swnetmanifest", changed, problems));
    CHECK(changed.digest != digest.digest);
#ifndef _WIN32
    std::error_code ec;
    fs::create_symlink(temp.dir / "Scenes" / "a.swscene", temp.dir / "Content" / "link.swscene", ec);
    if (!ec) {
        ContentDigest linked;
        problems.clear();
        CHECK(!computeContentDigest(temp.project, m, "Config/NetworkManifest.swnetmanifest", linked, problems));
        CHECK(anyProblem(problems, "link"));
    }
#endif
}

// ---- The registry ------------------------------------------------------------------------

ReplicationProfile gameProfile(uint16_t id, std::string name) {
    ReplicationProfile p;
    p.id = id;
    p.name = std::move(name);
    p.fields = {netInt("score", NetFieldType::U16, 0, 1000, NetFieldRole::Score)};
    p.requiredComponents = {"Transform"};
    p.capture = [](const World&, Entity, const ReplicationContext&, ProfileValue& out) -> const char* {
        out.fields[0].u = 1;
        return nullptr;
    };
    p.apply = [](World&, Entity, const ReplicationContext&, const ProfileValue&) {};
    return p;
}

void testRegistry() {
    std::string why;
    std::vector<std::string> problems;
    {
        ReplicationRegistry r;
        registerEngineReplication(r);
        CHECK(r.addProfile(gameProfile(32, "game.thing.v1"), why));
        CHECK(!r.addProfile(gameProfile(32, "game.other.v1"), why) && contains(why, "32"));   // duplicate ID
        CHECK(!r.addProfile(gameProfile(40, "game.thing.v1"), why));   // duplicate name
        CHECK(!r.profile(1));   // lookups wait for freeze
        CHECK(!r.freeze(problems));   // the rejected registrations fail it
        CHECK(!r.frozen());
    }
    {
        ReplicationRegistry r;
        CHECK(!r.addProfile(gameProfile(5, "game.thing.v1"), why));   // engine range
        CHECK(!r.addProfile(gameProfile(1024, "game.thing.v1"), why));
        CHECK(!r.addProfile(gameProfile(0, "game.thing.v1"), why));
    }
    {
        // Invalid metadata fails freeze, each with a reason.
        ReplicationRegistry r;
        ReplicationProfile noCapture = gameProfile(32, "game.a.v1");
        noCapture.capture = nullptr;
        r.addProfile(std::move(noCapture), why);
        ReplicationProfile badName = gameProfile(33, "Game A");
        r.addProfile(std::move(badName), why);
        ReplicationProfile badRange = gameProfile(34, "game.c.v1");
        badRange.fields = {netInt("x", NetFieldType::U8, 0, 300)};
        r.addProfile(std::move(badRange), why);
        ReplicationProfile duplicateField = gameProfile(35, "game.d.v1");
        duplicateField.fields.push_back(duplicateField.fields[0]);
        r.addProfile(std::move(duplicateField), why);
        ReplicationProfile tooMany = gameProfile(36, "game.e.v1");
        tooMany.fields.clear();
        for (int i = 0; i < 33; ++i) tooMany.fields.push_back(netInt("f" + std::to_string(i), NetFieldType::U8, 0, 1));
        r.addProfile(std::move(tooMany), why);
        CommandDesc fast;
        fast.id = 32;
        fast.name = "game.fast";
        fast.ratePerSecond = 21;
        r.addCommand(std::move(fast), why);
        CommandDesc target;
        target.id = 33;
        target.name = "game.target";
        target.targetProfiles = {"game.missing.v1"};
        r.addCommand(std::move(target), why);
        EventDesc engineEvent;
        engineEvent.id = 32;
        engineEvent.name = "engine.not_ours";
        r.addEvent(std::move(engineEvent), why);
        problems.clear();
        CHECK(!r.freeze(problems));
        CHECK(problems.size() >= 8);
        if (problems.size() < 8)
            for (const std::string& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
    }
    // The real catalog: engine plus a game's types, frozen, looked up, described.
    ReplicationRegistry r;
    registerEngineReplication(r);
    CHECK(r.addProfile(gameProfile(32, "game.thing.v1"), why));
    CommandDesc team;
    team.id = 32;
    team.name = "game.request_team";
    team.payload = {netInt("team", NetFieldType::U8, 0, 1, NetFieldRole::Team)};
    CHECK(r.addCommand(std::move(team), why));
    EventDesc hit;
    hit.id = 32;
    hit.name = "game.hit";
    hit.payload = {netVec3("at", 100, NetFieldRole::Position)};
    CHECK(r.addEvent(std::move(hit), why));
    problems.clear();
    CHECK(r.freeze(problems));
    for (const std::string& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
    CHECK(r.frozen());
    CHECK(!r.addProfile(gameProfile(40, "game.late.v1"), why));   // frozen for good
    CHECK(r.profile(1) && r.profile(1)->name == "engine.pose.v1" && r.profile(1)->encodedBytes == 53);
    CHECK(r.profile(2) && r.profile(2)->name == "engine.rigidbody.v1");
    CHECK(r.profile("game.thing.v1") && r.profile("game.thing.v1")->encodedBytes == 2);
    CHECK(!r.profile(3) && !r.profile(1023) && !r.profile(uint16_t(60000)) && !r.profile("nope"));
    CHECK(r.command(32) && r.command(32)->payloadBytes == 1 && !r.command(33) && !r.command(1));
    CHECK(r.event(32) && r.event(32)->payloadBytes == 12 && !r.event(31));
    // The digest follows the catalog.
    ReplicationRegistry same;
    registerEngineReplication(same);
    CHECK(same.addProfile(gameProfile(32, "game.thing.v1"), why));
    CommandDesc team2;
    team2.id = 32;
    team2.name = "game.request_team";
    team2.payload = {netInt("team", NetFieldType::U8, 0, 1, NetFieldRole::Team)};
    CHECK(same.addCommand(std::move(team2), why));
    EventDesc hit2;
    hit2.id = 32;
    hit2.name = "game.hit";
    hit2.payload = {netVec3("at", 100, NetFieldRole::Position)};
    CHECK(same.addEvent(std::move(hit2), why));
    CHECK(same.freeze(problems));
    CHECK(same.schemaDigest() == r.schemaDigest());
    CHECK(toHex(sha256(r.catalogText())) == toHex(r.schemaDigest()));
    ReplicationRegistry engineOnly;
    registerEngineReplication(engineOnly);
    CHECK(engineOnly.freeze(problems));
    CHECK(engineOnly.schemaDigest() != r.schemaDigest());
    CHECK(contains(networkSchemaMarkdown(r, "test"), "game.thing.v1"));
}

void testCodec() {
    ReplicationRegistry r;
    registerEngineReplication(r);
    std::vector<std::string> problems;
    CHECK(r.freeze(problems));
    const ReplicationProfile& pose = *r.profile(1);
    // The generic codec lays engine.pose.v1 out exactly like the wire module's encodePose.
    PoseState state;
    state.position = {1.5f, -2.25f, 300.0f};
    state.rotation = {0.0f, 0.70710678f, 0.0f, 0.70710678f};
    state.linearVelocity = {3, 0, -4};
    state.angularVelocity = {0, 1, 0};
    state.flags = PoseState::kTeleport;
    std::array<uint8_t, 64> wire{}, generic{};
    uint32_t wireBytes = 0, genericBytes = 0;
    CHECK(bool(encodePose(state, wire, wireBytes)));
    ProfileValue value;
    setVec3(value.fields[0], 1.5f, -2.25f, 300.0f);
    setQuat(value.fields[1], 0.0f, 0.70710678f, 0.0f, 0.70710678f);
    setVec3(value.fields[2], 3, 0, -4);
    setVec3(value.fields[3], 0, 1, 0);
    value.fields[4].u = PoseState::kTeleport;
    CHECK(bool(encodeProfile(pose, value, generic, genericBytes)));
    CHECK(wireBytes == 53 && genericBytes == 53);
    CHECK(std::equal(wire.begin(), wire.begin() + 53, generic.begin()));
    ProfileValue decoded;
    CHECK(bool(decodeProfile(pose, std::span(generic.data(), 53), decoded)));
    CHECK(decoded.fields[0].f == value.fields[0].f && decoded.fields[4].u == value.fields[4].u);

    // Out of range is an error, never clamped; a failed decode leaves the output alone.
    ProfileValue bad = value;
    bad.fields[0].f[0] = 40000.0f;
    CHECK(encodeProfile(pose, bad, generic, genericBytes).code == WireStatusCode::OutOfRange);
    bad = value;
    bad.fields[2].f[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(encodeProfile(pose, bad, generic, genericBytes).code == WireStatusCode::NonFinite);
    bad = value;
    setQuat(bad.fields[1], 0, 0, 0, 0.5f);
    CHECK(!encodeProfile(pose, bad, generic, genericBytes));
    bad = value;
    bad.fields[4].u = 4;   // not in the flags mask
    CHECK(!encodeProfile(pose, bad, generic, genericBytes));
    std::array<uint8_t, 8> small{};
    CHECK(encodeProfile(pose, value, small, genericBytes).code == WireStatusCode::Overflow);
    CHECK(!decodeProfile(pose, std::span(generic.data(), 52), decoded));
    std::array<uint8_t, 54> longer{};
    std::copy(wire.begin(), wire.begin() + 53, longer.begin());
    CHECK(decodeProfile(pose, longer, decoded).code == WireStatusCode::TrailingBytes);
    std::array<uint8_t, 53> nan = {};
    std::copy(wire.begin(), wire.begin() + 53, nan.begin());
    const uint32_t nanBits = 0x7fc00000u;
    std::memcpy(nan.data(), &nanBits, 4);
    ProfileValue untouched;
    untouched.fields[4].u = 99;
    CHECK(!decodeProfile(pose, nan, untouched));
    CHECK(untouched.fields[4].u == 99);

    // Signed, wide and bool fields round trip.
    const std::vector<NetFieldDesc> fields = {
        netInt("a", NetFieldType::I8, -128, 127),   netInt("b", NetFieldType::I16, -300, 300),
        netInt("c", NetFieldType::I32, -100000, 5), netInt("d", NetFieldType::U64, 0, 18446744073709551615.0),
        netInt("e", NetFieldType::Bool, 0, 1),      netFloat("f", 10),
    };
    ProfileValue v;
    v.fields[0].u = uint64_t(int64_t(-128));
    v.fields[1].u = uint64_t(int64_t(-300));
    v.fields[2].u = uint64_t(int64_t(-99999));
    v.fields[3].u = 18446744073709551615ull;
    v.fields[4].u = 1;
    v.fields[5].f[0] = -9.5f;
    std::array<uint8_t, 32> out{};
    uint32_t written = 0;
    CHECK(bool(encodeFields(fields, v, out, written)) && written == 1 + 2 + 4 + 8 + 1 + 4);
    ProfileValue back;
    CHECK(bool(decodeFields(fields, std::span(out.data(), written), back)));
    for (int i = 0; i < 5; ++i) CHECK(back.fields[size_t(i)].u == v.fields[size_t(i)].u);
    CHECK(back.fields[5].f[0] == -9.5f);
    v.fields[1].u = uint64_t(int64_t(-301));
    CHECK(!encodeFields(fields, v, out, written));
}

// ---- Scenes and sessions ----------------------------------------------------------------

void testWorldRules() {
    registerNetworkComponents();
    ReplicationRegistry r;
    registerEngineReplication(r);
    std::vector<std::string> freezeProblems;
    CHECK(r.freeze(freezeProblems));

    World world;
    auto object = [&](const char* name, std::string key, const char* profile) {
        const Entity e = world.create();
        world.add<Name>(e, {name});
        world.add<Transform>(e);
        NetworkObject o;
        o.key = std::move(key);
        o.profile = profile;
        world.add<NetworkObject>(e, o);
        return e;
    };
    const Entity a = object("A", "door-a", "engine.pose.v1");
    std::vector<NetProblem> problems;
    validateNetworkWorld(world, r, "S", false, problems);
    if (!problems.empty()) dump(problems);
    CHECK(problems.empty());

    object("B", "door-a", "engine.pose.v1");   // duplicate key
    object("C", "", "engine.pose.v1");   // no key
    object("D", "bad key!", "engine.pose.v1");
    object("E", "e", "game.missing.v1");
    const Entity body = object("F", "f", "engine.rigidbody.v1");   // no RigidBody or Collider
    const Entity child = object("G", "g", "engine.pose.v1");
    setParent(world, child, a);
    const Entity scaled = object("H", "h", "engine.pose.v1");
    world.get<Transform>(scaled)->scale = {2, 2, 2};
    const Entity predicted = object("I", "i", "engine.pose.v1");
    world.get<NetworkObject>(predicted)->mode = NetworkObject::Mode::PredictedCharacter;
    const Entity loose = world.create();   // a moving body without replication
    world.add<Name>(loose, {"Loose"});
    world.add<Transform>(loose);
    world.add<RigidBody>(loose);
    world.add<Collider>(loose);
    for (int i = 0; i < 2; ++i) {
        const Entity spawn = world.create();
        world.add<Name>(spawn, {"Spawn"});
        world.add<Transform>(spawn);
        world.add<NetworkSpawnPoint>(spawn, {1, 3});
    }
    problems.clear();
    validateNetworkWorld(world, r, "S", false, problems);
    CHECK(anyProblem(problems, "S: entity 'B': NetworkObject key \"door-a\" is also used by 'A'"));
    CHECK(anyProblem(problems, "entity 'C': NetworkObject has no key"));
    CHECK(anyProblem(problems, "entity 'D': NetworkObject key"));
    CHECK(anyProblem(problems, "entity 'E': NetworkObject profile \"game.missing.v1\" isn't registered"));
    CHECK(anyProblem(problems, "entity 'F': profile engine.rigidbody.v1 needs a RigidBody component"));
    CHECK(anyProblem(problems, "entity 'G': a NetworkObject must be a root"));
    CHECK(anyProblem(problems, "under another one"));
    CHECK(anyProblem(problems, "entity 'H': a NetworkObject's Transform must have scale 1 1 1"));
    CHECK(anyProblem(problems, "entity 'I': PredictedCharacter needs a CharacterController"));
    CHECK(anyProblem(problems, "entity 'Loose': a moving RigidBody in a network scene needs a NetworkObject"));
    CHECK(anyProblem(problems, "NetworkSpawnPoint team 1 order 3 is also"));
    (void)body;

    // A prefab may leave its key empty; scenes can't.
    World prefab;
    const Entity root = prefab.create();
    prefab.add<Transform>(root);
    prefab.add<NetworkObject>(root, NetworkObject{"", "", "engine.pose.v1"});
    problems.clear();
    validateNetworkWorld(prefab, r, "P", true, problems);
    CHECK(problems.empty());

    // Keys: valid, fresh on duplicates, filled in when missing.
    CHECK(validNetworkKey("a") && validNetworkKey("Door-1.west:2_b") && validNetworkKey(std::string(96, 'k')));
    CHECK(
        !validNetworkKey("") && !validNetworkKey(std::string(97, 'k')) && !validNetworkKey("a b") &&
        !validNetworkKey("a/b"));
    const std::string k1 = newNetworkKey(), k2 = newNetworkKey();
    CHECK(validNetworkKey(k1) && k1.size() == 36 && k1 != k2 && k1[14] == '4');
    const Entity copies[] = {a};
    const std::string before = world.get<NetworkObject>(a)->key;
    CHECK(renewNetworkKeys(world, copies) == 2);   // a and its network child G
    CHECK(world.get<NetworkObject>(a)->key != before && validNetworkKey(world.get<NetworkObject>(a)->key));
    const Entity unkeyed = world.create();
    world.add<NetworkObject>(unkeyed);
    CHECK(assignMissingNetworkKey(world, unkeyed) && validNetworkKey(world.get<NetworkObject>(unkeyed)->key));
    CHECK(!assignMissingNetworkKey(world, unkeyed));
}

void testReadyForSession() {
    TempProject temp("shockwave_network_ready_test");
    temp.project.tickRate = 60;
    ReplicationRegistry open;   // not frozen
    registerEngineReplication(open);
    NetworkProject network;
    std::vector<NetProblem> problems;
    CHECK(loadNetworkProject(temp.project, network, problems));
    CHECK(!network.present && problems.empty());   // no file: offline, no problem
    CHECK(!readyForSession(temp.project, network, open, 1, problems));
    CHECK(anyProblem(problems, "offline only"));

    writeFile(temp.dir / "Config" / "Network.swnetwork", "shockwave_network 1\nenabled 0\n");
    network = {};
    problems.clear();
    loadNetworkProject(temp.project, network, problems);
    CHECK(network.present && !network.manifest);
    CHECK(problems.empty());   // a missing manifest only matters once enabled
    problems.clear();
    CHECK(!readyForSession(temp.project, network, open, 1, problems));
    CHECK(anyProblem(problems, "isn't enabled"));
    CHECK(anyProblem(problems, "frozen"));
    CHECK(anyProblem(problems, "manifest is missing"));

    writeFile(
        temp.dir / "Config" / "Network.swnetwork",
        "shockwave_network 1\nenabled 1\ntransport loopback\nmax_players 64\noutbound_bytes_per_second 262144\n");
    writeFile(
        temp.dir / "Config" / "NetworkManifest.swnetmanifest",
        "shockwave_network_manifest 1\ngame_protocol \"test.v1\"\nscene 1 \"Scenes/a.swscene\"\n");
    ReplicationRegistry frozen;
    registerEngineReplication(frozen);
    std::vector<std::string> freezeProblems;
    CHECK(frozen.freeze(freezeProblems));
    network = {};
    problems.clear();
    CHECK(loadNetworkProject(temp.project, network, problems));
    CHECK(network.manifest.has_value());
    CHECK(readyForSession(temp.project, network, frozen, 1, problems));
    if (!problems.empty()) dump(problems);
    CHECK(!readyForSession(temp.project, network, frozen, 2, problems));
    CHECK(anyProblem(problems, "scene ID 2 isn't in the manifest"));

    // 64 players of a large per-player profile overflow the default bandwidth.
    ReplicationRegistry game;
    registerEngineReplication(game);
    ReplicationProfile avatar = gameProfile(32, "game.avatar.v1");
    avatar.fields = poseFields();
    for (int i = 0; i < 12; ++i)
        avatar.fields.push_back(netInt("x" + std::to_string(i), NetFieldType::U16, 0, 100, NetFieldRole::State));
    avatar.perPlayer = true;
    std::string why;
    CHECK(game.addProfile(std::move(avatar), why));
    CHECK(game.freeze(freezeProblems));
    NetworkSettings s;
    s.maxPlayers = 64;
    CHECK(estimatePlayerSnapshotBytesPerSecond(s, game) > s.outboundBytesPerSecond - 16384);
    CHECK(estimatePlayerSnapshotBytesPerSecond(s, frozen) == 0);
    writeFile(
        temp.dir / "Config" / "Network.swnetwork",
        "shockwave_network 1\nenabled 1\ntransport loopback\nmax_players 64\n");
    network = {};
    problems.clear();
    loadNetworkProject(temp.project, network, problems);
    validateNetworkProject(temp.project, network, game, problems);
    CHECK(anyProblem(problems, "Config/Network.swnetwork:4: 64 players at snapshot_rate 20 need about"));
    // A project tick rate other than the file's.
    temp.project.tickRate = 120;
    problems.clear();
    validateNetworkProject(temp.project, network, game, problems);
    CHECK(anyProblem(problems, "doesn't match the project's tick_rate 120"));
}

void testJson() {
    CHECK(jsonU64(18446744073709551615ull) == "\"18446744073709551615\"");
    CHECK(jsonU64(0) == "\"0\"");
    CHECK(jsonQuote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
}

// ---- No allocation in the tick path ------------------------------------------------------

void testNoAllocations() {
    registerNetworkComponents();
    ReplicationRegistry r;
    registerEngineReplication(r);
    std::vector<std::string> problems;
    CHECK(r.freeze(problems));
    World world;
    const Entity e = world.create();
    world.add<Transform>(e);
    world.add<RigidBody>(e);
    world.add<Collider>(e);
    world.get<Transform>(e)->position = {4, 5, 6};
    world.get<RigidBody>(e)->linearVelocity = {1, 2, 3};
    const ReplicationContext context{60};
    std::array<uint8_t, 128> buffer{};
    ProfileValue value, decoded;

    const uint64_t before = g_allocations.load();
    uint64_t checksum = 0;
    for (int tick = 0; tick < 1000; ++tick) {
        const ReplicationProfile* body = r.profile(kProfileRigidBody);
        const ReplicationProfile* byName = r.profile("engine.pose.v1");
        checksum += byName ? byName->id : 0;
        if (!body) break;
        world.get<Transform>(e)->position.x = float(tick % 100);
        if (captureProfile(*body, world, e, context, value)) break;
        uint32_t written = 0;
        if (!encodeProfile(*body, value, buffer, written)) break;
        if (!decodeProfile(*body, std::span(buffer.data(), written), decoded)) break;
        body->apply(world, e, context, decoded);
        checksum += written + (r.command(1) ? 1 : 0) + (r.event(1) ? 1 : 0);
    }
    const uint64_t allocations = g_allocations.load() - before;
    if (allocations) std::fprintf(stderr, "  %llu allocations in 1000 ticks\n", (unsigned long long)allocations);
    CHECK(allocations == 0);
    CHECK(checksum == 1000 * 54);
    CHECK(world.get<Transform>(e)->position.x == 99.0f);
    CHECK(world.get<RigidBody>(e)->linearVelocity.y == 2.0f);
}

}   // namespace

int main() {
    testSettingsRoundTrip();
    testSettingsRanges();
    testSettingsRules();
    testSha256();
    testContentDigest();
    testManifest();
    testRegistry();
    testCodec();
    testWorldRules();
    testReadyForSession();
    testJson();
    testNoAllocations();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("network settings, manifest, digests and registry: all checks passed\n");
    return 0;
}
