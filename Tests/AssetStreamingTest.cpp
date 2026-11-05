// Loading on demand (S04 part 2, docs/AssetResidency.md), on a real AssetFolder and the
// renderer named on the command line ("null" by default, whose fake GPU frees what's
// destroyed two frames later; "vulkan" in the gpu-labelled test):
//   - open() lists the files and imports nothing; load() imports what it's asked for, in
//     batches under a staging limit, publishing on the main thread only;
//   - a load cancelled after its decode publishes nothing, and what it published goes again;
//   - a failed upload leaves nothing registered, and its partial GPU resources are freed;
//   - unload() unregisters, a material reading an unloaded texture falls back, and the GPU
//     frees wait for frames in flight, then drain;
//   - saving a loaded file re-imports it; saving one that isn't loaded doesn't;
//   - AssetFolderLoader under AssetResidency loads a lease and unloads it when it retires;
//   - 100 load/unload cycles plateau, and drain to nothing pending.

#include <Assets/AssetFolder.h>
#include <Assets/AssetResidency.h>
#include <Engine/Engine.h>
#include <GameFramework/LevelAssets.h>
#include <Projects/Project.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

// A binary PPM: the AssetFolder reads it like any image.
void writeImage(const fs::path& file, uint32_t size, uint8_t shade) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out << "P6\n" << size << " " << size << "\n255\n";
    for (uint32_t i = 0; i < size * size; ++i) out << char(shade) << char(255 - shade) << char(shade / 2);
}

void writeText(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// Its modification time pushed forward, so a refresh sees it changed whatever the clock.
void touch(const fs::path& file, int seconds) {
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(seconds));
}

const char* kCube = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
                    "f 1 2 3\nf 1 3 4\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 4 3 7\nf 4 7 8\n";

std::string tex(int i) { return "textures/t" + std::to_string(i) + ".ppm"; }

class App final : public Application {
public:
    fs::path root, content;
    std::unique_ptr<AssetFolder> folder;
    std::thread::id mainThread;
    std::vector<std::string> uploads;
    std::set<std::string> failUploads;
    int phase = 0;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    size_t textureNames = 0, modelNames = 0;

    void onStart() override {
        mainThread = std::this_thread::get_id();
        root = fs::temp_directory_path() / ("shockwave_asset_streaming_" + engine().renderer().backendName());
        content = root / "Content";
        fs::remove_all(root);
        for (int i = 0; i < 10; ++i) writeImage(content / tex(i), 16, uint8_t(20 * i));
        writeText(content / "models/box.obj", kCube);
        writeText(content / "materials/m.swmaterial", "swmaterial 1\ntexture base_color \"" + tex(0) + "\"\n");
        writeText(content / "ui/menu.rml", "<rml><body/></rml>\n");

        Assets& assets = engine().assets();
        folder = std::make_unique<AssetFolder>(engine().renderer(), assets, engine().jobs());
        folder->setOnDemand(true);
        folder->setBackgroundCompression(false);
        folder->setFaultHook([this](std::string_view stage, std::string_view name) {
            check(std::this_thread::get_id() == mainThread, "uploads happen on the main thread only");
            if (stage == "upload") uploads.emplace_back(name);
            return failUploads.count(std::string(name)) > 0;
        });
        folder->open(content);
        check(folder->files().size() == 13, "open() lists every file (" + std::to_string(folder->files().size()) + ")");
        bool any = false;
        for (const auto& f : folder->files()) any = any || f.imported;
        check(!any, "open() imports nothing on demand");
        check(!assets.findTexture(tex(0)) && !assets.findModel("models/box.obj").valid(), "nothing is registered");
        check(!assets.findMaterial("materials/m.swmaterial").valid(), "no material either");

        // In batches: each file its own under a tiny staging limit.
        AssetFolder::LoadOptions tiny;
        tiny.stagingBytes = 1;
        auto r = folder->load({"materials/m.swmaterial", tex(0), tex(1), "ui/menu.rml"}, tiny);
        check(r.ok() && r.loaded == 3 && r.resident == 1, "loads what's asked; a .rml has nothing to import");
        check(r.batches == 3, "a batch each under the staging limit (" + std::to_string(r.batches) + ")");
        check(r.peakStagedBytes >= 16 * 16 * 4, "the staged bytes are measured");
        check(assets.findTexture(tex(0)).has_value() && folder->loaded(tex(1)), "the textures are registered");
        const MaterialHandle m = assets.findMaterial("materials/m.swmaterial");
        check(m.valid() && !assets.material(m)->fallback, "the material resolves against its texture");
        // In one batch by default; twice in a request is once; an unknown name fails.
        r = folder->load({tex(2), tex(2), tex(9)});
        check(r.ok() && r.loaded == 2 && r.batches == 1, "one batch, duplicates once");
        r = folder->load({tex(0), "textures/nope.png"});
        check(r.resident == 1 && r.failed.size() == 1 && r.failed[0].name == "textures/nope.png", "unknown names fail");

        // Cancelled after the second batch's decode: it never publishes, the first goes again.
        const size_t pendingBefore = engine().renderer().pendingFrees();
        int asked = 0;
        AssetFolder::LoadOptions cancel = tiny;
        cancel.cancelled = [&] { return ++asked >= 4; };   // before 1, after 1, before 2, after 2
        uploads.clear();
        r = folder->load({tex(3), tex(4), tex(5)}, cancel);
        check(r.cancelled && r.loaded == 0 && !r.ok(), "the load reports it was cancelled");
        check(uploads == std::vector<std::string>{tex(3)}, "the cancelled decode was never uploaded");
        check(
            !assets.findTexture(tex(3)) && !assets.findTexture(tex(4)) && !folder->loaded(tex(3)),
            "nothing of a cancelled load stays");
        check(engine().renderer().pendingFrees() > pendingBefore, "what it published is freed");

        // A failed upload, part way through a model and before a texture: nothing stays.
        failUploads = {"models/box.obj", tex(6)};
        const size_t pendingModel = engine().renderer().pendingFrees();
        r = folder->load({"models/box.obj", tex(6)});
        check(r.failed.size() == 2 && r.loaded == 0, "both uploads fail");
        check(!assets.findModel("models/box.obj").valid() && !assets.findTexture(tex(6)), "neither is registered");
        check(engine().renderer().pendingFrees() > pendingModel, "the model's meshes on the GPU are freed");
        check(!folder->find("models/box.obj")->error.empty(), "the file says why");
        failUploads.clear();
        r = folder->load({"models/box.obj"});
        check(r.ok() && assets.findModel("models/box.obj").valid(), "and loads once the fault's gone");

        // Unloading: the name stops resolving, the material that read it falls back.
        check(folder->unload({tex(0), tex(0), "ui/menu.rml"}) == 1, "unloads what's loaded, once");
        check(!assets.findTexture(tex(0)) && !folder->loaded(tex(0)), "an unloaded texture is unregistered");
        check(
            assets.material(assets.findMaterial("materials/m.swmaterial"))->fallback,
            "a material reading an unloaded texture falls back instead of reading freed memory");
        check(engine().renderer().pendingFrees() > 0, "its GPU memory waits for the frames in flight");
        since = std::chrono::steady_clock::now();
    }

    bool drained() { return engine().renderer().pendingFrees() == 0; }

    void onFrame(double) override {
        if (std::chrono::steady_clock::now() - since > std::chrono::seconds(20)) {
            check(
                false, "phase " + std::to_string(phase) + " never finished (" +
                           std::to_string(engine().renderer().pendingFrees()) + " frees pending)");
            engine().quit();
            return;
        }
        Assets& assets = engine().assets();
        switch (phase) {
        case 0: {   // the deferred frees complete once frames have passed
            if (!drained()) return;
            // Hot reload: a loaded file that's saved re-imports, one that isn't loaded doesn't.
            writeImage(content / tex(1), 32, 200);
            touch(content / tex(1), 5);
            writeImage(content / tex(7), 32, 100);
            touch(content / tex(7), 5);
            uploads.clear();
            const auto changes = folder->refresh();
            bool reloaded = false;
            for (const auto& c : changes)
                reloaded = reloaded || (c.name == tex(1) && c.what == AssetFolder::Change::What::Reloaded);
            check(reloaded && folder->find(tex(1))->width == 32, "a loaded file re-imports when it's saved");
            check(
                uploads == std::vector<std::string>{tex(1)} && !folder->loaded(tex(7)),
                "one that isn't loaded stays out");
            const auto r = folder->load({tex(7)});
            check(r.ok() && folder->find(tex(7))->width == 32, "and comes in as saved when it's asked for");

            // Under AssetResidency: a lease loads, its retirement unloads.
            Project project;
            project.root = root;
            project.assetFolder = content;
            AssetFolderLoader loader(project, *folder, &engine().renderer());
            AssetResidency residency;
            residency.setLoader(&loader);
            {
                auto lease = residency.acquire(
                    "level",
                    {{"Content/" + tex(8), AssetKind::Texture, 100, 0}, {"Scenes/a.swscene", AssetKind::Scene, 10, 0}});
                std::string error;
                check(residency.load(lease, error) && folder->loaded(tex(8)), "a lease loads its Content");
                check(residency.find("Scenes/a.swscene")->loaded, "a scene has nothing to import");
                auto broken = residency.acquire("broken", {{"Content/textures/gone.png", AssetKind::Texture, 1, 0}});
                check(
                    !residency.load(broken, error) && error.find("AssetLoadFailed: Content/textures/gone.png") == 0,
                    "a missing file fails the lease's load: " + error);
            }
            check(!folder->loaded(tex(8)) && residency.size() == 0, "its retirement unloads it");
            break;
        }
        case 1: {   // 100 load/unload cycles plateau
            if (!drained()) return;
            for (int cycle = 0; cycle < 100; ++cycle) {
                const auto r = folder->load({tex(8), tex(9), "models/box.obj"});
                check(r.ok(), "cycle load");
                if (cycle == 0) {
                    textureNames = assets.textures().size();
                    modelNames = assets.models().size();
                }
                check(
                    assets.textures().size() == textureNames && assets.models().size() == modelNames,
                    "the registry doesn't grow");
                folder->unload({tex(8), tex(9), "models/box.obj"});
            }
            check(!assets.findTexture(tex(9)) && !assets.findModel("models/box.obj").valid(), "all unloaded");
            break;
        }
        case 2:   // and drain
            if (!drained()) return;
            folder.reset();
            fs::remove_all(root);
            engine().quit();
            break;
        }
        ++phase;
        since = std::chrono::steady_clock::now();
    }
};

}   // namespace

int main(int argc, char** argv) {
    EngineConfig config;
    config.window = {"asset streaming test", 320, 240, false, true};
    config.renderer = argc > 1 ? argv[1] : "null";
    config.maxFrames = 0;   // until the last phase quits
    config.showFpsInTitle = false;
    Engine engine(config);
    App app;
    engine.run(app);
    check(app.phase == 3, "every phase ran (stopped at " + std::to_string(app.phase) + ")");
    if (failures) {
        std::fprintf(stderr, "asset_streaming: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("asset_streaming: all checks passed\n");
    return 0;
}
