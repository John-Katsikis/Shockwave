// Asset residency (S04, docs/AssetResidency.md): leases on their own, then LevelHost
// travel holding them. A shared asset survives A -> B, a unique one retires, a duplicate
// request coalesces, a level that doesn't fit the budget is refused before the current
// level ends (BudgetExceeded, with totals), a start that doesn't fit fails explicitly,
// load reports carry every phase, and 100 A/B cycles plateau and drain to nothing at Stop.
// With a loader (part 2, on demand): only new assets load, only retired ones unload, travel
// unloads the old level's own assets before it loads the new one's and reads its scene, a
// load that fails or is cancelled leaves nothing loaded, and cycles plateau there too.
#include <Assets/AssetResidency.h>
#include <Engine/Components.h>
#include <GameFramework/GameInstance.h>
#include <GameFramework/LevelHost.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace shockwave;

namespace {

int failures = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #x);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint64_t MiB = 1024 * 1024;

AssetCost cost(const std::string& id, uint64_t gpuMiB, uint64_t cpu = 1000) {
    return {id, AssetKind::Texture, cpu, gpuMiB * MiB};
}

// Level contents: a shared font and mesh, plus each level's own.
std::map<std::string, std::vector<AssetCost>> levelAssets = {
    {"menu", {cost("Content/ui/font.ttf", 1), cost("Content/ui/menu.png", 2)}},
    {"a", {cost("Content/ui/font.ttf", 1), cost("Content/mesh/crate.glb", 4), cost("Content/a/floor.png", 8)}},
    {"b",
     {cost("Content/ui/font.ttf", 1), cost("Content/mesh/crate.glb", 4), cost("Content/b/sand.png", 16),
      cost("Content/b/sand.png", 16)}},   // listed twice: once
    {"huge", {cost("Content/huge/terrain.png", 4096)}},
};

void testLeases() {
    AssetResidency residency;
    auto menu = residency.acquire("menu", levelAssets["menu"]);
    auto ui = residency.acquire("application ui", {cost("Content/ui/font.ttf", 1)});
    CHECK(residency.size() == 2);
    CHECK(residency.find("Content/ui/font.ttf")->refs == 2);
    CHECK(residency.gpuBytes() == 3 * MiB);   // the font counts once
    // Travel menu -> a, planned before the menu goes.
    const auto plan = residency.plan(levelAssets["a"], &menu);
    CHECK(plan.shared == 1 && plan.added == 2 && plan.retired == 1);
    CHECK(plan.addedGpu == 12 * MiB && plan.retiredGpu == 2 * MiB);
    CHECK(plan.afterGpu == 13 * MiB && plan.peakGpu == 13 * MiB);
    menu.release();
    CHECK(residency.find("Content/ui/menu.png") == nullptr);   // unique: retired
    CHECK(residency.find("Content/ui/font.ttf")->refs == 1);   // the application's still
    auto a = residency.acquire("a", levelAssets["a"]);
    CHECK(residency.size() == 3);
    // A duplicate request coalesces.
    auto b = residency.acquire("b", levelAssets["b"]);
    CHECK(b.size() == 3);
    CHECK(residency.find("Content/b/sand.png")->refs == 1);
    CHECK(residency.find("Content/mesh/crate.glb")->refs == 2);
    a.release();
    a.release();   // safe to repeat
    CHECK(residency.find("Content/mesh/crate.glb")->refs == 1);
    CHECK(residency.find("Content/a/floor.png") == nullptr);
    // Moving a lease keeps one owner.
    AssetResidency::Lease moved = std::move(b);
    CHECK(!b.valid() && moved.valid() && moved.owner() == "b");
    moved = {};
    ui.release();
    CHECK(residency.size() == 0 && residency.gpuBytes() == 0 && residency.cpuBytes() == 0 && residency.leases() == 0);
    // The budget: explicit, with totals.
    residency.setBudget({0, 64 * MiB});
    std::string error;
    CHECK(residency.fits(residency.plan(levelAssets["a"], nullptr), error));
    CHECK(!residency.fits(residency.plan(levelAssets["huge"], nullptr), error));
    CHECK(error.rfind("BudgetExceeded: GPU 4.00 GiB over the budget of 64.0 MiB", 0) == 0);
}

// A level is a marker entity; the GameMode does nothing.
class Mode : public GameMode {};
class Instance : public GameInstance {
public:
    std::unique_ptr<GameMode> createGameMode(const World&) override { return std::make_unique<Mode>(); }
};

LevelHost::Hooks hooks(int* assetCalls = nullptr) {
    LevelHost::Hooks h;
    h.load = [](const LevelRequest& request, World& into, std::string& error) {
        if (!levelAssets.count(request.level)) {
            error = "no level " + request.level;
            return false;
        }
        into.add<Name>(into.create(), Name{"level:" + request.level});
        return true;
    };
    h.assets = [assetCalls](const LevelRequest& request, const World&, std::vector<AssetCost>& out, std::string&) {
        if (assetCalls) ++*assetCalls;
        out = levelAssets[request.level];
        return true;
    };
    return h;
}

void tick(PlaySession& session) {
    session.update(1.0 / 60.0);
    session.frame(1.0 / 60.0);
}

void testTravel(JobSystem& jobs) {
    AssetResidency residency;
    Instance instance;
    World world;
    world.add<Name>(world.create(), Name{"level:a"});
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.setResidency(&residency);
    host.start({"a"});
    CHECK(residency.size() == 3 && host.lease().valid());
    CHECK(host.loadReports().size() == 1 && host.loadReports()[0].added == 3);
    tick(*host.session());
    // A -> B: the font and crate are shared and never leave; a's floor retires.
    CHECK(host.request({"b"}) != 0);
    host.process();
    CHECK(host.playing() && host.current().level == "b");
    CHECK(residency.find("Content/a/floor.png") == nullptr);
    CHECK(residency.find("Content/mesh/crate.glb") && residency.find("Content/mesh/crate.glb")->refs == 1);
    CHECK(residency.find("Content/b/sand.png") != nullptr);
    CHECK(residency.retiredCount() == 1);
    const LoadReport& report = host.loadReports().back();
    CHECK(report.status == TravelStatus::Succeeded && report.from == "a" && report.request.level == "b");
    CHECK(report.shared == 2 && report.added == 1 && report.retired == 1);
    CHECK(report.addedGpuBytes == 16 * MiB && report.retiredGpuBytes == 8 * MiB);
    CHECK(report.residentGpuBytes == 21 * MiB && report.peakGpuBytes == 21 * MiB);
    CHECK(report.totalMs >= report.startMs && report.startMs >= 0 && report.readMs >= 0);
    // A level that won't fit is refused before b ends: b plays on, its lease untouched.
    residency.setBudget({0, 1024 * MiB});
    tick(*host.session());
    std::vector<TravelResult> results;
    host.onResult = [&](const TravelResult& r) { results.push_back(r); };
    CHECK(host.request({"huge"}) != 0);
    host.process();
    CHECK(results.size() == 1 && results[0].status == TravelStatus::Failed);
    CHECK(!results.empty() && results[0].error.rfind("BudgetExceeded: GPU", 0) == 0);
    CHECK(!results.empty() && results[0].error.find("4.00 GiB") != std::string::npos);
    CHECK(host.playing() && host.current().level == "b");
    CHECK(residency.find("Content/b/sand.png") != nullptr && residency.size() == 3);
    CHECK(host.loadReports().back().status == TravelStatus::Failed && host.loadReports().back().retireMs == 0);
    host.stop();
    CHECK(residency.size() == 0 && residency.leases() == 0);
    CHECK(!host.lease().valid());
}

void testStartOverBudget(JobSystem& jobs) {
    AssetResidency residency;
    residency.setBudget({0, 1 * MiB});   // the minimum: nothing fits
    Instance instance;
    World world;
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.setResidency(&residency);
    bool threw = false;
    try {
        host.start({"a"});
    } catch (const std::exception& e) {
        threw = std::string(e.what()).find("BudgetExceeded") != std::string::npos;
    }
    CHECK(threw);
    CHECK(!host.playing() && residency.size() == 0);
    CHECK(host.loadReports().size() == 1 && host.loadReports()[0].status == TravelStatus::Failed);
}

void testOff(JobSystem& jobs) {
    // Without a residency the hook isn't even asked.
    int calls = 0;
    Instance instance;
    World world;
    LevelHost host(world, jobs, instance, nullptr, hooks(&calls));
    host.start({"a"});
    tick(*host.session());
    host.request({"b"});
    host.process();
    CHECK(host.playing() && calls == 0 && !host.lease().valid());
    CHECK(host.loadReports().size() == 2 && host.loadReports()[1].assets == 0);
    host.stop();
}

void testCyclesPlateau(JobSystem& jobs) {
    AssetResidency residency;
    auto application = residency.acquire("application ui", {cost("Content/ui/font.ttf", 1)});
    Instance instance;
    World world;
    LevelHost host(world, jobs, instance, nullptr, hooks());
    host.setResidency(&residency);
    host.start({"a"});
    size_t sizeAtB = 0;
    uint64_t gpuAtB = 0, cpuAtB = 0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (const char* level : {"b", "a"}) {
            tick(*host.session());
            host.request({level});
            host.process();
            CHECK(host.playing() && host.current().level == level);
        }
        if (cycle == 0) {
            sizeAtB = residency.size();
            gpuAtB = residency.gpuBytes();
            cpuAtB = residency.cpuBytes();
        }
        CHECK(residency.size() == sizeAtB && residency.gpuBytes() == gpuAtB && residency.cpuBytes() == cpuAtB);
        CHECK(residency.leases() == 2);
    }
    CHECK(residency.find("Content/ui/font.ttf")->refs == 2);
    host.stop();
    // Drained: only the application's lease is left.
    CHECK(residency.size() == 1 && residency.leases() == 1);
    application.release();
    CHECK(residency.size() == 0 && residency.gpuBytes() == 0);
}

// ---- On demand: a fake loader that keeps a log --------------------------------------------

struct FakeLoader final : AssetResidency::Loader {
    std::vector<std::string>* log = nullptr;
    std::set<std::string> live;   // what's loaded now
    std::set<std::string> broken;   // these fail to load
    size_t frees = 0;
    bool load(
        const std::vector<std::string>& ids, const std::function<bool()>& cancelled, std::vector<std::string>& loaded,
        std::string& error) override {
        if (cancelled && cancelled()) {
            error = "cancelled";
            return false;
        }
        bool ok = true;
        for (const std::string& id : ids) {
            if (log) log->push_back("load " + id);
            if (broken.count(id)) {
                error = "AssetLoadFailed: " + id;
                ok = false;
                continue;
            }
            CHECK(live.insert(id).second);   // never loaded twice
            loaded.push_back(id);
        }
        return ok;
    }
    void unload(const std::vector<std::string>& ids) override {
        for (const std::string& id : ids) {
            if (log) log->push_back("unload " + id);
            CHECK(live.erase(id) == 1);   // only what's loaded
            ++frees;
        }
    }
    size_t pendingFrees() const override { return frees; }
};

void testLoader() {
    AssetResidency residency;
    FakeLoader loader;
    residency.setLoader(&loader);
    auto menu = residency.acquire("menu", levelAssets["menu"]);
    CHECK(!residency.find("Content/ui/font.ttf")->loaded);   // accounted, not in yet
    std::string error;
    CHECK(residency.load(menu, error) && loader.live.size() == 2 && residency.find("Content/ui/font.ttf")->loaded);
    CHECK(residency.load(menu, error) && residency.loadedCount() == 2);   // nothing new: no call
    // A -> menu's font stays; only a's own load, only menu's own unload.
    auto a = residency.acquire("a", levelAssets["a"]);
    menu.release();
    CHECK(loader.live == (std::set<std::string>{"Content/ui/font.ttf"}));
    CHECK(residency.load(a, error) && loader.live.size() == 3 && residency.unloadedCount() == 1);
    CHECK(residency.pendingFrees() == 1);
    // A load that fails: what did load stays until the lease goes, then nothing is left.
    loader.broken = {"Content/b/sand.png"};
    auto b = residency.acquire("b", levelAssets["b"]);
    CHECK(!residency.load(b, error) && error.find("AssetLoadFailed: Content/b/sand.png") != std::string::npos);
    CHECK(residency.find("Content/b/sand.png")->lastError.find("AssetLoadFailed") != std::string::npos);
    b.release();
    CHECK(!residency.find("Content/b/sand.png") && loader.live.size() == 3);
    // Cancelled before it starts: nothing loads.
    loader.broken.clear();
    auto again = residency.acquire("b", levelAssets["b"]);
    CHECK(!residency.load(again, [] { return true; }, error) && error == "cancelled");
    CHECK(!residency.find("Content/b/sand.png")->loaded);
    again.release();
    a.release();
    CHECK(loader.live.empty() && residency.size() == 0);
}

LevelHost::Hooks loggingHooks(std::vector<std::string>& log, std::set<std::string>* refuse = nullptr) {
    LevelHost::Hooks h = hooks();
    h.load = [&log](const LevelRequest& request, World& into, std::string& error) {
        log.push_back("read " + request.level);
        if (!levelAssets.count(request.level)) {
            error = "no level " + request.level;
            return false;
        }
        into.add<Name>(into.create(), Name{"level:" + request.level});
        return true;
    };
    h.assets = [&log,
                refuse](const LevelRequest& request, const World&, std::vector<AssetCost>& out, std::string& error) {
        log.push_back("plan " + request.level);
        if (refuse && refuse->count(request.level)) {
            error = "AssetMissing: Content/" + request.level + "/gone.png";
            return false;
        }
        out = levelAssets[request.level];
        return true;
    };
    h.stopping = [&log](PlaySession&) { log.push_back("end"); };
    return h;
}

void testTravelOnDemand(JobSystem& jobs) {
    std::vector<std::string> log;
    std::set<std::string> refuse;
    AssetResidency residency;
    FakeLoader loader;
    loader.log = &log;
    residency.setLoader(&loader);
    Instance instance;
    World world;
    world.add<Name>(world.create(), Name{"level:a"});
    LevelHost host(world, jobs, instance, nullptr, loggingHooks(log, &refuse));
    host.setResidency(&residency);
    host.start({"a"});
    CHECK(loader.live.size() == 3);   // start() loads what the lease holds
    tick(*host.session());
    log.clear();
    CHECK(host.request({"b"}) != 0);
    host.process();
    CHECK(host.playing() && host.current().level == "b");
    // Unload before upload, then the scene: the order section 6.1 of the plan gives.
    const std::vector<std::string> order = {
        "plan b", "end", "unload Content/a/floor.png", "load Content/b/sand.png", "read b"};
    CHECK(log == order);
    if (log != order)
        for (const auto& line : log) std::fprintf(stderr, "  %s\n", line.c_str());
    const LoadReport& report = host.loadReports().back();
    CHECK(report.loadedAssets == 1 && report.unloadedAssets == 1 && report.pendingFrees == loader.frees);
    CHECK(report.status == TravelStatus::Succeeded && report.retireMs >= 0 && report.uploadMs >= 0);
    // A broken link is refused before anything ends: b plays on, nothing loaded or unloaded.
    refuse = {"a"};
    tick(*host.session());
    log.clear();
    std::vector<TravelResult> results;
    host.onResult = [&](const TravelResult& r) { results.push_back(r); };
    host.request({"a"});
    host.process();
    CHECK(host.playing() && host.current().level == "b" && log == (std::vector<std::string>{"plan a"}));
    CHECK(
        results.size() == 1 && results[0].status == TravelStatus::Failed &&
        results[0].error.find("AssetMissing") != std::string::npos);
    // A load that fails after b ended: a failed start, nothing plays and nothing stays loaded.
    refuse.clear();
    loader.broken = {"Content/a/floor.png"};
    tick(*host.session());
    host.request({"a"});
    host.process();
    CHECK(!host.playing() && host.failure().find("didn't load") != std::string::npos);
    CHECK(host.failure().find("AssetLoadFailed: Content/a/floor.png") != std::string::npos);
    CHECK(loader.live.empty() && residency.size() == 0 && !host.lease().valid());
    CHECK(host.loadReports().back().status == TravelStatus::Failed);
    host.stop();
}

void testCyclesOnDemand(JobSystem& jobs) {
    std::vector<std::string> log;
    AssetResidency residency;
    FakeLoader loader;
    residency.setLoader(&loader);
    auto application = residency.acquire("application ui", {cost("Content/ui/font.ttf", 1)});
    std::string error;
    CHECK(residency.load(application, error));
    Instance instance;
    World world;
    world.add<Name>(world.create(), Name{"level:a"});
    LevelHost host(world, jobs, instance, nullptr, loggingHooks(log));
    host.setResidency(&residency);
    host.start({"a"});
    size_t liveAtA = loader.live.size();
    for (int cycle = 0; cycle < 100; ++cycle)
        for (const char* level : {"b", "a"}) {
            tick(*host.session());
            host.request({level});
            host.process();
            CHECK(host.playing() && host.current().level == level);
        }
    CHECK(loader.live.size() == liveAtA && residency.loadedCount() == 3 + 200);
    CHECK(residency.unloadedCount() == 200);   // each change: one out, one in
    host.stop();
    CHECK(loader.live == (std::set<std::string>{"Content/ui/font.ttf"}));   // the application's, only
    application.release();
    CHECK(loader.live.empty() && residency.size() == 0);
}

}   // namespace

int main() {
    registeredComponents();
    testLeases();
    JobSystem jobs(2);
    testTravel(jobs);
    testStartOverBudget(jobs);
    testOff(jobs);
    testCyclesPlateau(jobs);
    testLoader();
    testTravelOnDemand(jobs);
    testCyclesOnDemand(jobs);
    if (failures) {
        std::fprintf(stderr, "asset_residency: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("asset_residency: all checks passed\n");
    return 0;
}
