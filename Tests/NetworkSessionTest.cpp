// Session integration (plan Batch 4, docs/Networking.md "Session integration"): real
// PlaySessions with physics, going online over an in-process LoopbackHub with
// LoopbackOnlineServices. Start/Stop 100 times offline and online, a failure at every
// startup phase with the authored world restored, nothing left open after Stop, no events
// after a session is gone, pumps that keep running while editing and in menus, the online
// pause, save and breakpoint policies, the simulation clock's online time policy, and the
// --net-test scenarios. No Steam, sockets, window or audio device.
#include <Circuits/CircuitWorld.h>
#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/SimulationClock.h>
#include <GameFramework/NetworkPump.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <Multiplayer/NetTest.h>
#include <Multiplayer/NetworkWorld.h>
#include <Network/LoopbackServices.h>
#include <Physics/Physics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;
using net::NetworkRole;
using net::NetworkState;
using net::ReasonCode;

namespace {

int failures = 0, checks = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

fs::path tempFolder(const char* name) {
    const auto folder =
        fs::temp_directory_path() / (std::string("shockwave-") + name + "-" +
                                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(folder);
    return folder;
}

// What the GameMode saw.
struct Seen {
    NetworkRole roleAtBeginPlay = NetworkRole::Offline;
    bool authorityAtBeginPlay = false;
    int beginPlays = 0, endPlays = 0, collects = 0, suppressedCollects = 0, ticks = 0;
    bool throwInBeginPlay = false;
};

class Mode final : public GameMode {
public:
    explicit Mode(Seen& seen) : s(seen) {}
    void beginPlay(GameContext& game) override {
        ++s.beginPlays;
        s.roleAtBeginPlay = game.network().role();
        s.authorityAtBeginPlay = game.network().authority();
        if (s.throwInBeginPlay) throw std::runtime_error("beginPlay failed on purpose");
    }
    void collectInput(GameContext& game, double) override {
        ++s.collects;
        s.suppressedCollects += game.network().localInputSuppressed();
    }
    void fixedUpdate(GameContext&, double) override { ++s.ticks; }
    void endPlay(GameContext&) override { ++s.endPlays; }

private:
    Seen& s;
};

struct Instance final : GameInstance {
    Seen seen;
    std::unique_ptr<GameMode> createGameMode(const World&) override { return std::make_unique<Mode>(seen); }
};

void makeScene(World& world) {
    const Entity ground = world.create();
    world.add<Name>(ground, {"Ground"});
    world.add<Transform>(ground);
    world.add<Collider>(ground, {.halfExtents = {10, 0.5f, 10}});
    world.add<RigidBody>(ground, {.motion = RigidBody::Motion::Static});
    const Entity box = world.create();
    world.add<Name>(box, {"Box"});
    world.add<Transform>(box, {{0, 5, 0}});
    world.add<Collider>(box);
    world.add<RigidBody>(box);
}

glm::vec3 boxPosition(World& world) {
    const Entity box = findEntity(world, "Box").entity();
    return world.get<Transform>(box)->position;
}

net::NetworkStart startFor(NetworkRole role) {
    net::NetworkStart start;
    start.role = role;
    start.settings.tickRate = 60;
    start.settings.maxPlayers = 4;
    start.compatibility.gameProtocol = "session-test";
    start.compatibility.schemaDigest[0] = 1;
    start.compatibility.contentDigest[0] = 2;
    start.address = {net::TransportAddress::Loopback, 7, 0};
    return start;
}

// One machine: its world, Play session and services.
struct Machine {
    World world;
    Instance instance;
    std::unique_ptr<net::LoopbackOnlineServices> services;
    std::unique_ptr<PlaySession> session;
    NetworkPumpCounters counters;

    Machine(net::LoopbackHub& hub, uint64_t identity, JobSystem& jobs) {
        makeScene(world);
        services = std::make_unique<net::LoopbackOnlineServices>(hub, net::PeerId{identity});
        session = std::make_unique<PlaySession>(world, jobs, instance);
        session->setAudioDevice(false);
    }
    void go(NetworkRole role) {
        session->setNetworkStart(startFor(role), services.get());
        session->start();
    }
    net::NetworkWorld& network() { return *session->network(); }
};

// The application loop, one frame and one tick at a time, on the hub's clock.
struct Loop {
    net::LoopbackHub& hub;
    std::vector<Machine*> machines;
    void frame(bool overrun = false) {
        const double now = double(hub.nowUs()) / 1e6;
        for (Machine* m : machines) {
            pumpNetworkFrame(m->services.get(), m->session.get(), {now, 1.0 / 60, overrun}, &m->counters);
            m->session->update(1.0 / 60);
            m->session->frame(1.0 / 60);
        }
        hub.advance(16667);
    }
    bool until(const std::function<bool()>& done, int frames) {
        for (int i = 0; i < frames; ++i) {
            if (done()) return true;
            frame();
        }
        return done();
    }
};

// ---- The simulation clock ----------------------------------------------------------------

void simulationClock() {
    const double dt = 1.0 / 60;
    {
        // Offline: the engine's old loop exactly, smoothed and capped at 0.25 s.
        SimulationClock clock;
        double reference = 0;
        int ticks = 0, referenceTicks = 0;
        const double frames[] = {0.016, 0.017, 0.5, 0.001, 0.033, 0.25, 2.0, 0.0166};
        for (double f : frames) {
            ticks += clock.advance(f * 3, f, dt).ticks;   // raw is ignored offline
            reference += std::min(f, 0.25);
            while (reference >= dt) reference -= dt, ++referenceTicks;
        }
        check(ticks == referenceTicks, "offline time gives the same ticks as the old loop");
        check(std::abs(clock.accumulator() - reference) < 1e-12, "and the same remainder");
        check(clock.counters().stalls == 0 && clock.counters().overruns == 0, "offline never reports stalls");
        check(clock.advance(1.0, 1.0, dt).ticks == 15, "a long offline frame runs 0.25 s of ticks");
    }
    {
        // Online: raw time, at most 8 ticks a frame, debt carried over.
        SimulationClock clock;
        clock.setSource(SimulationTime::Monotonic);
        SimulationStep step = clock.advance(0.010, 1.0, dt);
        check(step.ticks == 0 && !step.stalled, "online uses raw time, not the smoothed one");
        step = clock.advance(0.2, 0.016, dt);
        check(step.ticks == 8 && step.frameTime == 0.2, "online catches up at most 8 ticks a frame");
        check(step.debt > 0.07 && !step.overrun, "and carries the rest as debt");
        step = clock.advance(0.016, 0.016, dt);
        check(step.ticks == 5, "the debt is paid off over the next frames");
        step = clock.advance(0.3, 0.016, dt);
        check(step.stalled && step.overrun && step.ticks == 8, "a stall over 250 ms is an overrun");
        check(clock.accumulator() < dt, "an overrun drops the debt");
        check(clock.counters().stalls == 1 && clock.counters().overruns == 1, "stalls and overruns are counted");
        // Steady 8 ms frames at 60 Hz never overrun.
        SimulationClock steady;
        steady.setSource(SimulationTime::Monotonic);
        int t = 0;
        bool overrun = false;
        for (int i = 0; i < 1250; ++i) {
            const SimulationStep s = steady.advance(0.008, 0.008, dt);
            t += s.ticks;
            overrun |= s.overrun;
        }
        check(!overrun && t >= 599 && t <= 600, "10 s of 8 ms frames is 600 ticks online");
        // Debt that grows (frames slower than 8 ticks' worth) ends in an overrun.
        SimulationClock slow;
        slow.setSource(SimulationTime::Monotonic);
        bool ended = false;
        for (int i = 0; i < 100 && !ended; ++i) ended = slow.advance(0.2, 0.2, 1.0 / 120).overrun;
        check(ended, "debt growing past 250 ms is an overrun");
        check(clock.advance(-1, NAN, dt).ticks == 0, "negative and NaN times count as nothing");
    }
}

// ---- Offline -----------------------------------------------------------------------------

void offline() {
    World world;
    makeScene(world);
    const glm::vec3 authored = boxPosition(world);
    const size_t entities = world.entityCount();
    JobSystem jobs(2);
    Instance instance;
    const fs::path saves = tempFolder("session-offline");
    PlaySession session(world, jobs, instance);
    session.setAudioDevice(false);
    session.setSaveFolder(saves);
    for (int round = 0; round < 100; ++round) {
        session.start();
        net::NetworkWorld* network = session.network();
        check(network && network->role() == NetworkRole::Offline, "offline Play has a network world, role offline");
        check(network && network->authority() && network->localSlot() == 1, "offline is local authority");
        check(network && !network->hasTransport() && !session.online(), "offline has no transport");
        for (int i = 0; i < 5; ++i) session.update(1.0 / 60);
        session.stop();
        if (boxPosition(world) != authored || world.entityCount() != entities) {
            check(false, "offline Stop restores the world (round " + std::to_string(round) + ")");
            break;
        }
    }
    check(instance.seen.beginPlays == 100 && instance.seen.endPlays == 100, "offline Start/Stop 100 times");
    check(instance.seen.roleAtBeginPlay == NetworkRole::Offline, "beginPlay sees the offline role");
    check(session.network() == nullptr, "no network world while editing");
    // Offline keeps its old pause and save behaviour.
    session.start();
    check(session.pause() && session.state() == PlaySession::State::Paused, "offline Pause pauses");
    session.start();
    check(session.saves()->save("slot"), "offline saving works");
    session.saves()->wait();
    check(fs::exists(saves / "slot.swsave"), "and writes the file");
    check(session.circuits()->breakpointsAllowed(), "offline breakpoints stay on");
    // Pumping an offline session does nothing, and collects input while playing.
    pumpNetworkFrame(nullptr, &session, {1, 1.0 / 60, true});
    check(instance.seen.collects == 1, "collectInput runs once per pumped frame");
    check(session.network()->state() == NetworkState::Offline, "an overrun doesn't touch offline play");
    session.stop();
    fs::remove_all(saves);
}

// ---- Online over loopback ----------------------------------------------------------------

void online() {
    net::LoopbackHub hub;
    JobSystem jobs(2);
    Machine host(hub, 100, jobs), client(hub, 200, jobs);
    const glm::vec3 authored = boxPosition(host.world);
    Loop loop{hub, {&host, &client}};

    host.go(NetworkRole::Authority);
    check(host.network().state() == NetworkState::Active, "the host is active once it listens");
    check(host.instance.seen.roleAtBeginPlay == NetworkRole::Authority, "beginPlay sees the authority role");
    client.go(NetworkRole::Client);
    check(client.network().state() == NetworkState::Connecting, "a client starts connecting");
    check(!client.instance.seen.authorityAtBeginPlay, "beginPlay sees a client without authority");
    check(
        loop.until([&] { return client.network().state() == NetworkState::Synchronizing; }, 120),
        "the client is welcomed and authenticated");
    check(client.network().localSlot() == 2, "the first client gets slot 2");
    check(host.network().stats().admitted == 1, "the host admitted it");
    net::NetworkEvent event;
    bool joined = false;
    while (host.network().pollEvent(event))
        joined |= event.kind == net::NetworkEventKind::PlayerJoined && event.slot == 2;
    check(joined, "the host reports PlayerJoined");
    check(
        host.services->openAuthSessions() == 1 && client.services->openAuthSessions() == 1,
        "both sides validated the other's ticket");

    // Policies while online.
    check(host.session->online() && client.session->online(), "both are online");
    check(!host.session->pause() && host.session->state() == PlaySession::State::Playing, "online Pause is refused");
    check(!client.session->pause(), "on clients too");
    check(
        !host.session->saves()->save("slot") &&
            host.session->saves()->lastError() == std::string(kUnavailableDuringNetworkPlay),
        "online saving fails with UnavailableDuringNetworkPlay");
    check(
        !host.session->saves()->load("slot") && !host.session->saves()->loadFile("x.swsave") &&
            !host.session->saves()->remove("slot"),
        "and so do load and remove");
    check(!host.session->circuits()->breakpointsAllowed(), "breakpoints are off online");
    // A client in a menu keeps pumping and the host keeps simulating.
    client.network().setLocalInputSuppressed(true);
    const int hostTicks = host.instance.seen.ticks, clientCollects = client.instance.seen.collects;
    const uint64_t clientPumps = client.services->pumps();
    for (int i = 0; i < 30; ++i) loop.frame();
    check(host.instance.seen.ticks == hostTicks + 30, "the host simulates every tick");
    check(
        client.instance.seen.collects == clientCollects + 30 && client.instance.seen.suppressedCollects == 30,
        "a client in a menu still collects (neutral) input every frame");
    check(client.services->pumps() == clientPumps + 30, "and still pumps its services");
    check(client.network().state() == NetworkState::Synchronizing, "and stays connected");

    // Stop: the host first; the client hears and fails HostLeft.
    host.session->stop();
    check(host.session->network() == nullptr, "the host's network world is gone after Stop");
    check(boxPosition(host.world) == authored, "the host's authored world is restored");
    check(
        loop.until([&] { return client.network().state() == NetworkState::Failed; }, 10),
        "the client notices the host stopped");
    check(client.network().failure() == ReasonCode::HostLeft, "with HostLeft");
    check(client.session->online(), "a failed client is still in network play until Stop");
    check(!client.session->pause(), "so Pause stays off");
    client.session->stop();
    check(boxPosition(client.world) == authored, "the client's authored world is restored");
    check(host.services->openAuthSessions() == 0 && client.services->openAuthSessions() == 0, "auth sessions ended");
    check(host.services->liveTickets() == 0 && client.services->liveTickets() == 0, "tickets cancelled");
    for (int i = 0; i < 5; ++i) loop.frame();
    check(hub.stats().packetsInFlight == 0, "nothing in flight after both stopped");

    // Menus and editing: services are pumped with no Play at all.
    const uint64_t pumps = host.services->pumps();
    for (int i = 0; i < 10; ++i) pumpNetworkFrame(host.services.get(), host.session.get(), {0, 1.0 / 60, false});
    pumpNetworkFrame(host.services.get(), nullptr, {0, 1.0 / 60, false});
    check(host.services->pumps() == pumps + 11, "services are pumped while editing and without a session");
}

void startStop100() {
    net::LoopbackHub hub;
    JobSystem jobs(2);
    Machine host(hub, 100, jobs), client(hub, 200, jobs);
    const glm::vec3 authored = boxPosition(host.world);
    Loop loop{hub, {&host, &client}};
    int rounds = 0;
    for (int round = 0; round < 100; ++round) {
        host.go(NetworkRole::Authority);
        client.go(NetworkRole::Client);
        if (!loop.until([&] { return client.network().state() == NetworkState::Synchronizing; }, 120)) break;
        // Alternate who stops first.
        if (round % 2) {
            client.session->stop();
            loop.until([&] { return host.network().stats().admitted == 0; }, 10);
            if (host.network().stats().admitted != 0) break;
            host.session->stop();
        } else {
            host.session->stop();
            loop.until([&] { return client.network().state() == NetworkState::Failed; }, 10);
            client.session->stop();
        }
        loop.frame();
        if (boxPosition(host.world) != authored || boxPosition(client.world) != authored) break;
        if (host.services->openAuthSessions() || client.services->openAuthSessions() || host.services->liveTickets() ||
            client.services->liveTickets() || hub.stats().packetsInFlight)
            break;
        ++rounds;
    }
    check(rounds == 100, "online Start/Stop 100 times, alternating who stops first (" + std::to_string(rounds) + ")");
    check(host.instance.seen.beginPlays == 100 && host.instance.seen.endPlays == 100, "every Play began and ended");
}

// ---- Startup failures --------------------------------------------------------------------

// start() throws, and everything is as it was.
void expectStartFails(Machine& m, const char* what) {
    const glm::vec3 authored = boxPosition(m.world);
    bool threw = false;
    try {
        m.session->start();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, std::string(what) + ": start() fails");
    check(
        m.session->state() == PlaySession::State::Editing && !m.session->network(),
        std::string(what) + ": the session is back to editing");
    check(boxPosition(m.world) == authored, std::string(what) + ": the authored world is restored");
    check(m.services->openAuthSessions() == 0, std::string(what) + ": no auth sessions");
}

void startupFailures() {
    net::LoopbackHub hub;
    JobSystem jobs(2);
    {
        Machine m(hub, 100, jobs);
        m.session->setNetworkStart(startFor(NetworkRole::Authority), nullptr);
        expectStartFails(m, "no services");
    }
    {
        Machine m(hub, 100, jobs);
        m.services->setTransportFailure(true);
        m.session->setNetworkStart(startFor(NetworkRole::Client), m.services.get());
        expectStartFails(m, "no transport");
    }
    {
        Machine m(hub, 100, jobs);
        auto unavailable = net::makeUnavailableOnlineServices();
        m.session->setNetworkStart(startFor(NetworkRole::Authority), unavailable.get());
        expectStartFails(m, "services unavailable");
    }
    {
        Machine m(hub, 100, jobs);
        net::NetworkStart start = startFor(NetworkRole::Authority);
        start.authenticate = false;
        start.address.kind = net::TransportAddress::SteamPeer;
        m.session->setNetworkStart(start, m.services.get());
        expectStartFails(m, "no authentication on Steam");
    }
    {
        Machine first(hub, 100, jobs), second(hub, 101, jobs);
        first.go(NetworkRole::Authority);
        second.session->setNetworkStart(startFor(NetworkRole::Authority), second.services.get());
        expectStartFails(second, "the address is taken");
        first.session->stop();
        // The listener is gone with the session: the address is free again.
        second.go(NetworkRole::Authority);
        check(second.network().state() == NetworkState::Active, "a stopped host's address can be reused");
        second.session->stop();
    }
    {
        Machine m(hub, 100, jobs);
        m.instance.seen.throwInBeginPlay = true;
        m.session->setNetworkStart(startFor(NetworkRole::Authority), m.services.get());
        expectStartFails(m, "beginPlay throws");
        m.instance.seen.throwInBeginPlay = false;
        m.go(NetworkRole::Authority);
        check(m.network().state() == NetworkState::Active, "the listener from the failed start was closed");
        m.session->stop();
    }
}

// Failures after start, at each phase of the handshake: the client ends Failed with the
// right reason, and Stop still cleans up.
void handshakeFailures() {
    struct Case {
        const char* name;
        ReasonCode expected;
        std::function<void(Machine& host, Machine& client, net::NetworkStart& clientStart)> setup;
        int frames = 120;
    };
    const Case cases[] = {
        {"protocol mismatch", ReasonCode::ProtocolMismatch,
         [](Machine&, Machine&, net::NetworkStart& c) { c.compatibility.gameProtocol = "other"; }},
        {"content mismatch", ReasonCode::ContentMismatch,
         [](Machine&, Machine&, net::NetworkStart& c) { c.compatibility.contentDigest[5] = 9; }},
        {"scene mismatch", ReasonCode::MapNotLoaded,
         [](Machine&, Machine&, net::NetworkStart& c) { c.compatibility.sceneId = 3; }},
        {"settings mismatch", ReasonCode::ContentMismatch,
         [](Machine&, Machine&, net::NetworkStart& c) { c.settings.snapshotRate = 30; }},
        {"client's ticket can't be made", ReasonCode::AuthUnavailable,
         [](Machine&, Machine& client, net::NetworkStart&) { client.services->setTicketFailure(true, false); }},
        {"client's ticket fails later", ReasonCode::AuthUnavailable,
         [](Machine&, Machine& client, net::NetworkStart&) { client.services->setTicketFailure(false, true); }},
        {"host's ticket fails later", ReasonCode::AuthUnavailable,
         [](Machine& host, Machine&, net::NetworkStart&) { host.services->setTicketFailure(false, true); }},
        {"host refuses the client's ticket", ReasonCode::AuthenticationFailed,
         [](Machine& host, Machine&, net::NetworkStart&) {
             host.services->setValidation(net::LoopbackOnlineServices::Validation::Refuse);
         }},
        {"client refuses the host's ticket", ReasonCode::AuthenticationFailed,
         [](Machine&, Machine& client, net::NetworkStart&) {
             client.services->setValidation(net::LoopbackOnlineServices::Validation::Refuse);
         }},
        {"host never validates", ReasonCode::AuthUnavailable,
         [](Machine& host, Machine&, net::NetworkStart&) {
             host.services->setValidation(net::LoopbackOnlineServices::Validation::NoAnswer);
         },
         60 * 17},
        {"client never validates", ReasonCode::AuthUnavailable,
         [](Machine&, Machine& client, net::NetworkStart&) {
             client.services->setValidation(net::LoopbackOnlineServices::Validation::NoAnswer);
         },
         60 * 17},
        {"no baseline in time", ReasonCode::Timeout,
         [](Machine&, Machine&, net::NetworkStart& c) { c.synchronizeTimeoutSeconds = 2; }, 60 * 4},
        {"server full", ReasonCode::ServerFull, nullptr},
    };
    for (const Case& c : cases) {
        net::LoopbackHub hub;
        JobSystem jobs(2);
        Machine host(hub, 100, jobs), client(hub, 200, jobs), other(hub, 300, jobs);
        Loop loop{hub, {&host, &client}};
        net::NetworkStart hostStart = startFor(NetworkRole::Authority);
        net::NetworkStart clientStart = startFor(NetworkRole::Client);
        if (c.setup) c.setup(host, client, clientStart);
        if (std::string(c.name) == "server full") {
            hostStart.settings.maxPlayers = 2;
            clientStart.settings.maxPlayers = 2;
        }
        host.session->setNetworkStart(hostStart, host.services.get());
        host.session->start();
        if (std::string(c.name) == "server full") {   // another client takes the only slot
            loop.machines.push_back(&other);
            other.session->setNetworkStart(clientStart, other.services.get());
            other.session->start();
            loop.until([&] { return other.network().state() == NetworkState::Synchronizing; }, 120);
        }
        client.session->setNetworkStart(clientStart, client.services.get());
        client.session->start();
        loop.until([&] { return client.network().state() == NetworkState::Failed; }, c.frames);
        const NetworkState state = client.network().state();
        check(
            state == NetworkState::Failed,
            std::string(c.name) + ": the client fails (it's " + net::networkStateName(state) + ")");
        check(
            client.network().failure() == c.expected,
            std::string(c.name) + ": with " + net::reasonName(c.expected) + ", not " +
                (net::reasonName(client.network().failure()) ? net::reasonName(client.network().failure()) : "?"));
        check(!client.network().hasTransport(), std::string(c.name) + ": the failed client's transport is released");
        for (int i = 0; i < 3; ++i) loop.frame();
        check(
            host.network().stats().admitted == (std::string(c.name) == "server full" ? 1u : 0u),
            std::string(c.name) + ": the host doesn't keep the client");
        client.session->stop();
        other.session->stop();
        host.session->stop();
        check(
            host.services->openAuthSessions() == 0 && client.services->openAuthSessions() == 0 &&
                host.services->liveTickets() == 0 && client.services->liveTickets() == 0,
            std::string(c.name) + ": nothing left open after Stop");
    }
}

// ---- After a session ---------------------------------------------------------------------

void noEventsAfterDestruction() {
    net::LoopbackHub hub;
    JobSystem jobs(2);
    Machine host(hub, 100, jobs), client(hub, 200, jobs);
    Loop loop{hub, {&host, &client}};
    host.go(NetworkRole::Authority);
    client.go(NetworkRole::Client);
    // Stop both mid-handshake, with ticket results still waiting in the services.
    loop.until([&] { return client.network().state() == NetworkState::Authenticating; }, 60);
    loop.frame();
    client.session->stop();
    host.session->stop();
    for (int i = 0; i < 5; ++i) loop.frame();   // whatever the services still had goes nowhere
    check(!host.session->network() && !client.session->network(), "stopped sessions have no network world");
    net::OnlineEvent late;
    late.kind = net::OnlineEventKind::AuthSucceeded;
    late.peer = {200};
    check(!host.session->handleOnlineEvent(late), "a stopped session takes no service events");

    // A closed world reports nothing more, whatever arrives.
    net::NetworkWorld world(startFor(NetworkRole::Authority), host.services.get());
    check(world.begin().ok(), "a host world starts");
    world.pump(0);
    world.close();
    net::NetworkEvent event;
    check(!world.pollEvent(event), "close() leaves no events to poll");
    world.pump(1);
    world.fail(ReasonCode::Timeout, "after close");
    check(!world.pollEvent(event) && !world.handleOnlineEvent(late), "and nothing is reported after it");
    check(world.state() == NetworkState::Offline && !world.hasTransport(), "a closed host is offline");
    world.close();   // idempotent

    // Shutdown order with a lobby: joins off, then auth sessions, then leaving the lobby.
    net::LoopbackHub hub2;
    Machine lobbyHost(hub2, 100, jobs), joiner(hub2, 200, jobs);
    Loop loop2{hub2, {&lobbyHost, &joiner}};
    auto lobbyRequest = lobbyHost.services->createLobby({});
    check(lobbyRequest.ok(), "a loopback host can make a lobby");
    lobbyHost.services->pump();
    net::OnlineEvent created;
    check(lobbyHost.services->pollEvent(created) && created.kind == net::OnlineEventKind::LobbyCreated, "LobbyCreated");
    net::NetworkStart start = startFor(NetworkRole::Authority);
    start.lobby = created.lobby;
    lobbyHost.session->setNetworkStart(start, lobbyHost.services.get());
    lobbyHost.session->start();
    joiner.go(NetworkRole::Client);   // no lobby: refused, since the host has one
    loop2.until([&] { return joiner.network().state() == NetworkState::Failed; }, 60);
    check(joiner.network().failure() == ReasonCode::NotLobbyMember, "a host with a lobby refuses direct joins");
    lobbyHost.services->clearCalls();
    lobbyHost.session->stop();
    const auto& calls = lobbyHost.services->calls();
    auto at = [&](const char* name) {
        return std::find(calls.begin(), calls.end(), std::string(name)) - calls.begin();
    };
    check(
        at("setLobbyJoinable 0") < at("leaveLobby") && at("leaveLobby") < std::ptrdiff_t(calls.size()),
        "Stop closes the lobby to joins first and leaves it last");
    joiner.session->stop();
}

void overrun() {
    net::LoopbackHub hub;
    JobSystem jobs(2);
    Machine host(hub, 100, jobs), client(hub, 200, jobs);
    Loop loop{hub, {&host, &client}};
    host.go(NetworkRole::Authority);
    client.go(NetworkRole::Client);
    loop.until([&] { return client.network().state() == NetworkState::Synchronizing; }, 120);
    // The client's simulation falls too far behind.
    pumpNetworkFrame(client.services.get(), client.session.get(), {double(hub.nowUs()) / 1e6, 0.4, true});
    check(
        client.network().state() == NetworkState::Failed && client.network().failure() == ReasonCode::SimulationOverrun,
        "an overrun ends network play with SimulationOverrun");
    check(loop.until([&] { return host.network().stats().admitted == 0; }, 10), "the host drops the client");
    host.session->stop();
    client.session->stop();
}

void netTestScenarios() {
    for (const char* scenario : net::netTestScenarios()) {
        net::NetTestOptions options;
        options.scenario = scenario;
        options.seed = 42;
        net::NetTestReport report;
        check(
            net::runNetTest(options, report) && report.ok,
            std::string("--net-test --scenario ") + scenario + ": " + net::netTestText(report));
    }
    net::NetTestOptions bad;
    bad.scenario = "nonsense";
    net::NetTestReport report;
    check(!net::runNetTest(bad, report), "an unknown scenario is refused");
}

}   // namespace

int main() {
    simulationClock();
    offline();
    online();
    startStop100();
    startupFailures();
    handshakeFailures();
    noEventsAfterDestruction();
    overrun();
    netTestScenarios();
    if (failures) {
        std::fprintf(stderr, "network_session: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("network_session: all %d checks passed\n", checks);
    return 0;
}
