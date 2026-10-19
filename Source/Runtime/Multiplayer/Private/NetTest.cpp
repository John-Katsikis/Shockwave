// player --net-test scenarios: NetworkWorlds on a LoopbackHub (Multiplayer/NetTest.h).
#include <Multiplayer/NetTest.h>

#include <Multiplayer/NetworkSchema.h>
#include <Network/LoopbackServices.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>

namespace shockwave::net {

namespace {

constexpr const char* kScenarios[] = {"handshake", "rejections", "stop-start"};
constexpr uint64_t kHostIdentity = 1000;

struct Node {
    std::string name;
    std::unique_ptr<LoopbackOnlineServices> services;
    std::unique_ptr<NetworkWorld> world;
    ReasonCode expected = ReasonCode::Normal;
    double joinMs = -1;
    NetTestPeer result;
};

// Host and clients stepped together on the hub's virtual clock, one tick at a time.
class Harness {
public:
    Harness(const NetTestOptions& options, const FaultProfile& faults) : m_options(options) {
        LoopbackHubConfig config;
        config.seed = options.seed;
        config.faults = faults;
        m_hub = std::make_unique<LoopbackHub>(config);
        DeterministicRandom random(options.seed ^ 0x5eedu);
        for (uint8_t& b : m_compatibility.schemaDigest) b = uint8_t(random.next());
        for (uint8_t& b : m_compatibility.contentDigest) b = uint8_t(random.next());
        m_compatibility.gameProtocol = "nettest";
        m_compatibility.sceneId = 1;
    }

    NetworkStart start(NetworkRole role) const {
        NetworkStart s;
        s.role = role;
        s.settings = m_options.settings;
        s.compatibility = m_compatibility;
        s.address = {TransportAddress::Loopback, 1, 0};
        return s;
    }

    Node& add(std::string name, const NetworkStart& start, uint64_t identity) {
        Node& node = m_nodes.emplace_back();
        node.name = std::move(name);
        node.services = std::make_unique<LoopbackOnlineServices>(*m_hub, PeerId{identity});
        node.world = std::make_unique<NetworkWorld>(start, node.services.get());
        node.result.name = node.name;
        node.result.role = start.role;
        node.result.peer = identity;
        return node;
    }

    // Starts every node not started yet; false (with the error) when one can't.
    bool begin(Node& node, std::vector<std::string>& errors) {
        if (auto begun = node.world->begin(); !begun) {
            errors.push_back(node.name + ": didn't start: " + begun.error().message);
            return false;
        }
        return true;
    }

    double nowSeconds() const { return double(m_hub->nowUs()) / 1e6; }

    void step() {
        const double now = nowSeconds();
        for (Node& node : m_nodes) {
            if (!node.world) continue;
            node.services->pump();
            OnlineEvent event;
            while (node.services->pollEvent(event)) node.world->handleOnlineEvent(event);
            node.world->pump(now);
            node.world->drain(m_ticks);
            if (node.world->role() == NetworkRole::Client && node.joinMs < 0 &&
                node.world->state() == NetworkState::Synchronizing)
                node.joinMs = now * 1000;
        }
        m_hub->advance(1000000 / std::max<uint32_t>(m_options.settings.tickRate, 1));
        ++m_ticks;
    }

    bool runUntil(const std::function<bool()>& done, uint32_t maxTicks) {
        for (uint32_t i = 0; i < maxTicks; ++i) {
            if (done()) return true;
            step();
        }
        return done();
    }

    // Records where each node got to, then stops it.
    void record(Node& node) {
        NetworkWorld& w = *node.world;
        node.result.state = w.state();
        node.result.reason = w.state() == NetworkState::Failed ? w.failure() : ReasonCode::Normal;
        node.result.expected = node.expected;
        node.result.slot = w.localSlot();
        node.result.joinMs = node.joinMs;
        const NetworkWorldStats& s = w.stats();
        m_sent += s.messagesSent;
        m_received += s.messagesReceived;
        m_rejected += s.rejected;
        m_malformed += s.malformed;
    }
    void stop(Node& node) {
        if (!node.world) return;
        node.world->close();
        node.world.reset();
    }
    // Stops everyone, lets the hub settle, and reports what's still open.
    void stopAll(NetTestReport& report) {
        for (Node& node : m_nodes)
            if (node.world) record(node), stop(node);
        for (int i = 0; i < 10; ++i) step();
        finish(report);
    }
    void finish(NetTestReport& report) {
        report.packetsInFlightAfterStop += m_hub->stats().packetsInFlight;
        for (Node& node : m_nodes) {
            report.openAuthSessionsAfterStop += node.services->openAuthSessions();
            report.liveTicketsAfterStop += node.services->liveTickets();
            report.peers.push_back(node.result);
        }
        report.messagesSent += m_sent;
        report.messagesReceived += m_received;
        report.rejected += m_rejected;
        report.malformed += m_malformed;
        report.durationTicks += m_ticks;
    }

    std::deque<Node>& nodes() { return m_nodes; }
    LoopbackHub& hub() { return *m_hub; }
    uint32_t ticks() const { return m_ticks; }
    NetworkCompatibility& compatibility() { return m_compatibility; }
    uint32_t ticksFor(double seconds) const { return uint32_t(seconds * m_options.settings.tickRate) + 1; }

private:
    NetTestOptions m_options;
    std::unique_ptr<LoopbackHub> m_hub;
    std::deque<Node> m_nodes;   // the host first; a deque, so references stay valid
    NetworkCompatibility m_compatibility;
    uint32_t m_ticks = 0;
    uint64_t m_sent = 0, m_received = 0, m_rejected = 0, m_malformed = 0;
};

void checkClean(NetTestReport& report) {
    if (report.packetsInFlightAfterStop)
        report.errors.push_back(
            std::to_string(report.packetsInFlightAfterStop) + " packets still in flight after stop");
    if (report.openAuthSessionsAfterStop)
        report.errors.push_back(std::to_string(report.openAuthSessionsAfterStop) + " auth sessions left open");
    if (report.liveTicketsAfterStop)
        report.errors.push_back(std::to_string(report.liveTicketsAfterStop) + " session tickets not cancelled");
}

void handshake(const NetTestOptions& options, const FaultProfile& faults, NetTestReport& report) {
    Harness h(options, faults);
    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    if (!h.begin(host, report.errors)) return h.finish(report);
    for (uint32_t i = 0; i < options.clients; ++i) {
        Node& client = h.add("client " + std::to_string(i + 1), h.start(NetworkRole::Client), kHostIdentity + 1 + i);
        h.begin(client, report.errors);
    }
    auto joined = [&] {
        for (Node& n : h.nodes())
            if (n.world->role() == NetworkRole::Client && n.world->state() != NetworkState::Synchronizing) return false;
        return true;
    };
    if (!h.runUntil(joined, h.ticksFor(options.settings.handshakeTimeoutSeconds + 1)))
        report.errors.push_back("not every client reached synchronizing");
    const NetworkWorldStats& hs = h.nodes()[0].world->stats();
    if (hs.admitted != options.clients)
        report.errors.push_back(
            "the host admitted " + std::to_string(hs.admitted) + " of " + std::to_string(options.clients) + " clients");
    std::vector<uint16_t> slots;
    for (Node& n : h.nodes())
        if (n.world->role() == NetworkRole::Client) slots.push_back(n.world->localSlot());
    std::sort(slots.begin(), slots.end());
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i] != i + 2) {
            report.errors.push_back("player slots aren't 2.." + std::to_string(options.clients + 1));
            break;
        }
    h.stopAll(report);
    checkClean(report);
}

struct Rejection {
    const char* name;
    ReasonCode expected;
    std::function<void(Harness&, NetworkStart& client)> setup;
    double seconds = 3;
    bool hostStops = false;   // the host closes once the client is authenticating
};

void rejections(const NetTestOptions& options, NetTestReport& report) {
    NetTestOptions o = options;
    o.settings.maxPlayers = 2;
    const Rejection cases[] = {
        {"protocol mismatch", ReasonCode::ProtocolMismatch,
         [](Harness&, NetworkStart& c) { c.compatibility.gameProtocol = "another game"; }},
        {"content mismatch", ReasonCode::ContentMismatch,
         [](Harness&, NetworkStart& c) { c.compatibility.contentDigest[0] ^= 1; }},
        {"schema mismatch", ReasonCode::ContentMismatch,
         [](Harness&, NetworkStart& c) { c.compatibility.schemaDigest[31] ^= 1; }},
        {"scene mismatch", ReasonCode::MapNotLoaded, [](Harness&, NetworkStart& c) { c.compatibility.sceneId = 2; }},
        {"server full", ReasonCode::ServerFull, nullptr},
        {"ticket refused", ReasonCode::AuthenticationFailed,
         [](Harness& h, NetworkStart&) {
             h.nodes()[0].services->setValidation(LoopbackOnlineServices::Validation::Refuse);
         }},
        {"ticket never answered", ReasonCode::AuthUnavailable,
         [](Harness& h, NetworkStart&) {
             h.nodes()[0].services->setValidation(LoopbackOnlineServices::Validation::NoAnswer);
         },
         o.settings.handshakeTimeoutSeconds + 2},
        {"host stops mid-handshake", ReasonCode::HostLeft, nullptr, 3, true},
        {"no host", ReasonCode::ServiceUnavailable, nullptr},
    };
    for (const Rejection& c : cases) {
        Harness h(o, {});
        const std::string name = c.name;
        Node& host = h.add("host (" + name + ")", h.start(NetworkRole::Authority), kHostIdentity);
        NetworkStart start = h.start(NetworkRole::Client);
        if (c.setup) c.setup(h, start);
        if (name == "no host") {
            host.world.reset();   // nobody listens
        } else if (!h.begin(host, report.errors)) {
            h.finish(report);
            continue;
        }
        if (name == "server full") {   // one client fills the only slot first
            Node& first = h.add("first client", h.start(NetworkRole::Client), kHostIdentity + 1);
            h.begin(first, report.errors);
            h.runUntil([&] { return first.world->state() == NetworkState::Synchronizing; }, h.ticksFor(3));
        }
        Node& client = h.add(name, start, kHostIdentity + 2);
        client.expected = c.expected;
        if (c.hostStops) client.services->setValidation(LoopbackOnlineServices::Validation::NoAnswer);
        h.begin(client, report.errors);
        if (c.hostStops) {
            h.runUntil([&] { return client.world->state() == NetworkState::Authenticating; }, h.ticksFor(3));
            h.runUntil([] { return false; }, 10);   // tickets exchanged; the client is still validating
            h.record(h.nodes()[0]);
            h.stop(h.nodes()[0]);
        }
        h.runUntil([&] { return client.world->state() == NetworkState::Failed; }, h.ticksFor(c.seconds));
        if (client.world->state() != NetworkState::Failed)
            report.errors.push_back(
                name + ": the client didn't fail (it's " + networkStateName(client.world->state()) + ")");
        else if (client.world->failure() != c.expected)
            report.errors.push_back(
                name + ": failed with " + reasonName(client.world->failure()) + ", expected " + reasonName(c.expected));
        h.stopAll(report);
    }
    checkClean(report);
}

void stopStart(const NetTestOptions& options, NetTestReport& report) {
    Harness h(options, {});
    Node& host = h.add("host", h.start(NetworkRole::Authority), kHostIdentity);
    Node& client = h.add("client", h.start(NetworkRole::Client), kHostIdentity + 1);
    for (uint32_t round = 0; round < 100; ++round) {
        host.world = std::make_unique<NetworkWorld>(h.start(NetworkRole::Authority), host.services.get());
        client.world = std::make_unique<NetworkWorld>(h.start(NetworkRole::Client), client.services.get());
        if (!h.begin(host, report.errors) || !h.begin(client, report.errors)) break;
        client.joinMs = -1;
        if (!h.runUntil([&] { return client.world->state() == NetworkState::Synchronizing; }, h.ticksFor(5))) {
            report.errors.push_back("round " + std::to_string(round + 1) + ": the client didn't join");
            break;
        }
        // Alternate who stops first; the other sees it, then stops too.
        Node& first = round % 2 ? client : host;
        Node& second = round % 2 ? host : client;
        h.stop(first);
        h.runUntil([] { return false; }, 3);
        if (second.world->role() == NetworkRole::Client && second.world->state() != NetworkState::Failed)
            report.errors.push_back("round " + std::to_string(round + 1) + ": the client didn't notice the host stop");
        if (second.world->role() == NetworkRole::Authority && second.world->stats().admitted != 0)
            report.errors.push_back("round " + std::to_string(round + 1) + ": the host kept a stopped client");
        h.record(second);
        h.stop(second);
        h.runUntil([] { return false; }, 2);
        if (host.services->openAuthSessions() || client.services->openAuthSessions() || host.services->liveTickets() ||
            client.services->liveTickets()) {
            report.errors.push_back("round " + std::to_string(round + 1) + ": auth sessions or tickets left open");
            break;
        }
        ++report.rounds;
    }
    h.finish(report);
    checkClean(report);
}

}   // namespace

std::span<const char* const> netTestScenarios() { return kScenarios; }

bool runNetTest(const NetTestOptions& options, NetTestReport& report) {
    report = {};
    report.scenario = options.scenario;
    report.seed = options.seed;
    report.preset = options.preset;
    const FaultPreset* preset = findFaultPreset(options.preset);
    if (!preset) {
        report.errors.push_back("unknown fault preset " + options.preset + " (clean, cafe, poor or hostile)");
        return false;
    }
    if (options.clients < 1 ||
        options.clients + 1 > std::clamp<uint32_t>(options.settings.maxPlayers, 2, kMaxPlayers)) {
        report.errors.push_back(
            "--clients must be 1 to max_players - 1 (" + std::to_string(options.settings.maxPlayers - 1) + ")");
        return false;
    }
    if (options.scenario == "handshake")
        handshake(options, preset->profile, report);
    else if (options.scenario == "rejections")
        rejections(options, report);
    else if (options.scenario == "stop-start")
        stopStart(options, report);
    else {
        report.errors.push_back("unknown scenario " + options.scenario + " (handshake, rejections or stop-start)");
        return false;
    }
    report.ok = report.errors.empty();
    return true;
}

std::string netTestText(const NetTestReport& r) {
    std::string out;
    char line[256];
    std::snprintf(
        line, sizeof(line), "net-test %s seed %llu preset %s: %s\n", r.scenario.c_str(), (unsigned long long)r.seed,
        r.preset.c_str(), r.ok ? "ok" : "FAILED");
    out += line;
    for (const NetTestPeer& p : r.peers) {
        std::snprintf(
            line, sizeof(line), "peer %s: %s, %s%s%s, slot %u", p.name.c_str(), networkRoleName(p.role),
            networkStateName(p.state), p.state == NetworkState::Failed ? " " : "",
            p.state == NetworkState::Failed ? reasonName(p.reason) : "", unsigned(p.slot));
        out += line;
        if (p.joinMs >= 0) {
            std::snprintf(line, sizeof(line), ", joined at %.0f ms", p.joinMs);
            out += line;
        }
        out += "\n";
    }
    if (r.rounds) out += "rounds " + std::to_string(r.rounds) + "\n";
    std::snprintf(
        line, sizeof(line),
        "ticks %u, messages %llu sent, %llu received, %llu rejected, %llu malformed; after stop: %llu packets in "
        "flight, %llu auth sessions, %llu tickets\n",
        r.durationTicks, (unsigned long long)r.messagesSent, (unsigned long long)r.messagesReceived,
        (unsigned long long)r.rejected, (unsigned long long)r.malformed, (unsigned long long)r.packetsInFlightAfterStop,
        (unsigned long long)r.openAuthSessionsAfterStop, (unsigned long long)r.liveTicketsAfterStop);
    out += line;
    for (const std::string& e : r.errors) out += "error " + e + "\n";
    for (const std::string& w : r.warnings) out += "warning " + w + "\n";
    return out;
}

std::string netTestJson(const NetTestReport& r) {
    auto list = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& item : items) out += std::string(out.empty() ? "" : ", ") + jsonQuote(item);
        return "[" + out + "]";
    };
    auto reason = [](ReasonCode code) { return jsonQuote(reasonName(code) ? reasonName(code) : "unknown"); };
    std::string out = "{\n  \"version\": 1,\n  \"mode\": \"net-test\",\n";
    out += "  \"scenario\": " + jsonQuote(r.scenario) + ",\n";
    out += "  \"ok\": " + std::string(r.ok ? "true" : "false") + ",\n";
    out += "  \"errors\": " + list(r.errors) + ",\n  \"warnings\": " + list(r.warnings) + ",\n";
    out += "  \"seed\": \"" + std::to_string(r.seed) + "\",\n";
    out += "  \"preset\": " + jsonQuote(r.preset) + ",\n";
    out += "  \"duration_ticks\": " + std::to_string(r.durationTicks) + ",\n";
    out += "  \"peers\": [";
    for (size_t i = 0; i < r.peers.size(); ++i) {
        const NetTestPeer& p = r.peers[i];
        out += std::string(i ? "," : "") + "\n    {\"name\": " + jsonQuote(p.name) +
               ", \"role\": " + jsonQuote(networkRoleName(p.role)) +
               ", \"state\": " + jsonQuote(networkStateName(p.state)) + ", \"reason\": " + reason(p.reason) +
               ", \"expected\": " + reason(p.expected) + ", \"slot\": " + std::to_string(p.slot) + ", \"peer\": \"" +
               std::to_string(p.peer) +
               "\", \"join_ms\": " + (p.joinMs >= 0 ? std::to_string(int64_t(p.joinMs)) : std::string("null")) + "}";
    }
    out += r.peers.empty() ? "],\n" : "\n  ],\n";
    out += "  \"counters\": {\"rounds\": " + std::to_string(r.rounds) +
           ", \"messages_sent\": " + std::to_string(r.messagesSent) +
           ", \"messages_received\": " + std::to_string(r.messagesReceived) +
           ", \"rejected\": " + std::to_string(r.rejected) + ", \"malformed\": " + std::to_string(r.malformed) +
           ", \"packets_in_flight_after_stop\": " + std::to_string(r.packetsInFlightAfterStop) +
           ", \"open_auth_sessions_after_stop\": " + std::to_string(r.openAuthSessionsAfterStop) +
           ", \"live_tickets_after_stop\": " + std::to_string(r.liveTicketsAfterStop) + "}\n}\n";
    return out;
}

}   // namespace shockwave::net
