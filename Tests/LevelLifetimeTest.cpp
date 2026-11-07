// Level lifetime (S03, docs/LevelLifetime.md) without a window: LevelHost travel between
// levels built in memory, counted lifecycles, requests from every kind of callback,
// failures while loading and starting, stale requests and EntityRefs from an earlier
// level, redirect cycles, and editor-style Play/A/B/Stop cycles that leave the authored
// scene exactly as it was.
#include <Engine/Components.h>
#include <Engine/EntityRef.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/LevelHost.h>
#include <GameFramework/PlaySession.h>

#include <cstdio>
#include <limits>
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

// What each level's GameMode saw. Keyed by level ID, read from the world's marker.
struct Counts {
    int constructed = 0, begun = 0, ended = 0, destroyed = 0, ticks = 0, frames = 0;
};
std::map<std::string, Counts> counts;
std::vector<StartReason> reasons;

// What a level's GameMode does, set per level by the test.
struct Script {
    std::string onBegin;   // request this level from beginPlay
    std::string onTick;   // ... from the first fixedUpdate
    std::string onFrame;   // ... from the first frame
    bool throwOnBegin = false;
    bool throwOnEnd = false;
};
std::map<std::string, Script> scripts;
// The last context a GameMode saw, so a "UI callback" can call game.requestLevel between frames.
const GameContext* lastContext = nullptr;
LevelFlow staleTravel;   // kept from an earlier level

std::string levelOf(const World& world) {
    std::string id;
    const_cast<World&>(world).each<const Name>([&](Entity, const Name& name) {
        if (name.value.rfind("level:", 0) == 0) id = name.value.substr(6);
    });
    return id;
}

class CountingMode : public GameMode {
public:
    explicit CountingMode(std::string level) : m_level(std::move(level)) { ++counts[m_level].constructed; }
    ~CountingMode() override { ++counts[m_level].destroyed; }
    void beginPlay(GameContext& game) override {
        ++counts[m_level].begun;
        reasons.push_back(game.startReason);
        lastContext = &game;
        const Script& script = scripts[m_level];
        if (script.throwOnBegin) throw std::runtime_error("beginPlay of " + m_level + " threw");
        if (!script.onBegin.empty()) CHECK(game.requestLevel(script.onBegin));
    }
    void fixedUpdate(GameContext& game, double) override {
        lastContext = &game;
        if (counts[m_level].ticks++ == 0 && !scripts[m_level].onTick.empty())
            CHECK(game.requestLevel(scripts[m_level].onTick));
    }
    void frame(GameContext& game, double) override {
        lastContext = &game;
        if (counts[m_level].frames++ == 0 && !scripts[m_level].onFrame.empty())
            CHECK(game.requestLevel(scripts[m_level].onFrame));
    }
    void endPlay(GameContext& game) override {
        ++counts[m_level].ended;
        if (lastContext == &game) lastContext = nullptr;
        if (scripts[m_level].throwOnEnd) throw std::runtime_error("endPlay of " + m_level + " threw");
    }

private:
    std::string m_level;
};

class CountingInstance : public GameInstance {
public:
    std::unique_ptr<GameMode> createGameMode(const World& scene) override {
        return std::make_unique<CountingMode>(levelOf(scene));
    }
};

// A level is a marker entity plus `extra` plain entities, so handles line up between levels.
void buildLevel(World& world, const std::string& id, int extra = 2) {
    world.add<Name>(world.create(), Name{"level:" + id});
    for (int i = 0; i < extra; ++i) world.add<Name>(world.create(), Name{id + "-" + std::to_string(i)});
}

std::vector<std::string> known = {"menu", "a", "b", "boom", "endboom"};

LevelHost::Hooks hooks(std::vector<std::string>* loads = nullptr) {
    LevelHost::Hooks h;
    h.load = [loads](const LevelRequest& request, World& into, std::string& error) {
        if (loads) loads->push_back(request.level);
        bool ok = false;
        for (const auto& id : known) ok |= id == request.level;
        if (request.level.rfind("loop", 0) == 0) ok = true;
        if (!ok) {
            error = "no level \"" + request.level + "\"";
            return false;
        }
        buildLevel(into, request.level);
        return true;
    };
    return h;
}

void reset() {
    counts.clear();
    reasons.clear();
    scripts.clear();
    lastContext = nullptr;
    staleTravel = {};
}

void tick(LevelHost& host, int n = 1) {
    for (int i = 0; i < n; ++i) {
        host.session()->update(1.0 / 60.0);
        host.session()->frame(1.0 / 60.0);
        host.process();
    }
}

void testTravel(JobSystem& jobs) {
    reset();
    CountingInstance instance;
    World world;
    buildLevel(world, "menu");
    std::vector<TravelResult> results;
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.onResult = [&](const TravelResult& r) { results.push_back(r); };
    host.start({"menu"});
    CHECK(host.playing() && host.current().level == "menu" && host.epoch() == 1 && world.levelEpoch() == 1);
    CHECK(host.session()->worldPolicy() == SessionWorldPolicy::OwnedRuntimeWorld);
    CHECK(host.session()->settingsPolicy() == SessionSettingsPolicy::PreserveUserSettings);

    // From fixedUpdate: nothing happens until process().
    scripts["menu"].onTick = "a";
    host.session()->update(1.0 / 60.0);
    CHECK(host.transitioning() && host.current().level == "menu" && levelOf(world) == "menu");
    host.process();
    CHECK(!host.transitioning() && host.current().level == "a" && levelOf(world) == "a" && host.epoch() == 2);
    CHECK(results.size() == 1 && results[0].status == TravelStatus::Succeeded && results[0].epoch == 2);
    CHECK(
        counts["menu"].constructed == 1 && counts["menu"].begun == 1 && counts["menu"].ended == 1 &&
        counts["menu"].destroyed == 1);

    // From frame.
    scripts["a"].onFrame = "b";
    tick(host);
    CHECK(host.current().level == "b" && counts["a"].ended == 1 && counts["a"].destroyed == 1);

    // From a UI callback between frames, while paused.
    CHECK(host.session()->pause());
    CHECK(lastContext && lastContext->requestLevel("menu"));
    host.process();
    CHECK(host.current().level == "menu" && host.session()->state() == PlaySession::State::Playing);
    CHECK(counts["menu"].constructed == 2 && counts["menu"].begun == 2 && counts["menu"].ended == 1);
    for (StartReason reason : reasons) CHECK(reason == StartReason::Fresh);

    host.stop();
    CHECK(!host.playing() && !host.session());
    for (const auto& [id, c] : counts) {
        CHECK(c.constructed == c.destroyed && c.begun == c.ended);
    }
    CHECK(counts["menu"].ended == 2 && counts["b"].destroyed == 1);
    host.stop();   // again: nothing
    CHECK(host.requestLevel({"a"}) == 0);   // nothing playing
}

void testFailures(JobSystem& jobs) {
    reset();
    CountingInstance instance;
    World world;
    buildLevel(world, "menu");
    LevelHost host(world, jobs, instance, nullptr, hooks());
    std::vector<TravelResult> results;
    host.onResult = [&](const TravelResult& r) { results.push_back(r); };
    host.start({"menu"});
    tick(host);

    // A level that doesn't load: the current one keeps playing, untouched.
    const uint64_t id = host.requestLevel({"nowhere"});
    CHECK(id != 0);
    host.process();
    CHECK(results.size() == 1 && results[0].id == id && results[0].status == TravelStatus::Failed);
    CHECK(results[0].error.find("nowhere") != std::string::npos);
    CHECK(host.playing() && host.current().level == "menu" && host.epoch() == 1 && counts["menu"].ended == 0);

    // endPlay throws: the level still ends and the next one starts.
    scripts["menu"].throwOnEnd = true;
    host.requestLevel({"a"});
    host.process();
    CHECK(host.current().level == "a" && results.back().status == TravelStatus::Succeeded);
    CHECK(counts["menu"].destroyed == 1);

    // beginPlay throws: nothing plays, the failure is reported once.
    scripts["boom"].throwOnBegin = true;
    tick(host);
    host.requestLevel({"boom"});
    host.process();
    CHECK(!host.playing() && results.back().status == TravelStatus::Failed);
    CHECK(host.failure().find("beginPlay of boom threw") != std::string::npos);
    CHECK(counts["boom"].constructed == 1 && counts["boom"].destroyed == 1 && counts["a"].destroyed == 1);

    // The application starts somewhere safe again.
    world = World{};
    buildLevel(world, "menu");
    host.start({"menu"});
    CHECK(host.playing() && host.failure().empty());

    // A second request while one waits is refused (Busy, S05): the first carries on. A
    // stopped request is cancelled, exactly once.
    tick(host);
    const size_t before = results.size();
    const uint64_t first = host.requestLevel({"a"});
    CHECK(first != 0 && host.requestLevel({"b"}) == 0 && host.request(to("b")).error == TravelError::Busy);
    CHECK(results.size() == before && host.activeTicket() == first);
    host.stop();
    CHECK(results.back().id == first && results.back().status == TravelStatus::Cancelled);
    CHECK(results.back().error.find("stopped") != std::string::npos);
    std::map<uint64_t, int> seen;
    for (const auto& r : results) ++seen[r.id];
    for (const auto& [rid, n] : seen) CHECK(n == 1);

    // A failed start of the very first level throws, and leaves nothing playing.
    World broken;
    buildLevel(broken, "boom");
    LevelHost other(broken, jobs, instance, nullptr, hooks());
    bool threw = false;
    try {
        other.start({"boom"});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw && !other.playing());
}

void testStaleAndRefs(JobSystem& jobs) {
    reset();
    CountingInstance instance;
    World world;
    buildLevel(world, "a");
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.start({"a"});
    tick(host);
    // An EntityRef and a travel callback kept from level A.
    EntityRef first(world, world.entityAt(0));
    CHECK(first && first.get<Name>().value == "level:a");
    staleTravel = lastContext->flow();
    CHECK(staleTravel.available());

    host.requestLevel({"b"});
    host.process();
    CHECK(host.current().level == "b");
    // Level B's first entity has the same handle, but the old ref doesn't reach it.
    CHECK(world.alive(first.entity()) && !first && first.find<Name>() == nullptr);
    EntityRef fresh(world, world.entityAt(0));
    CHECK(fresh && fresh.get<Name>().value == "level:b");
    // The old level's callback is refused.
    CHECK(!staleTravel.request("a") && !host.transitioning());

    // A save game loaded in place keeps refs: copies keep the epoch.
    World copy = world;
    CHECK(copy.levelEpoch() == world.levelEpoch());

    // Hot reload has to wait while a request is pending.
    host.requestLevel({"a"});
    CHECK(host.transitioning());
    host.process();
    CHECK(!host.transitioning());

    // Stop during preparation: the load hook quits.
    LevelHost::Hooks quitting = hooks();
    LevelHost* self = nullptr;
    auto load = quitting.load;
    quitting.load = [&](const LevelRequest& r, World& into, std::string& e) {
        self->stop();
        return load(r, into, e);
    };
    World w2;
    buildLevel(w2, "menu");
    LevelHost quitter(w2, jobs, instance, nullptr, quitting);
    self = &quitter;
    std::vector<TravelResult> results;
    quitter.onResult = [&](const TravelResult& r) { results.push_back(r); };
    quitter.start({"menu"});
    quitter.requestLevel({"a"});
    quitter.process();
    CHECK(!quitter.playing() && results.size() == 1 && results[0].status == TravelStatus::Cancelled);
    CHECK(levelOf(w2) == "menu");   // nothing was moved in

    // Epochs that would overflow fail explicitly.
    host.setEpochForTesting(std::numeric_limits<uint64_t>::max());
    tick(host);
    std::vector<TravelResult> last;
    host.onResult = [&](const TravelResult& r) { last.push_back(r); };
    host.requestLevel({"b"});
    host.process();
    CHECK(
        last.size() == 1 && last[0].status == TravelStatus::Failed && last[0].error.find("epoch") != std::string::npos);
    CHECK(host.playing() && host.current().level == "a");
}

void testRedirects(JobSystem& jobs) {
    reset();
    CountingInstance instance;
    // A short chain from beginPlay is fine: menu -> a -> b, each straight from beginPlay.
    {
        World world;
        buildLevel(world, "menu");
        scripts["menu"].onBegin = "a";
        scripts["a"].onBegin = "b";
        LevelHost host(world, jobs, instance, nullptr, hooks());
        host.start({"menu"});   // the request waits until menu has started
        CHECK(host.current().level == "menu" && host.transitioning());
        host.process();
        CHECK(host.current().level == "b" && !host.transitioning());
        host.stop();
    }
    // A cycle stops after kMaxRedirects.
    reset();
    for (int i = 0; i < 4; ++i) scripts["loop" + std::to_string(i)].onBegin = "loop" + std::to_string((i + 1) % 4);
    World world;
    buildLevel(world, "loop0");
    std::vector<TravelResult> results;
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.onResult = [&](const TravelResult& r) { results.push_back(r); };
    host.start({"loop0"});
    host.process();
    CHECK(results.size() == size_t(LevelHost::kMaxRedirects) + 1);
    CHECK(results.back().status == TravelStatus::Failed && results.back().error.find("cycle") != std::string::npos);
    CHECK(results.back().error.find("loop1 -> loop2") != std::string::npos);
    CHECK(host.playing() && !host.transitioning());   // the last level to start keeps playing
    // Once a level has ticked, travel counts afresh.
    scripts[host.current().level].onBegin.clear();
    for (auto& [id, script] : scripts) script.onBegin.clear();
    tick(host);
    host.requestLevel({"a"});
    host.process();
    CHECK(results.back().status == TravelStatus::Succeeded && host.current().level == "a");
}

// The editor's Play: the authored scene is copied once into a runtime world, levels come
// and go there, and Stop shows the authored scene again, exactly as it was.
void testEditorCycles(JobSystem& jobs) {
    reset();
    CountingInstance instance;
    World authored;
    buildLevel(authored, "a", 5);
    const Entity selected = authored.entityAt(3);
    authored.get<Name>(selected)->value = "selected";
    const uint64_t authoredLifetime = authored.lifetime();
    World runtime;
    LevelHost host(runtime, jobs, instance, nullptr, hooks());
    for (int cycle = 0; cycle < 100; ++cycle) {
        runtime = authored;   // Play: one copy
        host.start({"a"});
        tick(host, 2);
        lastContext->requestLevel("b");
        host.process();
        tick(host, 2);
        host.stop();
    }
    CHECK(authored.lifetime() == authoredLifetime && authored.entityCount() == 6 && authored.levelEpoch() == 0);
    CHECK(authored.alive(selected) && authored.get<Name>(selected)->value == "selected");
    CHECK(levelOf(authored) == "a");
    CHECK(counts["a"].constructed == 100 && counts["a"].destroyed == 100);
    CHECK(counts["b"].begun == 100 && counts["b"].ended == 100);

    // The original policy is still the default: a plain session restores the world.
    World plain;
    buildLevel(plain, "a");
    {
        PlaySession session(plain, jobs, instance);
        CHECK(session.worldPolicy() == SessionWorldPolicy::RestoreAuthoredSnapshot);
        session.start();
        plain.destroy(plain.entityAt(1));
        session.stop();
    }
    CHECK(plain.entityCount() == 3);
    // OwnedRuntimeWorld keeps what Play did.
    {
        PlaySession session(plain, jobs, instance);
        session.setWorldPolicy(SessionWorldPolicy::OwnedRuntimeWorld);
        session.start();
        plain.destroy(plain.entityAt(1));
        session.stop();
    }
    CHECK(plain.entityCount() == 2);
}

}   // namespace

int main() {
    JobSystem jobs(2);
    testTravel(jobs);
    testFailures(jobs);
    testStaleAndRefs(jobs);
    testRedirects(jobs);
    testEditorCycles(jobs);
    if (failures) {
        std::fprintf(stderr, "level lifetime: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("level lifetime: all checks passed\n");
    return 0;
}
