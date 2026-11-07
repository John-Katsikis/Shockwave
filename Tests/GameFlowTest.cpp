// Game code and circuits travelling (S05, docs/LevelLifetime.md) without a window: the
// game.flow() facade, game.arrival, and the Level.* circuit nodes in a game circuit:
//   - Level.RequestTravel's accepted and refused outputs, with the ticket and code;
//   - Level.OnTravelFinished runs once, in the level playing afterwards, with
//     Level.TravelStatus reading the ended ticket;
//   - a cancel before the boundary reaches the level that stays;
//   - Level.UseExit, Level.RestartLevel and Level.ReturnToMenu;
//   - a flow kept from an ended level, and a context without one, refuse.
#include <Assets/Assets.h>
#include <Circuits/Circuit.h>
#include <Circuits/CircuitWorld.h>
#include <Engine/Components.h>
#include <Engine/Levels.h>
#include <Engine/Prefab.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/PlaySession.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #x);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

const char* kGameCircuit = R"(shockwave_circuit 1
var finished Int 0
var arrivedLevel String ""
var arrivedFrom String ""
var lastCode String ""
var lastPhase String ""
var ticket Int 0
node done Level.OnTravelFinished
node count Set.finished
  exec done.then
  in value = inc.result
node inc Math.AddInt
  in a = finished
  in b = 1
node level Set.arrivedLevel
  exec count.then
  in value = done.level
node from Set.arrivedFrom
  exec level.then
  in value = done.from
node code Set.lastCode
  exec from.then
  in value = done.code
node status Level.TravelStatus
  in ticket = done.ticket
node phase Set.lastPhase
  exec code.then
  in value = status.phase
node go Event.Message
  in message = "go"
node travel Level.RequestTravel
  exec go.then
  in level = go.value
node keep Set.ticket
  exec travel.accepted
  in value = travel.ticket
node refused Set.lastCode
  exec travel.refused
  in value = travel.code
node toB Event.Message
  in message = "b"
node travelB Level.RequestTravel
  exec toB.then
  in level = "b"
  in spawn = "east"
node keepB Set.ticket
  exec travelB.accepted
  in value = travelB.ticket
node leave Event.Message
  in message = "exit"
node useExit Level.UseExit
  exec leave.then
  in exit = "exit0"
node restartMessage Event.Message
  in message = "restart"
node restart Level.RestartLevel
  exec restartMessage.then
node menuMessage Event.Message
  in message = "menu"
node menu Level.ReturnToMenu
  exec menuMessage.then
)";

std::string levelOf(const World& world) {
    std::string id;
    const_cast<World&>(world).each<const Name>([&](Entity, const Name& name) {
        if (name.value.rfind("level:", 0) == 0) id = name.value.substr(6);
    });
    return id;
}

LevelArrival lastArrival;
std::string flowLevel;
LevelFlow kept;   // from the first level, to try once it has ended

class Mode : public GameMode {
public:
    void beginPlay(GameContext& game) override {
        lastArrival = game.arrival;
        flowLevel = game.flow().level();
        CHECK(game.flow().available() && game.flow().epoch() == game.levelEpoch);
        if (!kept.available()) kept = game.flow();
    }
};

class Instance : public GameInstance {
public:
    std::unique_ptr<GameMode> createGameMode(const World&) override { return std::make_unique<Mode>(); }
};

void buildLevel(World& world, const std::string& id) {
    world.add<Name>(world.create(), Name{"level:" + id});
    for (const auto& [spawn, x] : std::vector<std::pair<std::string, float>>{{"start", 1.0f}, {"east", 5.0f}}) {
        const Entity e = world.create();
        world.add<Transform>(e, Transform{{x, 0, 0}});
        world.add<LevelSpawn>(e, LevelSpawn{spawn});
    }
    const Entity exit = world.create();
    world.add<Name>(exit, Name{"exit0"});
    world.add<LevelExit>(exit, LevelExit{"b", "east", true, true});
}

const Value* var(LevelHost& host, const char* name) {
    CircuitWorld* circuits = host.session() ? host.session()->circuits() : nullptr;
    return circuits ? circuits->variable(kNullEntity, name) : nullptr;
}
int intVar(LevelHost& host, const char* name) {
    const Value* v = var(host, name);
    return v ? v->i : -1;
}
std::string textVar(LevelHost& host, const char* name) {
    const Value* v = var(host, name);
    return v ? v->text : std::string("<none>");
}

// A message to the game circuit, run by the next tick.
void say(LevelHost& host, const std::string& message, const std::string& value = {}) {
    CHECK(host.session()->circuits()->send(kNullEntity, message, kNullEntity, 0.0f, value));
    host.session()->update(1.0 / 60.0);
}

void across(LevelHost& host) {
    host.process();   // prepared, at the boundary
    host.process();   // across
}

void testCircuits(const fs::path& content) {
    {
        fs::create_directories(content / "Circuits");
        std::ofstream(content / "Circuits/game.swcircuit", std::ios::binary) << kGameCircuit;
    }
    World world;
    JobSystem jobs(1);
    Instance instance;
    LevelHost::Hooks hooks;
    hooks.resolve = [](TravelRequest& request, TravelError& code, std::string& error) {
        if (request.level == "a" || request.level == "b" || request.level == "menu") return true;
        code = TravelError::UnknownLevel;
        error = "no level \"" + request.level + "\"";
        return false;
    };
    hooks.load = [](const LevelRequest& request, World& into, std::string&) {
        buildLevel(into, request.level);
        return true;
    };
    hooks.configure = [](PlaySession& session, const LevelRequest&) {
        session.setGameCircuit("Circuits/game.swcircuit");
    };
    LevelHost host(world, jobs, instance, nullptr, hooks);
    host.setStaged(true);
    host.setMenuLevel("menu");
    buildLevel(world, "a");
    host.start({"a"});
    const std::vector<std::string> problems = host.session()->circuits()->diagnostics();
    CHECK(problems.empty());
    for (const auto& p : problems) std::fprintf(stderr, "  %s\n", p.c_str());
    CHECK(flowLevel == "a" && lastArrival.found && lastArrival.spawn == "start");
    host.session()->update(1.0 / 60.0);

    // Accepted: the ticket comes out; the level that asked never hears how it ended.
    say(host, "go", "b");
    const int ticket = intVar(host, "ticket");
    CHECK(ticket > 0 && uint64_t(ticket) == host.activeTicket());
    host.process();
    CHECK(intVar(host, "finished") == 0);   // waiting at the boundary: nothing delivered
    host.process();
    CHECK(levelOf(world) == "b" && flowLevel == "b");
    CHECK(intVar(host, "finished") == 1 && textVar(host, "arrivedLevel") == "b" && textVar(host, "arrivedFrom") == "a");
    CHECK(textVar(host, "lastCode") == "None" && textVar(host, "lastPhase") == "succeeded");

    // Refused: the refused output, with the code; nothing is delivered.
    host.session()->update(1.0 / 60.0);
    say(host, "go", "nowhere");
    CHECK(textVar(host, "lastCode") == "UnknownLevel" && intVar(host, "finished") == 1 && !host.activeTicket());

    // A literal level and spawn.
    say(host, "b");
    across(host);
    CHECK(lastArrival.spawn == "east" && lastArrival.found && intVar(host, "finished") == 1);

    // Cancelled at the boundary: the level that stays hears it, once.
    host.session()->update(1.0 / 60.0);
    say(host, "go", "a");
    const int cancelled = intVar(host, "ticket");
    host.process();
    CHECK(host.cancel(uint64_t(cancelled)) == TravelError::None);
    CHECK(intVar(host, "finished") == 2 && textVar(host, "lastCode") == "Cancelled");
    CHECK(textVar(host, "lastPhase") == "cancelled" && levelOf(world) == "b");

    // An exit, a restart and the menu, from circuits.
    say(host, "exit");
    CHECK(host.activeTicket() != 0);
    across(host);
    CHECK(levelOf(world) == "b" && lastArrival.spawn == "east" && textVar(host, "arrivedFrom") == "b");
    host.session()->update(1.0 / 60.0);
    say(host, "restart");
    across(host);
    CHECK(levelOf(world) == "b" && lastArrival.spawn == "east" && intVar(host, "finished") == 1);
    host.session()->update(1.0 / 60.0);
    say(host, "menu");
    across(host);
    CHECK(levelOf(world) == "menu" && textVar(host, "arrivedLevel") == "menu");

    // A flow kept from the first level is stale now; no flow at all can't travel.
    CHECK(kept.request("a").error == TravelError::StaleLevel);
    CHECK(kept.restartLevel().error == TravelError::StaleLevel);
    CHECK(LevelFlow().request("a").error == TravelError::NotPlaying && !LevelFlow().available());
    host.stop();
}

}   // namespace

int main() {
    registerCircuitComponents();
    const fs::path root =
        fs::temp_directory_path() /
        ("shockwave-game-flow-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path content = root / "Content";
    fs::create_directories(content);
    Assets assets;
    prefabs().setup(content, &assets);
    testCircuits(content);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::fprintf(stderr, "game_flow: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("game_flow: all checks passed\n");
    return 0;
}
