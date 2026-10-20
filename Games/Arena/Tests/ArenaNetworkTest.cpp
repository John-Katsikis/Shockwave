// Arena over the network (plan Batch 6): a host and eleven clients, each a real Arena
// PlaySession with Jolt, on the multiplayer scene and Net Player prefab, over an
// in-process LoopbackHub. Checks six a side and one local camera per process, a match
// played from autoplay input commands, every client ending on the host's state, and a
// cheating client (its transport lets the test inject messages) that can't change damage,
// team, ownership, the round or its own speed. Then the offline input path: ammo, cadence
// and button presses don't depend on the frame rate.
#include "ArenaGame.h"
#include "ArenaInput.h"
#include "ArenaNetwork.h"
#include "ArenaSimulation.h"

#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>
#include <Engine/Prefab.h>
#include <Engine/SceneFile.h>
#include <Engine/SimulationClock.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/NetworkPump.h>
#include <GameFramework/PlaySession.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkWorld.h>
#include <Multiplayer/Replication.h>
#include <Network/Loopback.h>
#include <Network/LoopbackServices.h>
#include <Physics/Physics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The game module entry isn't linked into tests; ArenaNetwork.cpp's SW_NETWORK_TYPES
// registers into this list.
shockwave::detail::AutoNetworkTypes*& shockwave::detail::AutoNetworkTypes::gameNetworkTypes() {
    static AutoNetworkTypes* head = nullptr;
    return head;
}

using namespace shockwave;
using namespace arena;
namespace fs = std::filesystem;
using net::InputCommand;
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

const net::ReplicationRegistry& registry() {
    static const net::ReplicationRegistry frozen = [] {
        net::ReplicationRegistry r;
        net::registerEngineReplication(r);
        registerNetworkTypes(r);
        std::vector<std::string> problems;
        r.freeze(problems);
        for (const auto& p : problems) std::fprintf(stderr, "registry: %s\n", p.c_str());
        return r;
    }();
    return frozen;
}

// ---- A client whose transport the test controls ------------------------------------------

struct CheatTransport final : net::ITransport, net::TransportSink {
    std::unique_ptr<net::ITransport> inner;
    net::ConnectionId connection;
    net::Nonce session{};
    bool haveSession = false;
    net::TransportSink* outer = nullptr;
    // Outgoing messages from the client's own NetworkWorld: false drops one.
    std::function<bool(std::span<const uint8_t>)> filter;
    std::array<uint8_t, net::kMaxReliableMessageBytes> buffer{};

    net::PeerId localPeer() const override { return inner->localPeer(); }
    uint32_t endpointGeneration() const override { return inner->endpointGeneration(); }
    net::NetResult<void> listen(const net::TransportAddress& a, const net::TransportLimits& l) override {
        return inner->listen(a, l);
    }
    net::NetResult<net::ConnectionId> connect(const net::TransportAddress& address) override {
        auto c = inner->connect(address);
        if (c) connection = c.value();
        return c;
    }
    net::NetResult<void> accept(net::ConnectionId c) override { return inner->accept(c); }
    void close(net::ConnectionId c, ReasonCode r) override { inner->close(c, r); }
    void closeAll(ReasonCode r) override { inner->closeAll(r); }
    net::PollResult poll(net::TransportSink& sink, const net::ReceiveBudget& budget) override {
        outer = &sink;
        return inner->poll(*this, budget);
    }
    net::SendResult send(net::ConnectionId c, std::span<const uint8_t> bytes, net::Delivery d) override {
        if (filter && !filter(bytes)) return net::SendResult::Queued;
        return inner->send(c, bytes, d);
    }
    std::optional<net::ConnectionStats> stats(net::ConnectionId c) const override { return inner->stats(c); }
    void onEvent(const net::TransportEvent& event) override { outer->onEvent(event); }
    void onMessage(const net::ReceivedMessage& message) override {
        net::MessageHeader header;
        if (!haveSession && net::decodeHeader(message.bytes, header) && header.kind == net::MessageKind::Challenge) {
            session = header.session;
            haveSession = true;
        }
        outer->onMessage(message);
    }
    // Any message, encoded with the session's nonce, as if the client's game sent it.
    bool inject(const net::MessageBody& body) {
        uint32_t written = 0;
        if (!net::encodeMessage(body, session, 0, buffer, written)) return false;
        const net::MessageDescriptor* d = net::findMessage(uint8_t(net::kindOf(body)));
        return inner->send(connection, std::span(buffer.data(), written), d->delivery) == net::SendResult::Queued;
    }
};

struct CheatServices final : net::IOnlineServices {
    net::LoopbackOnlineServices inner;
    CheatTransport* transport = nullptr;
    CheatServices(net::LoopbackHub& hub, net::PeerId id) : inner(hub, id) {}
    net::OnlineCapabilities capabilities() const override { return inner.capabilities(); }
    net::PeerId localPeer() const override { return inner.localPeer(); }
    void pump() override { inner.pump(); }
    bool pollEvent(net::OnlineEvent& out) override { return inner.pollEvent(out); }
    void cancel(net::RequestId r) override { inner.cancel(r); }
    net::NetResult<net::RequestId> createLobby(const net::LobbyCreateRequest& r) override {
        return inner.createLobby(r);
    }
    net::NetResult<net::RequestId> findLobbies(const net::LobbyQuery& q) override { return inner.findLobbies(q); }
    net::NetResult<net::RequestId> joinLobby(net::LobbyId l) override { return inner.joinLobby(l); }
    net::NetResult<void> leaveLobby(net::LobbyId l) override { return inner.leaveLobby(l); }
    net::NetResult<void> inviteFriend(net::LobbyId l) override { return inner.inviteFriend(l); }
    net::NetResult<void> setLobbyJoinable(net::LobbyId l, bool j) override { return inner.setLobbyJoinable(l, j); }
    net::NetResult<void> setLobbyMetadata(net::LobbyId l, const std::string& k, const std::string& v) override {
        return inner.setLobbyMetadata(l, k, v);
    }
    net::NetResult<std::vector<net::PeerId>> lobbyMembers(net::LobbyId l) const override {
        return inner.lobbyMembers(l);
    }
    net::NetResult<std::unique_ptr<net::ITransport>> createTransport() override {
        auto t = inner.createTransport();
        if (!t) return t;
        auto wrapper = std::make_unique<CheatTransport>();
        wrapper->inner = std::move(t.value());
        transport = wrapper.get();
        return std::unique_ptr<net::ITransport>(std::move(wrapper));
    }
    net::NetResult<net::RequestId> requestAuthTicket(net::PeerId p) override { return inner.requestAuthTicket(p); }
    net::NetResult<std::span<const uint8_t>> authTicket(net::RequestId r) const override { return inner.authTicket(r); }
    net::NetResult<void> beginAuthSession(net::PeerId p, std::span<const uint8_t> t) override {
        return inner.beginAuthSession(p, t);
    }
    void endAuthSession(net::PeerId p) override { inner.endAuthSession(p); }
};

// ---- Machines ------------------------------------------------------------------------------

struct Instance final : GameInstance {
    std::unique_ptr<GameMode> createGameMode(const World&) override { return createGame(); }
};

net::NetworkStart startFor(NetworkRole role) {
    net::NetworkStart start;
    start.role = role;
    start.settings.tickRate = 60;
    start.settings.snapshotRate = 20;
    start.settings.inputRate = 60;
    start.settings.maxPlayers = 12;
    start.compatibility.gameProtocol = "arena.v1";
    start.compatibility.schemaDigest = registry().schemaDigest();
    start.compatibility.contentDigest[0] = 6;
    start.compatibility.sceneId = 2;
    start.registry = &registry();
    start.prefabs = {{1, kNetPlayerPrefab, "arena.player.v1"}};
    start.address = {net::TransportAddress::Loopback, 27015, 0};
    return start;
}

struct Machine {
    World world;
    Instance instance;
    std::unique_ptr<net::IOnlineServices> services;
    CheatServices* cheat = nullptr;
    std::unique_ptr<PlaySession> session;
    NetworkPumpCounters counters;

    Machine(net::LoopbackHub& hub, uint64_t identity, JobSystem& jobs, const World& scene, bool cheater)
        : world(scene) {
        if (cheater) {
            auto c = std::make_unique<CheatServices>(hub, net::PeerId{identity});
            cheat = c.get();
            services = std::move(c);
        } else {
            services = std::make_unique<net::LoopbackOnlineServices>(hub, net::PeerId{identity});
        }
        session = std::make_unique<PlaySession>(world, jobs, instance);
        session->setAudioDevice(false);
    }
    void go(NetworkRole role) {
        session->setNetworkStart(startFor(role), services.get());
        session->start();
    }
    net::NetworkWorld& net() { return *session->network(); }
    ArenaRules& rules() {
        ArenaRules* found = nullptr;
        world.each<ArenaRules>([&](ArenaRules& r) { found = &r; });
        return *found;
    }
    Entity pawn(uint16_t slot) {
        Entity found;
        world.each<const ArenaFighter>([&](Entity e, const ArenaFighter& f) {
            if (f.slot == slot) found = e;
        });
        return found;
    }
    int pawns() {
        int n = 0;
        world.each<const ArenaFighter>([&](const ArenaFighter& f) { n += f.slot != 0; });
        return n;
    }
};

struct Loop {
    net::LoopbackHub& hub;
    std::vector<Machine*> machines;
    void frame() {
        const double now = double(hub.nowUs()) / 1e6;
        for (Machine* m : machines) {
            pumpNetworkFrame(m->services.get(), m->session.get(), {now, 1.0 / 60, false}, &m->counters);
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
    void run(int frames) {
        for (int i = 0; i < frames; ++i) frame();
    }
};

bool loadArenaScene(const fs::path& game, const char* name, Assets& assets, World& out) {
    std::string error;
    std::vector<std::string> warnings;
    if (!loadScene(out, assets, (game / "Scenes" / name).string(), error, {}, &warnings)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    return true;
}

int primaryCameras(World& world) {
    int n = 0;
    world.each<const Camera>([&](const Camera& c) { n += c.primary; });
    return n;
}

// ---- Host and eleven clients ---------------------------------------------------------------

void twelvePlayers(const World& scene) {
    net::LoopbackHubConfig config;
    config.seed = 6;
    config.faults.latencyUs = 15000;
    config.faults.jitterUs = 3000;
    config.faults.unreliableLoss = 0.01;
    net::LoopbackHub hub(config);
    JobSystem jobs(2);
    std::vector<std::unique_ptr<Machine>> machines;
    machines.push_back(std::make_unique<Machine>(hub, 100, jobs, scene, false));
    for (int i = 0; i < 11; ++i)
        machines.push_back(std::make_unique<Machine>(hub, 200 + uint64_t(i), jobs, scene, i == 10));
    Machine& host = *machines[0];
    Machine& cheater = *machines[11];
    Loop loop{hub, {}};
    for (auto& m : machines) loop.machines.push_back(m.get());

    host.go(NetworkRole::Authority);
    check(host.pawns() == 1 && host.world.alive(host.pawn(1)), "the host has its own pawn in slot 1");
    for (size_t i = 1; i < machines.size(); ++i) machines[i]->go(NetworkRole::Client);
    const bool joined = loop.until(
        [&] {
            for (auto& m : machines)
                if (m->net().state() != NetworkState::Active || m->pawns() != 12) return false;
            return true;
        },
        600);
    check(joined, "eleven clients join and every machine has twelve pawns");
    check(teamCount(host.world, 0) == 6 && teamCount(host.world, 1) == 6, "the host puts six on each team");
    for (size_t i = 1; i < machines.size(); ++i) {
        Machine& m = *machines[i];
        const uint16_t slot = m.net().localPlayer();
        const Entity mine = m.pawn(slot);
        check(m.world.alive(mine) && m.net().isLocallyControlled(mine), "each client finds the pawn it controls");
        check(
            m.world.get<CharacterController>(mine) && !m.world.get<CharacterController>(mine)->enabled,
            "a client doesn't simulate its copy of a character");
    }
    for (auto& m : machines) {
        check(primaryCameras(m->world) == 1, "one primary camera per process");
        int cameras = 0;
        m->world.each<const ArenaFighter>([&](Entity e, const ArenaFighter&) { cameras += m->world.has<Camera>(e); });
        check(cameras == 0, "no pawn carries a camera");
        // The camera sits at the local pawn's eye after a frame.
        const Entity camera = findEntity(m->world, "Arena Camera").entity();
        const Entity mine = m->pawn(m->net().localPlayer());
        const glm::vec3 eye = m->world.get<Transform>(mine)->position + glm::vec3(0, kPlayerEye, 0);
        check(
            glm::length(m->world.get<Transform>(camera)->position - eye) < 0.05f, "the camera follows the local pawn");
    }
    check(host.rules().phase == ArenaRules::Waiting, "the round waits for players");

    // ---- Spoofing before the round starts ----
    const uint16_t cheatSlot = cheater.net().localPlayer();
    const Entity cheatPawnHost = host.pawn(cheatSlot);
    const int cheatTeam = host.world.get<ArenaFighter>(cheatPawnHost)->team;
    net::ProfileValue other;
    other.fields[0].u = uint64_t(1 - cheatTeam);
    check(bool(cheater.net().submitCommand(kRequestTeam, 0, other)), "the cheater asks for the other (full) team");
    check(
        !cheater.net().submitCommand(kRequestRoundRestart, 0, {}), "a client's own world refuses a host-only command");
    {
        // Straight onto the wire: restart, and a command this game doesn't have.
        net::Command restart;
        restart.commandSeq = 1000;
        restart.commandId = kRequestRoundRestart;
        check(cheater.cheat->transport->inject(restart), "the cheater sends a restart anyway");
    }
    const uint64_t refusedBefore = host.net().stats().commandsRefused;
    loop.run(30);
    check(host.world.get<ArenaFighter>(cheatPawnHost)->team == cheatTeam, "a full team stays full");
    check(host.net().stats().commandsRefused > refusedBefore, "the host refused the host-only command");
    check(host.rules().phase == ArenaRules::Waiting, "and the round didn't move");

    // Damage, ownership and state can only come from the host.
    const Entity victim = host.pawn(2);
    const uint32_t victimLife = host.net().identity(victim)->incarnation;

    // ---- The round ----
    for (auto& m : machines) m->rules().autoPlay = true;   // everyone aims and holds fire
    net::ProfileValue yes;
    yes.fields[0].u = 1;
    for (auto& m : machines)
        if (m.get() != &cheater) check(bool(m->net().submitCommand(kReadyToPlay, 0, yes)), "every player says ready");
    {
        // The cheater's injected restart took sequence 1000, so its world's next command
        // would look like a replay: it says ready on the wire too.
        std::array<uint8_t, net::kMaxCommandPayloadBytes> payload{};
        uint32_t written = 0;
        check(
            bool(net::encodeFields(registry().command(kReadyToPlay)->payload, yes, payload, written)),
            "encode a ready payload");
        net::Command ready;
        ready.commandSeq = 1001;
        ready.commandId = kReadyToPlay;
        ready.payload = std::span(payload.data(), written);
        check(cheater.cheat->transport->inject(ready), "the cheater says ready");
    }
    check(
        loop.until([&] { return host.rules().phase == ArenaRules::Playing; }, 120),
        "the round starts once all are ready");
    check(host.rules().roundSerial >= 1, "a new round serial");
    // Fake inputs: speed hacking by sending four commands a tick, all sprinting forward,
    // plus a burst for the victim's life.
    const Entity cheatPawn = host.pawn(cheatSlot);
    check(host.world.alive(cheatPawn) && cheater.net().state() == NetworkState::Active, "the cheater is still playing");
    if (!host.world.alive(cheatPawn)) return;
    cheater.cheat->transport->filter = [](std::span<const uint8_t>) { return true; };
    const glm::vec3 cheatStart = host.world.get<Transform>(cheatPawn)->position;
    const uint32_t startTick = host.net().tick();
    for (int frame = 0; frame < 60; ++frame) {
        net::Inputs fake;
        fake.count = 4;
        const uint32_t life = host.net().identity(cheatPawn)->incarnation;
        for (uint32_t k = 0; k < 4; ++k) {
            InputCommand& c = fake.commands[k];
            c.targetTick = host.net().tick() + 2 + k;
            c.incarnation = life;
            c.moveZ = -127;
            c.heldBits = InputCommand::kHeldSprint;
        }
        cheater.cheat->transport->inject(fake);
        net::Inputs other;
        other.count = 1;
        other.commands[0].targetTick = host.net().tick() + 3;
        other.commands[0].incarnation = victimLife;
        other.commands[0].moveX = 127;
        cheater.cheat->transport->inject(other);
        loop.frame();
    }
    const uint32_t ticks = host.net().tick() - startTick;
    const auto& cheatFighter = *host.world.get<ArenaFighter>(cheatPawn);
    const float moved = glm::length(host.world.get<Transform>(cheatPawn)->position - cheatStart);
    check(
        moved <= cheatFighter.speed * kSprint * float(ticks + 2) / 60 + 0.25f,
        "four inputs a tick move a pawn no faster");
    check(cheater.net().state() == NetworkState::Active, "a fast sender of in-range inputs isn't punished");

    loop.run(600);   // ten seconds of autoplay
    const auto& r = host.rules();
    check(r.shots > 50 && r.hits > 10, "players fire and hit through Jolt");
    check(r.blue + r.orange > 0, "players are eliminated and score");
    check(host.net().stats().inputsUsed > 1000, "the host simulated the clients' inputs");
    // Clients run no rules; after a quiet moment they show the host's state.
    for (auto& m : machines) m->rules().autoPlay = false;
    loop.run(90);
    for (size_t i = 1; i < machines.size(); ++i) {
        Machine& m = *machines[i];
        check(m.rules().blue == host.rules().blue && m.rules().orange == host.rules().orange, "clients show the score");
        bool same = true;
        for (uint16_t slot = 1; slot <= 12; ++slot) {
            const Entity a = host.pawn(slot), b = m.pawn(slot);
            if (!host.world.alive(a) || !m.world.alive(b)) {
                same = false;
                continue;
            }
            const auto& fa = *host.world.get<ArenaFighter>(a);
            const auto& fb = *m.world.get<ArenaFighter>(b);
            const auto& wa = *host.world.get<ArenaWeapon>(a);
            const auto& wb = *m.world.get<ArenaWeapon>(b);
            same = same && fa.team == fb.team && fa.kills == fb.kills && fa.deaths == fb.deaths && wa.ammo == wb.ammo &&
                   std::abs(fa.health - fb.health) < 0.5f &&
                   glm::length(host.world.get<Transform>(a)->position - m.world.get<Transform>(b)->position) < 0.05f;
        }
        check(same, "client " + std::to_string(i) + " ends on the host's state: teams, ammo, health, kills");
    }

    // ---- Spoofing that gets a client sent away ----
    {
        const float before = host.world.get<ArenaFighter>(victim)->health;
        net::GameEvent shot;
        shot.eventSeq = 1;
        shot.eventId = 32;   // arena.shot_resolved, host to client only
        shot.subject = host.net().networkId(victim);
        std::array<uint8_t, 64> payload{};
        shot.payload = std::span(payload.data(), payload.size());
        cheater.cheat->transport->inject(shot);
        loop.run(10);
        check(cheater.net().state() == NetworkState::Failed, "a client claiming a shot is sent away");
        check(
            std::abs(host.world.get<ArenaFighter>(victim)->health - before) < 0.001f ||
                host.world.get<ArenaFighter>(victim)->health > before,
            "and does no damage");
        check(!host.world.alive(host.pawn(cheatSlot)), "the cheater's pawn is gone");
    }
    // A second cheater: ownership claims, then an unknown command.
    {
        Machine late(hub, 300, jobs, scene, true);
        loop.machines.push_back(&late);
        late.go(NetworkRole::Client);
        check(
            loop.until([&] { return late.net().state() == NetworkState::Active && late.pawns() == 12; }, 300),
            "a late joiner takes the free slot");
        const uint16_t lateSlot = late.net().localPlayer();
        const Entity target = host.pawn(2);
        net::Ownership take;
        take.controlSerial = 999;
        take.entity = host.net().networkId(target);
        take.ownerSlot = lateSlot;
        take.incarnation = 99;
        late.cheat->transport->inject(take);
        loop.run(10);
        check(late.net().state() == NetworkState::Failed, "a client claiming ownership is sent away");
        check(host.net().identity(target)->ownerSlot == 2, "and the pawn keeps its owner");
        loop.machines.pop_back();
    }
    {
        Machine late(hub, 301, jobs, scene, true);
        loop.machines.push_back(&late);
        late.go(NetworkRole::Client);
        loop.until([&] { return late.net().state() == NetworkState::Active; }, 300);
        net::Command unknown;
        unknown.commandSeq = 1;
        unknown.commandId = 77;
        late.cheat->transport->inject(unknown);
        loop.run(10);
        check(late.net().state() == NetworkState::Failed, "a command this game doesn't have is a protocol error");
        loop.machines.pop_back();
    }
    {
        // Inputs far in the future, every frame for a second: disconnected.
        Machine late(hub, 302, jobs, scene, true);
        loop.machines.push_back(&late);
        late.go(NetworkRole::Client);
        loop.until([&] { return late.net().state() == NetworkState::Active; }, 300);
        for (int frame = 0; frame < 120 && late.net().state() == NetworkState::Active; ++frame) {
            net::Inputs far;
            far.count = 4;
            for (uint32_t k = 0; k < 4; ++k) far.commands[k].targetTick = host.net().tick() + 1000 + k;
            late.cheat->transport->inject(far);
            loop.frame();
        }
        check(
            late.net().state() == NetworkState::Failed && late.net().failure() == ReasonCode::RateLimited,
            "inputs kept far ahead get the sender sent away");
        check(host.net().stats().inputsAhead > 120, "and none of them were used");
        loop.machines.pop_back();
    }
    {
        // A diagonal longer than the wire allows.
        Machine late(hub, 303, jobs, scene, true);
        loop.machines.push_back(&late);
        late.go(NetworkRole::Client);
        loop.until([&] { return late.net().state() == NetworkState::Active; }, 300);
        net::Inputs bad;
        bad.commands[0].targetTick = host.net().tick() + 3;
        bad.commands[0].moveX = 127;
        bad.commands[0].moveZ = 127;
        if (late.cheat->transport->inject(bad)) {
            loop.run(10);
            check(late.net().state() == NetworkState::Failed, "an over-long move is a protocol error");
        }
        loop.machines.pop_back();
    }
    for (auto& m : machines) m->session->stop();
}

// A client that stops sending: two ticks of its last input with fire off, then standing.
void missingInput(const World& scene) {
    net::LoopbackHub hub;
    JobSystem jobs(2);
    Machine host(hub, 100, jobs, scene, false), client(hub, 200, jobs, scene, true);
    Loop loop{hub, {&host, &client}};
    host.go(NetworkRole::Authority);
    client.go(NetworkRole::Client);
    check(
        loop.until([&] { return client.net().state() == NetworkState::Active && client.pawns() == 2; }, 300), "joined");
    net::ProfileValue yes;
    yes.fields[0].u = 1;
    check(host.net().submitCommand(kReadyToPlay, 0, yes).ok(), "host ready");
    check(client.net().submitCommand(kReadyToPlay, 0, yes).ok(), "client ready");
    loop.until([&] { return host.rules().phase == ArenaRules::Playing; }, 120);
    const Entity pawn = host.pawn(2);
    // The client's own inputs are replaced by "walk forward" for a second, then nothing.
    auto& transport = *client.cheat->transport;
    transport.filter = [](std::span<const uint8_t> bytes) {
        net::MessageHeader h;
        return !(net::decodeHeader(bytes, h) && h.kind == net::MessageKind::Inputs);
    };
    loop.run(70);   // past respawn protection and the start of the round
    for (int frame = 0; frame < 60; ++frame) {
        net::Inputs walk;
        walk.commands[0].targetTick = host.net().tick() + 3;
        walk.commands[0].incarnation = host.net().identity(pawn)->incarnation;
        walk.commands[0].moveZ = -127;
        walk.commands[0].heldBits = InputCommand::kHeldFire;
        transport.inject(walk);
        loop.frame();
    }
    const float walked = glm::length(host.world.get<CharacterController>(pawn)->desiredVelocity);
    check(walked > 1, "the host walks the pawn by its inputs");
    const uint64_t fallbacks = host.net().stats().inputFallbacks;
    loop.run(10);   // the last few sent are still on their way
    check(host.net().stats().inputFallbacks >= fallbacks + 4, "missing inputs fall back");
    check(
        glm::length(host.world.get<CharacterController>(pawn)->desiredVelocity) < 0.001f,
        "after two ticks without input the pawn stands still");
    check(
        !(host.world.get<ArenaFighter>(pawn)->input.heldBits & InputCommand::kHeldFire),
        "and fire is never held for it");
    host.session->stop();
    client.session->stop();
}

// ---- Frame rate --------------------------------------------------------------------------------

// The offline rules with the local sampler, at one frame rate: fire held for five seconds,
// one reload press, and a jump press, on the same 360 ticks.
struct Result {
    int shots = 0, ammo = 0, reloads = 0, jumps = 0, commandsWithReload = 0;
};

Result playAt(double fps) {
    World world;
    const Entity rules = world.create();
    ArenaRules settings;
    world.add<ArenaRules>(rules, settings);
    const Entity floor = world.create();
    world.add<Transform>(floor, {{0, -.5f, 0}});
    Collider ground;
    ground.halfExtents = {20, .5f, 20};
    world.add<Collider>(floor, ground);
    const Entity player = world.create();
    world.add<Transform>(player, {{0, .05f, 0}});
    world.add<CharacterController>(player);
    ArenaFighter human;
    human.player = true;
    human.team = 0;
    world.add<ArenaFighter>(player, human);
    ArenaWeapon gun;
    gun.magazine = 6;
    world.add<ArenaWeapon>(player, gun);
    JobSystem jobs(1);
    PhysicsWorld physics(world, jobs, nullptr);
    ArenaSim sim{world, physics, rules, 60, {}};
    sim.startRound();
    Result result;
    sim.hooks.reloaded = [&](Entity) { ++result.reloads; };
    InputSampler sampler;
    SimulationClock clock;
    const double tick = 1.0 / 60;
    int ticks = 0;
    double t = 0;
    bool pressedReload = false, pressedJump = false;
    while (ticks < 360) {
        const double frame = 1.0 / fps;
        t += frame;
        RawInput raw;
        raw.fire = t < 5.0;
        raw.mouse = {3, 0};   // turning: aim changes every frame
        // One press of each, early, on whatever frame gets there first.
        if (!pressedJump && t >= 0.2) raw.jumpPressed = pressedJump = true;
        if (!pressedReload && t >= 0.5) raw.reloadPressed = pressedReload = true;
        sampler.collect(raw, 0.1f);
        const int run = clock.advance(frame, frame, tick).ticks;
        for (int i = 0; i < run && ticks < 360; ++i, ++ticks) {
            InputCommand c = sampler.next(0);
            result.commandsWithReload += (c.pressedBits & InputCommand::kPressedReload) != 0;
            result.jumps += (c.pressedBits & InputCommand::kPressedJump) != 0;
            world.get<ArenaFighter>(player)->input = c;
            sim.stepControls(float(tick));
            physics.step(tick);
            sim.stepCombat();
        }
    }
    result.shots = world.get<ArenaRules>(rules)->shots;
    result.ammo = world.get<ArenaWeapon>(player)->ammo;
    return result;
}

void frameRateIndependence() {
    const Result base = playAt(60);
    check(base.shots > 10 && base.reloads >= 2, "the reference run fires and reloads");
    check(base.commandsWithReload == 1 && base.jumps == 1, "each press reaches exactly one command");
    for (double fps : {24.0, 30.0, 144.0, 240.0, 1000.0}) {
        const Result r = playAt(fps);
        const std::string at = " at " + std::to_string(int(fps)) + " fps";
        check(r.commandsWithReload == 1 && r.jumps == 1, "one reload and one jump" + at);
        // Fire is held in whole frames, so its end may move by a frame at low rates.
        check(std::abs(r.shots - base.shots) <= 1, "the same shots" + at);
        check(r.reloads == base.reloads, "the same reloads" + at);
    }
    // Several ticks in one frame (catch-up) still give a press to only the first.
    InputSampler sampler;
    RawInput raw;
    raw.reloadPressed = true;
    sampler.collect(raw, 0.1f);
    int presses = 0;
    for (int i = 0; i < 5; ++i) presses += (sampler.next(0).pressedBits & InputCommand::kPressedReload) != 0;
    check(presses == 1, "catch-up ticks don't replay a press");
    // A frame with no tick keeps the press for the next one.
    sampler.collect(RawInput{.jumpPressed = true}, 0.1f);
    sampler.collect(RawInput{}, 0.1f);
    check((sampler.next(0).pressedBits & InputCommand::kPressedJump) != 0, "a press survives a frame without ticks");
    // Menus: neutral, and presses are dropped.
    sampler.collect(RawInput{.moveX = 1, .fire = true, .reloadPressed = true, .active = false}, 0.1f);
    const InputCommand menu = sampler.next(0);
    check(menu.moveX == 0 && menu.heldBits == 0 && menu.pressedBits == 0, "an inactive frame is neutral");
    // Diagonals are normalized onto the wire's circle.
    sampler.collect(RawInput{.moveX = 1, .moveZ = 1}, 0.1f);
    check(!sampler.next(0).invalid(), "a diagonal stays within the wire's limit");
}

}   // namespace

int main(int argc, char** argv) {
    const fs::path game = argc > 1 ? fs::path(argv[1]) : fs::path("Games/Arena");
    registerTypes();
    net::registerNetworkComponents();
    Assets assets;
    prefabs().setup(game / "Content", &assets);
    World scene;
    try {
        if (!loadArenaScene(game, "arena-mp.swscene", assets, scene)) {
            check(false, "the multiplayer scene loads");
        } else {
            twelvePlayers(scene);
            missingInput(scene);
        }
        frameRateIndependence();
    } catch (const std::exception& e) {
        check(false, std::string("exception: ") + e.what());
    }
    if (failures) {
        std::fprintf(stderr, "network_arena: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    std::printf("network_arena: %d checks passed\n", checks);
    return 0;
}
