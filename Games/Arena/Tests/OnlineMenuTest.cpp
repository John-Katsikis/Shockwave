// Batch 9 gate for Arena's online menu (Source/ArenaOnlineMenu.cpp) through the real RmlUi
// document (Content/UI/Arena/menu.rml): every button is clicked and calls the shared
// OnlineMatch API over the Steam mock with a real host session; the menu shows searching,
// the lobby list, a refused lobby with its reason, joining, cancel, retry, a friend's
// invite and the greyed-out section when the application offers no match control.
#include "ArenaOnlineMenu.h"

#include <Multiplayer/NetworkWorld.h>
#include <Multiplayer/OnlineMatch.h>
#include <Multiplayer/Replication.h>
#include <SteamServices/MockSteamApi.h>
#include <SteamServices/SteamServices.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace shockwave;
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

static constexpr uint64_t kHostId = 76561197960287930ull, kPlayerId = 76561197960287931ull,
                          kOtherId = 76561197960287932ull;

static const ReplicationRegistry& registry() {
    static ReplicationRegistry r = [] {
        ReplicationRegistry made;
        registerEngineReplication(made);
        std::vector<std::string> problems;
        made.freeze(problems);
        return made;
    }();
    return r;
}

static MatchSetup setup(uint8_t content = 0x5A) {
    MatchSetup s;
    s.game = "arena";
    s.settings.enabled = true;
    s.settings.transport = NetTransportKind::Steam;
    s.settings.virtualPort = 7;
    s.settings.maxPlayers = 12;
    s.compatibility.gameProtocol = "arena-1";
    s.compatibility.schemaDigest = registry().schemaDigest();
    s.compatibility.contentDigest.fill(content);
    s.compatibility.sceneId = 2;
    s.scenePath = "Scenes/arena-mp.swscene";
    s.registry = &registry();
    return s;
}

// One machine: Steam (mocked), its match, and the session the application would play.
struct Machine {
    MockSteamApi* mock = nullptr;
    std::unique_ptr<SteamServices> services;
    OnlineMatch match;
    World world;
    std::unique_ptr<NetworkWorld> session;
    bool signedIn = true;

    Machine(MockSteamNetwork& network, uint64_t steamId, MatchSetup s) {
        auto api = network.makeApi(steamId);
        mock = api.get();
        services = std::make_unique<SteamServices>(std::move(api));
        SteamServicesSettings steam;
        steam.appId = 480;
        steam.virtualPort = 7;
        CHECK(bool(services->initializeClient(steam)));
        match.setSetup(std::move(s));
        match.setServiceProvider([this](std::string& error) -> IOnlineServices* {
            if (!signedIn) error = "Steam isn't running";
            return signedIn ? services.get() : nullptr;
        });
    }
    ~Machine() {
        if (session) session->close();
    }
    void frame(double now, uint32_t tick) {
        services->framePass();
        OnlineEvent event;
        while (services->pollEvent(event))
            if (!match.handleOnlineEvent(event) && session) session->handleOnlineEvent(event);
        if (session) {
            session->pump(now);
            session->drain(tick);
            session->endTick(tick);
        }
        match.update(session.get());
        if (match.takeEnded() && session) {
            session->close();
            session.reset();
        }
        NetworkStart start;
        if (match.takeStart(start)) {
            if (session) session->close();
            session = std::make_unique<NetworkWorld>(start, services.get(), &world);
            CHECK(bool(session->begin()));
        }
    }
};

// The menu document and the game context Arena's menu sees.
struct Menu {
    GameUI ui{nullptr};
    World world;
    Entity document;
    JobSystem jobs{1};
    SubsystemCollection subsystems;
    UIWorld* controls = nullptr;
    GameContext game;
    arena::OnlineMenu menu;

    Menu(const fs::path& project, OnlineMatch* match) : game{world, jobs, nullptr, subsystems} {
        ui.setContentFolder(project / "Content");
        document = world.create();
        world.add<Name>(document, {"Arena Menu"});
        world.add<UIDocument>(document, {"UI/Arena/menu.rml", true, 10, true, {}});
        controls = &subsystems.add<UIWorld>(world, &ui);
        subsystems.add<NetworkWorld>();
        game.onlineMatch = match;
        menu.bind(game);
        draw();
        for (const auto& problem : ui.problems(document)) std::fprintf(stderr, "%s\n", problem.message.c_str());
        CHECK(ui.problems(document).empty());
    }
    void draw() {
        menu.refresh(game);
        ui.update(world, 0);
        ui.layout();
    }
    bool shown(const char* id) { return ui.elementVisible(document, id); }
    bool click(const char* id) {
        const bool clicked = shown(id) && controls->click("Arena Menu", id);
        draw();
        return clicked;
    }
    std::string text(const char* id) { return controls->text("Arena Menu", id); }
};

struct Sim {
    MockSteamNetwork network;
    std::vector<Machine*> machines;
    Menu* menu = nullptr;
    uint32_t tick = 0;
    template <class F>
    bool until(F done, uint32_t maxTicks = 300) {
        for (uint32_t i = 0; i < maxTicks && !done(); ++i) {
            const double now = double(network.nowUs()) / 1e6;
            for (Machine* m : machines) m->frame(now, tick);
            if (menu) menu->draw();
            network.advance(1000000 / 60);
            ++tick;
        }
        return done();
    }
};

static void testNoMatchControl(const fs::path& project) {
    Menu menu(project, nullptr);
    CHECK(menu.text("online-status").find("ISN'T AVAILABLE") != std::string::npos);
    CHECK(menu.shown("host-off") && menu.shown("find-off"));
    CHECK(!menu.shown("host") && !menu.shown("find") && !menu.shown("leave"));
    CHECK(menu.shown("resume") && menu.shown("graphics"));   // the offline menu is untouched
    CHECK(!menu.click("host"));
}

static void testFindJoinCancel(const fs::path& project) {
    Sim sim;
    Machine host(sim.network, kHostId, setup()), stranger(sim.network, kOtherId, setup(0x11));
    Machine player(sim.network, kPlayerId, setup());
    Menu menu(project, &player.match);
    sim.machines = {&host, &stranger, &player};
    sim.menu = &menu;
    CHECK(menu.shown("host") && menu.shown("find") && !menu.shown("host-off"));
    CHECK(menu.text("online-status") == "OFFLINE");
    CHECK(!menu.shown("lobby-1") && !menu.shown("cancel") && !menu.shown("retry"));

    CHECK(host.match.host() && stranger.match.host());
    CHECK(sim.until(
        [&] { return host.match.state() == MatchState::Hosting && stranger.match.state() == MatchState::Hosting; }));

    CHECK(menu.click("find"));
    CHECK(player.match.searching());
    CHECK(menu.text("online-status") == "SEARCHING FOR MATCHES");
    CHECK(sim.until([&] { return !player.match.searching(); }));
    CHECK(player.match.lobbies().size() == 2);
    CHECK(menu.text("online-status") == "2 MATCHES FOUND");
    CHECK(menu.shown("lobby-1") && menu.shown("lobby-2") && !menu.shown("lobby-3"));
    int good = -1, bad = -1;
    for (int i = 0; i < int(player.match.lobbies().size()); ++i)
        (player.match.lobbies()[size_t(i)].compatible ? good : bad) = i;
    CHECK(good >= 0 && bad >= 0);
    if (good < 0 || bad < 0) return;
    const std::string goodId = "lobby-" + std::to_string(good + 1), badId = "lobby-" + std::to_string(bad + 1);
    CHECK(menu.text(goodId.c_str()).find("MOCK 930  1/12  WAITING") == 0);
    CHECK(menu.text(badId.c_str()).find("CAN'T JOIN: ") != std::string::npos);
    CHECK(menu.click(badId.c_str()));   // refused in the menu: nothing is requested
    CHECK(player.match.state() == MatchState::Idle);

    // Join, then cancel while connecting: back to offline with the list again.
    CHECK(menu.click(goodId.c_str()));
    CHECK(player.match.state() == MatchState::JoiningLobby);
    CHECK(menu.shown("cancel"));
    CHECK(!menu.shown("host"));
    CHECK(!menu.shown("lobby-1"));
    CHECK(menu.text("online-status").find("JOINING LOBBY") == 0);
    CHECK(menu.click("cancel"));
    CHECK(player.match.state() == MatchState::Idle && !player.session);
    CHECK(menu.shown("host") && !menu.shown("cancel"));

    // Join for real: connecting, then in the match with Leave.
    CHECK(menu.click(goodId.c_str()));
    CHECK(sim.until([&] { return player.match.state() == MatchState::Joined; }));
    CHECK(player.session && player.session->state() == NetworkState::Active);
    CHECK(menu.shown("leave") && !menu.shown("invite") && !menu.shown("host"));
    CHECK(menu.text("online-status").find("PLAYING AS PLAYER 2") == 0);

    // A friend's invite while playing waits for an answer; Decline keeps the match.
    player.mock->injectJoinRequest(stranger.match.lobby().value);
    CHECK(sim.until([&] { return player.match.pendingJoin().valid(); }, 10));
    CHECK(menu.shown("accept-invite") && menu.shown("decline-invite"));
    CHECK(menu.click("decline-invite"));
    CHECK(!player.match.pendingJoin().valid() && player.match.state() == MatchState::Joined);
    CHECK(!menu.shown("accept-invite"));

    // Leave: the application would stop Play and go back to the offline scene.
    CHECK(menu.click("leave"));
    CHECK(player.match.state() == MatchState::Idle);
    CHECK(player.match.takeEnded());
}

static void testHostFailRetry(const fs::path& project) {
    Sim sim;
    Machine player(sim.network, kPlayerId, setup());
    Menu menu(project, &player.match);
    sim.machines = {&player};
    sim.menu = &menu;
    // Steam signs out: Host fails with the reason, and Retry asks again.
    player.signedIn = false;
    CHECK(menu.click("host"));
    CHECK(player.match.state() == MatchState::Idle && !player.match.error().empty());
    CHECK(menu.text("online-status").find("CAN'T HOST: STEAM ISN'T RUNNING") == 0);
    CHECK(menu.shown("retry") && menu.shown("host"));
    player.signedIn = true;
    CHECK(menu.click("retry"));
    CHECK(player.match.state() == MatchState::CreatingLobby);
    CHECK(menu.shown("cancel") && !menu.shown("retry"));
    CHECK(sim.until([&] { return player.match.state() == MatchState::Hosting; }));
    CHECK(menu.shown("invite") && menu.shown("leave"));
    CHECK(menu.text("online-status").find("HOSTING LOBBY") == 0);
    CHECK(menu.click("invite"));
    CHECK(player.mock->counts().invites == 1);
    // The panels fit a 720p window side by side, offline and in the match scene.
    menu.ui.setSize({1280, 720});
    for (const bool matchScene : {false, true}) {
        menu.controls->set("arena_match", matchScene);
        menu.draw();
        std::vector<glm::vec4> panels;
        for (const char* id : {"menu-panel", "online-panel", "match-panel"}) {
            if (!menu.shown(id)) continue;
            const auto panel = menu.ui.elementRect(menu.document, id);
            CHECK(panel && panel->x >= 0 && panel->y >= 0 && panel->x + panel->z <= 1280 && panel->y + panel->w <= 720);
            if (panel) panels.push_back(*panel);
        }
        CHECK(panels.size() == (matchScene ? 3u : 2u));
        for (size_t a = 0; a < panels.size(); ++a)
            for (size_t b = a + 1; b < panels.size(); ++b)
                CHECK(panels[a].x + panels[a].z <= panels[b].x || panels[b].x + panels[b].z <= panels[a].x);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const fs::path project = argv[1];
    testNoMatchControl(project);
    testFindJoinCancel(project);
    testHostFailRetry(project);
    if (failures)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    else
        std::printf("arena online menu: all checks passed\n");
    return failures ? 1 : 0;
}
