// Level travel's state machine (S05, docs/LevelLifetime.md, section 6 of the shipping plan)
// without a window: LevelHost as a GameFlow, staged over two frames and instant.
//   - tickets, phases and honest progress: monotonic, 1 only once it succeeded;
//   - A -> B -> A, an explicit spawn, restart, return to menu;
//   - refused requests with their codes: Busy, UnknownLevel, StaleLevel, InvalidRequest;
//   - failures before the boundary leave the current level playing, untouched; after it,
//     nothing plays and failure() says why;
//   - cancel before the boundary, TooLateToCancel after, AlreadyFinished and UnknownTicket;
//   - two exits touched in one frame travel once, and a one-shot exit is used once;
//   - Stop while queued, reading and waiting at the boundary;
//   - every ticket completes exactly once, after the change, and history is bounded.
#include <Engine/Components.h>
#include <Engine/Levels.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/PlaySession.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace shockwave;

namespace {

// A request from the application, by level and spawn.
TravelRequest to(std::string level, std::string spawn = {}) {
    TravelRequest r;
    r.level = std::move(level);
    r.spawn = std::move(spawn);
    return r;
}

int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #x);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

struct Counts {
    int begun = 0, ended = 0;
};
std::map<std::string, Counts> counts;
std::map<std::string, std::function<void(GameContext&)>> onBegin;   // per level
LevelArrival lastArrival;
const GameContext* lastContext = nullptr;

std::string levelOf(const World& world) {
    std::string id;
    const_cast<World&>(world).each<const Name>([&](Entity, const Name& name) {
        if (name.value.rfind("level:", 0) == 0) id = name.value.substr(6);
    });
    return id;
}

class Mode : public GameMode {
public:
    explicit Mode(std::string level) : m_level(std::move(level)) {}
    void beginPlay(GameContext& game) override {
        ++counts[m_level].begun;
        lastArrival = game.arrival;
        lastContext = &game;
        if (m_level == "boom") throw std::runtime_error("beginPlay of boom threw");
        if (const auto it = onBegin.find(m_level); it != onBegin.end() && it->second) it->second(game);
    }
    void endPlay(GameContext& game) override {
        ++counts[m_level].ended;
        if (lastContext == &game) lastContext = nullptr;
    }

private:
    std::string m_level;
};

class Instance : public GameInstance {
public:
    std::unique_ptr<GameMode> createGameMode(const World& scene) override {
        return std::make_unique<Mode>(levelOf(scene));
    }
};

// A level: a marker, a spawn "start" at x = 1 and one "east" at x = 5, and two exits to b.
void buildLevel(World& world, const std::string& id) {
    world.add<Name>(world.create(), Name{"level:" + id});
    for (const auto& [spawn, x] : std::vector<std::pair<std::string, float>>{{"start", 1.0f}, {"east", 5.0f}}) {
        const Entity e = world.create();
        world.add<Transform>(e, Transform{{x, 0, 0}});
        world.add<LevelSpawn>(e, LevelSpawn{spawn});
    }
    for (int i = 0; i < 2; ++i) {
        const Entity e = world.create();
        world.add<Name>(e, Name{"exit" + std::to_string(i)});
        world.add<LevelExit>(e, LevelExit{"b", "east", true, i == 0});
    }
}

Entity exitOf(const World& world, int i) {
    Entity found = kNullEntity;
    const_cast<World&>(world).each<const Name>([&](Entity e, const Name& name) {
        if (name.value == "exit" + std::to_string(i)) found = e;
    });
    return found;
}

int loadsOf = 0;
std::function<void()> duringLoad;   // runs inside the load hook (Stop while reading)

LevelHost::Hooks hooks() {
    LevelHost::Hooks h;
    h.resolve = [](TravelRequest& request, TravelError& code, std::string& error) {
        static const std::vector<std::string> known = {"menu", "a", "b", "boom", "broken", "pricey"};
        for (const auto& id : known)
            if (id == request.level) return true;
        code = TravelError::UnknownLevel;
        error = "no level \"" + request.level + "\"";
        return false;
    };
    h.load = [](const LevelRequest& request, World& into, std::string& error) {
        ++loadsOf;
        if (duringLoad) duringLoad();
        if (request.level == "broken") {
            error = "Scenes/broken.swscene doesn't parse";
            return false;
        }
        buildLevel(into, request.level);
        return true;
    };
    return h;
}

void tick(LevelHost& host) {
    if (host.session()) host.session()->update(1.0 / 60.0);
}

// Every ticket ends once, and after the change: the observer sees the level it left for.
struct Observer {
    std::map<uint64_t, int> seen;
    std::vector<TravelCompletion> all;
    LevelHost* host = nullptr;
    void operator()(const TravelCompletion& c) {
        ++seen[c.ticket];
        all.push_back(c);
        if (c.status == TravelStatus::Succeeded)
            CHECK(host->playing() && host->currentLevel().level == c.request.level && !host->activeTicket());
    }
};

void testStaged() {
    World world;
    JobSystem jobs(1);
    Instance instance;
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.setStaged(true);
    host.setMenuLevel("menu");
    Observer observer;
    observer.host = &host;
    auto subscription = host.subscribe([&](const TravelCompletion& c) { observer(c); });
    buildLevel(world, "a");
    host.start({"a"});
    CHECK(lastArrival.found && lastArrival.spawn == "start" && std::abs(lastArrival.transform.position.x - 1) < 1e-4f);
    tick(host);

    // A -> B, staged: prepared and paused at the boundary first, then across.
    const TravelTicket t = host.request(to("b", "east"));
    CHECK(t && t.error == TravelError::None && host.activeTicket() == t.id);
    auto s = host.status(t.id);
    CHECK(s && s->phase == TravelPhase::Queued && s->progress == 0 && s->cancellable && s->from == "a");
    host.process();
    s = host.status(t.id);
    CHECK(s && s->phase == TravelPhase::AwaitingBoundary && std::abs(s->progress - 0.45f) < 1e-4f && s->cancellable);
    CHECK(host.currentLevel().level == "a" && host.transitioning() && counts["a"].ended == 0);
    CHECK(host.session()->state() == PlaySession::State::Paused);   // a frame renders, the level waits
    CHECK(observer.all.empty());
    const float before = s->progress;
    host.process();
    s = host.status(t.id);
    CHECK(s && s->phase == TravelPhase::Succeeded && s->progress == 1.0f && s->progress >= before);
    CHECK(s->code == TravelError::None && !s->cancellable && s->durationMs >= 0 && std::isfinite(s->durationMs));
    CHECK(host.currentLevel() == (LevelRequest{"b", "east"}) && counts["a"].ended == 1 && counts["b"].begun == 1);
    CHECK(lastArrival.found && lastArrival.spawn == "east" && std::abs(lastArrival.transform.position.x - 5) < 1e-4f);
    CHECK(observer.seen[t.id] == 1 && observer.all.back().from == "a" && observer.all.back().to == "b");
    CHECK(host.cancel(t.id) == TravelError::AlreadyFinished && host.cancel(9999) == TravelError::UnknownTicket);

    // Refused, each with its code; nothing is queued.
    tick(host);
    CHECK(host.request(to("nowhere")).error == TravelError::UnknownLevel);
    CHECK(host.request(to("")).error == TravelError::InvalidRequest);
    TravelRequest reset = to("menu");
    reset.resetCampaign = true;
    CHECK(host.request(reset).error == TravelError::InvalidRequest);   // only a new game resets
    TravelRequest stale = to("a");
    stale.fromEpoch = host.epoch() - 1;
    CHECK(host.request(stale).error == TravelError::StaleLevel);
    CHECK(!host.transitioning() && !host.activeTicket());

    // A UI action fired twice in one frame: one ticket, the second is Busy.
    const TravelTicket first = host.request(to("a"));
    const TravelTicket second = host.request(to("a"));
    CHECK(first && !second && second.error == TravelError::Busy && host.activeTicket() == first.id);
    // Cancelled while queued, and again at the boundary: B plays on, untouched.
    CHECK(host.cancel(first.id) == TravelError::None && !host.transitioning());
    CHECK(
        host.status(first.id)->phase == TravelPhase::Cancelled &&
        host.status(first.id)->code == TravelError::Cancelled);
    const uint64_t epoch = host.epoch();
    const TravelTicket third = host.request(to("a"));
    host.process();
    CHECK(host.status(third.id)->phase == TravelPhase::AwaitingBoundary);
    CHECK(host.cancel(third.id) == TravelError::None);
    CHECK(host.session()->state() == PlaySession::State::Playing && host.epoch() == epoch);
    CHECK(host.currentLevel().level == "b" && counts["b"].ended == 0 && !host.transitioning());
    host.process();   // nothing left to do
    CHECK(host.currentLevel().level == "b" && host.epoch() == epoch);

    // A new game may reset the campaign; restart keeps the spawn; menu goes to the menu.
    TravelRequest newGame = to("a");
    newGame.reason = TravelReason::NewGame;
    newGame.resetCampaign = true;
    const TravelTicket fresh = host.request(newGame);
    CHECK(fresh);
    host.process();
    host.process();
    CHECK(host.currentLevel().level == "a" && lastArrival.spawn == "start");
    tick(host);
    const TravelTicket again = host.restartLevel();
    CHECK(again && host.status(again.id)->request.reason == TravelReason::Restart);
    host.process();
    host.process();
    CHECK(host.currentLevel().level == "a" && counts["a"].begun == 3);
    tick(host);
    const TravelTicket menu = host.returnToMenu();
    CHECK(menu && host.status(menu.id)->request.reason == TravelReason::Menu);
    host.process();
    host.process();
    CHECK(host.currentLevel().level == "menu");

    // A user's pause stays theirs: travel from a paused level, and a cancel leaves it paused.
    tick(host);
    CHECK(host.session()->pause());
    const TravelTicket paused = host.request(to("a"));
    host.process();
    CHECK(host.cancel(paused.id) == TravelError::None && host.session()->state() == PlaySession::State::Paused);
    const TravelTicket fromPause = host.request(to("a"));
    host.process();
    host.process();
    CHECK(
        host.status(fromPause.id)->phase == TravelPhase::Succeeded &&
        host.session()->state() == PlaySession::State::Playing);

    // Too late to cancel once it has crossed: from the new level's beginPlay.
    tick(host);
    uint64_t crossing = 0;
    TravelError late = TravelError::None;
    onBegin["b"] = [&](GameContext&) { late = host.cancel(crossing); };
    crossing = host.request(to("b")).id;
    host.process();
    host.process();
    onBegin["b"] = nullptr;
    CHECK(late == TravelError::TooLateToCancel && host.status(crossing)->phase == TravelPhase::Succeeded);

    // Two exits touched in one frame travel once; a one-shot exit is used once.
    tick(host);
    LevelFlow flow = lastContext->flow();
    const TravelTicket viaExit = flow.useExit(exitOf(world, 0));
    const TravelTicket twice = flow.useExit(exitOf(world, 1));
    CHECK(viaExit && twice.error == TravelError::Busy);
    CHECK(!world.get<LevelExit>(exitOf(world, 0))->enabled && world.get<LevelExit>(exitOf(world, 1))->enabled);
    CHECK(host.cancel(viaExit.id) == TravelError::None);
    CHECK(flow.useExit(exitOf(world, 0)).error == TravelError::InvalidRequest);   // used up
    CHECK(flow.useExit(Entity{12345, 0}).error == TravelError::InvalidRequest);
    const TravelTicket second2 = flow.useExit(exitOf(world, 1));   // not one-shot: still there afterwards
    CHECK(second2 && world.get<LevelExit>(exitOf(world, 1))->enabled);
    host.process();
    host.process();
    CHECK(host.currentLevel() == (LevelRequest{"b", "east"}));
    // The old level's flow can't travel any more.
    CHECK(flow.request("a").error == TravelError::StaleLevel);

    // Before the boundary, failures leave the level playing: a missing spawn, a broken
    // scene (the menu, say), a budget that doesn't fit.
    tick(host);
    const uint64_t stayed = host.epoch();
    const TravelTicket noSpawn = host.request(to("a", "west"));
    host.process();
    CHECK(
        host.status(noSpawn.id)->phase == TravelPhase::Failed &&
        host.status(noSpawn.id)->code == TravelError::UnknownSpawn);
    CHECK(host.playing() && host.epoch() == stayed && host.session()->state() == PlaySession::State::Playing);
    const TravelTicket broken = host.request(to("broken"));
    host.process();
    CHECK(host.status(broken.id)->code == TravelError::LoadFailed && host.epoch() == stayed && host.playing());
    CHECK(host.status(broken.id)->message.find("doesn't parse") != std::string::npos);
    CHECK(observer.all.back().to == "b");   // where it stayed

    // After the boundary: the new level didn't start, so nothing plays.
    const TravelTicket boom = host.request(to("boom"));
    host.process();
    host.process();
    CHECK(host.status(boom.id)->code == TravelError::StartFailed && !host.playing());
    CHECK(host.failure().find("beginPlay of boom threw") != std::string::npos);
    CHECK(observer.all.back().to.empty());
    // Nothing plays, but the application can still go somewhere (the error screen's Retry
    // and Main menu); the level that didn't start can't.
    TravelRequest fromBoom = to("a");
    fromBoom.fromEpoch = host.epoch();
    CHECK(host.request(fromBoom).error == TravelError::StaleLevel);
    const TravelTicket recover = host.request(to("a"));
    CHECK(recover.id != 0);
    host.process();
    host.process();
    CHECK(host.playing() && host.current().level == "a" && host.failure().empty());
    CHECK(host.status(recover.id)->phase == TravelPhase::Succeeded);
    host.stop();
    CHECK(host.request(to("a")).error == TravelError::NotPlaying);

    // Every ticket ended exactly once; subscriptions end with their token.
    for (const auto& [id, n] : observer.seen) CHECK(n == 1);
    const size_t heard = observer.all.size();
    subscription.reset();
    world = World{};
    buildLevel(world, "a");
    host.start({"a"});
    tick(host);
    host.request(to("broken"));
    host.process();
    CHECK(observer.all.size() == heard);
    host.stop();
}

void testStop() {
    World world;
    JobSystem jobs(1);
    Instance instance;
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.setStaged(true);
    int completions = 0;
    auto subscription = host.subscribe([&](const TravelCompletion& c) {
        ++completions;
        CHECK(c.status == TravelStatus::Cancelled && c.code == TravelError::Cancelled);
    });
    auto cycle = [&](int stage) {
        world = World{};
        buildLevel(world, "a");
        host.start({"a"});
        tick(host);
        const TravelTicket t = host.request(to("b"));
        if (stage == 1) duringLoad = [&] { host.stop(); };
        if (stage >= 1) host.process();   // 1: Stop while reading; 2: waiting at the boundary
        duringLoad = nullptr;
        host.stop();
        CHECK(!host.playing() && host.status(t.id) && host.status(t.id)->phase == TravelPhase::Cancelled);
        CHECK(counts["b"].begun == 0);
    };
    counts.clear();
    for (int stage = 0; stage < 3; ++stage) cycle(stage);
    CHECK(completions == 3);
    CHECK(counts["a"].begun == 3 && counts["a"].ended == 3);
}

void testInstantAndHistory() {
    World world;
    JobSystem jobs(1);
    Instance instance;
    LevelHost host(world, jobs, instance, nullptr, hooks());   // unstaged: as in S03
    buildLevel(world, "a");
    host.start({"a"});
    tick(host);
    const TravelTicket t = host.request(to("b"));
    host.process();
    CHECK(host.status(t.id)->phase == TravelPhase::Succeeded && host.currentLevel().level == "b");
    // 70 more: the first ones are forgotten, the last 64 kept.
    uint64_t firstId = 0, lastId = 0;
    for (int i = 0; i < 70; ++i) {
        tick(host);
        const TravelTicket next = host.request(to(i % 2 ? "b" : "a"));
        if (!firstId) firstId = next.id;
        lastId = next.id;
        host.process();
    }
    CHECK(!host.status(firstId) && host.cancel(firstId) == TravelError::UnknownTicket);
    CHECK(host.status(lastId) && host.status(lastId)->phase == TravelPhase::Succeeded);
    CHECK(host.status(lastId - LevelHost::kKeptTickets + 1).has_value());
    CHECK(!host.status(lastId - LevelHost::kKeptTickets).has_value());
    // A redirect from beginPlay isn't Busy: it runs right after, in the same process().
    onBegin["menu"] = [](GameContext& game) { CHECK(game.requestLevel("a")); };
    tick(host);
    host.request(to("menu"));
    host.process();
    onBegin["menu"] = nullptr;
    CHECK(host.currentLevel().level == "a");
    host.stop();
}

void testSubscriptionOutlivesHost() {
    GameFlow::Subscription kept;
    {
        World world;
        JobSystem jobs(1);
        Instance instance;
        LevelHost host(world, jobs, instance, nullptr, hooks());
        kept = host.subscribe([](const TravelCompletion&) {});
        CHECK(kept.active());
    }
    CHECK(!kept.active());
    kept.reset();   // nothing to do, and safe
}

}   // namespace

int main() {
    testStaged();
    testStop();
    testInstantAndHistory();
    testSubscriptionOutlivesHost();
    if (failures) {
        std::fprintf(stderr, "level_travel: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("level_travel: all checks passed\n");
    return 0;
}
