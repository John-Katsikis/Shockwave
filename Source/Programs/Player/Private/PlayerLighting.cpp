// The player's baked lighting tools (G05, G06, G07; docs/Lighting.md): --bake-lighting,
// --capture-reflections, --lighting-info, --lighting-assign-ids, --lighting-schema,
// --lighting-export and --lighting-import; and --check's rules for bake ids, a scene's bake
// and its reflection probes.

#include "Player.h"

#include <Assets/LightingData.h>
#include <Assets/ReflectionData.h>
#include <Engine/BakedLighting.h>
#include <Engine/Components.h>
#include <Engine/ReflectionProbes.h>
#include <Engine/SceneFile.h>
#include <Lighting/LightingBaker.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace player {

namespace {

std::string jsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escape[8];
            std::snprintf(escape, sizeof(escape), "\\u%04x", c);
            out += escape;
        } else {
            out += c;
        }
    }
    return out + "\"";
}
std::string jsonList(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + jsonQuote(items[i]);
    return out + "]";
}
std::string readText(const fs::path& file) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!shockwave::readLightingFile(file, bytes, error)) throw std::runtime_error(error);
    return std::string(bytes.begin(), bytes.end());
}
void writeBytes(const fs::path& file, std::span<const uint8_t> bytes) {
    std::string error;
    if (!shockwave::writeLightingFileAtomic(file, bytes, error)) throw std::runtime_error(error);
}
// What kind of payload a file is, by its extension.
enum class Payload { Lightmap, Receivers, Probes, Unknown };
Payload payloadOf(const fs::path& file) {
    const std::string ext = file.extension().string();
    return ext == ".ktx2"      ? Payload::Lightmap
           : ext == ".swbake"  ? Payload::Receivers
           : ext == ".swprobe" ? Payload::Probes
                               : Payload::Unknown;
}
// A scene loaded for a tool, keeping top-level lines it doesn't know (editor_camera).
struct ToolScene {
    shockwave::World world;
    std::vector<std::string> header;
    shockwave::SceneFileHooks hooks;
    ToolScene() {
        hooks.readHeader = [this](const std::string& key, std::istream& in) {
            std::string rest;
            std::getline(in, rest);
            header.push_back(key + rest);
        };
        hooks.writeHeader = [this](std::ostream& out) {
            for (const std::string& line : header) out << line << '\n';
        };
    }
};

}   // namespace

int lightingSchema(const fs::path& file) {
    const std::string schema = shockwave::lightingManifestSchema() + "\n" + shockwave::reflectionManifestSchema();
    if (file == "-") {
        std::fputs(schema.c_str(), stdout);
        return 0;
    }
    writeBytes(file, std::span(reinterpret_cast<const uint8_t*>(schema.data()), schema.size()));
    return 0;
}

int lightingExport(const fs::path& asset, const fs::path& output) {
    std::vector<uint8_t> bytes;
    std::string error, json;
    if (!shockwave::readLightingFile(asset, bytes, error)) throw std::runtime_error(error);
    switch (payloadOf(asset)) {
    case Payload::Lightmap: {
        // A reflection probe's cube is a .ktx2 too: six faces where a lightmap has one.
        if (shockwave::isReflectionKtx2(bytes)) {
            shockwave::ReflectionCube cube;
            if (!shockwave::decodeReflectionKtx2(bytes, cube, error))
                throw std::runtime_error(asset.string() + ": " + error);
            json = shockwave::reflectionCubeJson(cube);
            break;
        }
        uint32_t w = 0, h = 0;
        std::vector<float> rgba;
        if (!shockwave::decodeLightmapKtx2(bytes, w, h, rgba, error))
            throw std::runtime_error(asset.string() + ": " + error);
        json = shockwave::lightmapJson(w, h, rgba);
        break;
    }
    case Payload::Receivers: {
        std::vector<shockwave::LightingReceiver> receivers;
        if (!shockwave::readLightingReceivers(bytes, receivers, error))
            throw std::runtime_error(asset.string() + ": " + error);
        json = shockwave::receiversJson(receivers);
        break;
    }
    case Payload::Probes: {
        shockwave::LightingProbeSet probes;
        if (!shockwave::readLightingProbes(bytes, probes, error))
            throw std::runtime_error(asset.string() + ": " + error);
        json = shockwave::probesJson(probes);
        break;
    }
    case Payload::Unknown:
        throw UsageError("--lighting-export takes a lightmap .ktx2, a .swbake or a .swprobe (a .swlighting is "
                         "already text)");
    }
    writeBytes(output, std::span(reinterpret_cast<const uint8_t*>(json.data()), json.size()));
    std::printf("Wrote %s\n", output.string().c_str());
    return 0;
}

int lightingImport(const fs::path& json, const fs::path& output) {
    const std::string text = readText(json);
    std::vector<uint8_t> bytes;
    std::string error;
    bool ok = false;
    switch (payloadOf(output)) {
    case Payload::Lightmap: {
        // The JSON says which: a reflection cube's format is "reflection_cube".
        if (text.find("\"reflection_cube\"") != std::string::npos) {
            shockwave::ReflectionCube cube;
            ok = shockwave::reflectionCubeFromJson(text, cube, error) &&
                 shockwave::encodeReflectionKtx2(cube, bytes, error);
            break;
        }
        uint32_t w = 0, h = 0;
        std::vector<float> rgba;
        ok = shockwave::lightmapFromJson(text, w, h, rgba, error) &&
             shockwave::encodeLightmapKtx2(w, h, rgba, bytes, error);
        break;
    }
    case Payload::Receivers: {
        std::vector<shockwave::LightingReceiver> receivers;
        ok = shockwave::receiversFromJson(text, receivers, error);
        if (ok && !shockwave::writeLightingReceivers(receivers, bytes)) {
            ok = false;
            error = "the receivers don't fit the format's limits";
        }
        break;
    }
    case Payload::Probes: {
        shockwave::LightingProbeSet probes;
        ok = shockwave::probesFromJson(text, probes, error);
        if (ok && !shockwave::writeLightingProbes(probes, bytes)) {
            ok = false;
            error = "the probe volumes don't fit the format's limits";
        }
        break;
    }
    case Payload::Unknown:
        throw UsageError("--lighting-import writes a lightmap .ktx2, a .swbake or a .swprobe: name one with --output");
    }
    if (!ok) {
        std::fprintf(stderr, "Player: %s: %s\n", json.string().c_str(), error.c_str());
        return 1;
    }
    writeBytes(output, bytes);
    std::printf(
        "Wrote %s (%zu bytes). Rebake, or update the manifest's hash, before the scene uses it.\n",
        output.string().c_str(), bytes.size());
    return 0;
}

fs::path Player::lightingScene() const { return m_tool.scene.empty() ? m_project.startupScene : m_tool.scene; }

void Player::checkLighting(
    const shockwave::World& world, const std::string& scene, std::vector<std::string>& problems) {
    for (const std::string& problem : shockwave::checkLightingIds(world))
        problems.push_back(scene + ": " + problem + " (player --lighting-assign-ids fixes it)");
    // Content root: the bake's hashes include decoded textures and the environment map.
    const shockwave::LightingBakeInfo info =
        shockwave::inspectLightingBake(world, engine().assets(), engine().assets().contentRoot());
    using State = shockwave::BakedLightingRuntime::State;
    // Damaged files fail the check. Not baked yet or out of date is normal while editing:
    // the scene still draws (with the environment where the bake doesn't apply).
    if (info.broken)
        problems.push_back(scene + ": baked lighting " + info.asset + ": " + info.reason);
    else if (info.state == State::Missing || info.state == State::Invalid || info.state == State::Stale)
        std::printf("%s: warning: baked lighting %s: %s\n", scene.c_str(), info.asset.c_str(), info.reason.c_str());
    // Reflection probes (G07): the same, a damaged capture fails and an old one warns.
    const shockwave::ReflectionInfo reflections =
        shockwave::inspectReflectionProbes(world, engine().assets(), engine().assets().contentRoot());
    if (reflections.broken)
        problems.push_back(scene + ": reflection probes " + reflections.asset + ": " + reflections.reason);
    else if (
        reflections.state == shockwave::ReflectionInfo::State::Missing ||
        reflections.state == shockwave::ReflectionInfo::State::Invalid ||
        reflections.state == shockwave::ReflectionInfo::State::Stale)
        std::printf(
            "%s: warning: reflection probes%s%s: %s\n", scene.c_str(), reflections.asset.empty() ? "" : " ",
            reflections.asset.c_str(), reflections.reason.c_str());
    for (const std::string& w : reflections.warnings)
        std::printf("%s: warning: reflection probes: %s\n", scene.c_str(), w.c_str());
}

int Player::lightingInfo() {
    const fs::path scene = lightingScene();
    ToolScene loaded;
    std::string error;
    if (!shockwave::loadScene(loaded.world, engine().assets(), scene.string(), error, loaded.hooks)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    using State = shockwave::BakedLightingRuntime::State;
    const fs::path content = engine().assets().contentRoot();
    const shockwave::LightingBakeInfo info = shockwave::inspectLightingBake(loaded.world, engine().assets(), content);
    const shockwave::BakeSceneDescription description =
        shockwave::describeBakeScene(loaded.world, engine().assets(), content);
    const std::vector<std::string> idProblems = shockwave::checkLightingIds(loaded.world);
    uint32_t receivers = 0, occluders = 0;
    for (const auto& s : description.surfaces) (s.receiver ? receivers : occluders)++;
    const char* state = shockwave::BakedLightingRuntime::stateName(info.state);
    const std::string asset =
        info.asset.empty() ? shockwave::lightingManifestPath(loaded.world, m_project.relative(scene)) : info.asset;
    const shockwave::ReflectionInfo reflections =
        shockwave::inspectReflectionProbes(loaded.world, engine().assets(), content);
    const std::string reflectionAsset = reflections.asset.empty()
                                            ? shockwave::reflectionManifestPath(loaded.world, m_project.relative(scene))
                                            : reflections.asset;
    if (!m_tool.benchmarkJson.empty()) {
        std::string json = "{\n  \"schema\": \"shockwave.lighting-info.1\",\n";
        json += "  \"scene\": " + jsonQuote(m_project.relative(scene)) + ",\n";
        json += "  \"asset\": " + jsonQuote(asset) + ",\n";
        json += "  \"referenced\": " + std::string(info.asset.empty() ? "false" : "true") + ",\n";
        json += "  \"state\": " + jsonQuote(state) + ",\n";
        json += "  \"reason\": " + jsonQuote(info.reason) + ",\n";
        json += "  \"scene_receivers\": " + std::to_string(receivers) + ",\n";
        json += "  \"scene_occluders\": " + std::to_string(occluders) + ",\n";
        json += "  \"scene_volumes\": " + std::to_string(description.volumes.size()) + ",\n";
        json += "  \"scene_lights\": " + std::to_string(description.lights.size()) + ",\n";
        json += "  \"lighting_hash\": " + jsonQuote(shockwave::formatLightingHash(description.lightingHash)) + ",\n";
        json += "  \"geometry_hash\": " + jsonQuote(shockwave::formatLightingHash(description.geometryHash)) + ",\n";
        json += "  \"id_problems\": " + jsonList(idProblems) + ",\n";
        json += "  \"warnings\": " + jsonList(description.warnings) + ",\n";
        json += "  \"bake\": ";
        if (info.state == State::None || info.state == State::Missing || info.state == State::Invalid) {
            json += "null\n}\n";
        } else {
            const shockwave::LightingManifest& m = info.manifest;
            json += "{\"quality\": " + jsonQuote(m.quality) + ", \"samples\": " + std::to_string(m.samples) +
                    ", \"bounces\": " + std::to_string(m.bounces) + ", \"seed\": " + std::to_string(m.seed) +
                    ", \"atlas_size\": " + std::to_string(m.atlasSize) + ", \"tool\": " + jsonQuote(m.tool) +
                    ", \"payload_bytes\": " + std::to_string(info.payloadBytes) +
                    ", \"receivers\": " + std::to_string(m.receivers.size()) +
                    ", \"current_receivers\": " + std::to_string(info.currentReceivers) +
                    ", \"changed_receivers\": " + jsonList(info.changedReceivers) +
                    ", \"unbaked_receivers\": " + jsonList(info.unbakedReceivers) +
                    ", \"volumes\": " + std::to_string(m.volumes.size()) +
                    ", \"current_volumes\": " + std::to_string(info.currentVolumes) +
                    ", \"changed_volumes\": " + jsonList(info.changedVolumes) +
                    ", \"geometry_changed\": " + (info.geometryChanged ? "true" : "false") + "}\n}\n";
        }
        json.resize(json.size() - 3);   // "\n}\n": the reflection probes follow
        json += ",\n  \"reflections\": {\"asset\": " + jsonQuote(reflectionAsset) +
                ", \"referenced\": " + (reflections.asset.empty() ? "false" : "true") +
                ", \"state\": " + jsonQuote(shockwave::ReflectionProbesRuntime::stateName(reflections.state)) +
                ", \"reason\": " + jsonQuote(reflections.reason) +
                ", \"scene_probes\": " + std::to_string(reflections.probes) +
                ", \"budget_bytes\": " + std::to_string(reflections.budgetBytes) +
                ", \"captured\": " + std::to_string(reflections.manifest.probes.size()) +
                ", \"current_probes\": " + std::to_string(reflections.currentProbes) +
                ", \"changed_probes\": " + jsonList(reflections.changedProbes) +
                ", \"uncaptured_probes\": " + jsonList(reflections.uncapturedProbes) +
                ", \"scene_changed\": " + (reflections.sceneChanged ? "true" : "false") +
                ", \"payload_bytes\": " + std::to_string(reflections.payloadBytes) +
                ", \"warnings\": " + jsonList(reflections.warnings) + "}\n}\n";
        writeBytes(m_tool.benchmarkJson, std::span(reinterpret_cast<const uint8_t*>(json.data()), json.size()));
        std::printf("Wrote %s\n", m_tool.benchmarkJson.string().c_str());
        return 0;
    }
    std::printf("scene %s\n", m_project.relative(scene).c_str());
    std::printf("bake %s%s\n", asset.c_str(), info.asset.empty() ? " (no BakedLighting component yet)" : "");
    std::printf("state %s%s%s\n", state, info.reason.empty() ? "" : ": ", info.reason.c_str());
    std::printf(
        "participants: %u receivers, %u occluders, %zu probe volumes, %zu baked lights\n", receivers, occluders,
        description.volumes.size(), description.lights.size());
    if (info.state == State::Ready || info.state == State::Stale || info.state == State::Disabled) {
        const shockwave::LightingManifest& m = info.manifest;
        std::printf(
            "baked: %s, %u samples, %u bounces, seed %u, %u-texel atlases, by %s\n", m.quality.c_str(), m.samples,
            m.bounces, m.seed, m.atlasSize, m.tool.c_str());
        std::printf("payloads: %zu files, %.1f KB\n", m.resources.size(), double(info.payloadBytes) / 1024.0);
        std::printf("receivers: %u of %zu as baked\n", info.currentReceivers, m.receivers.size());
        for (const std::string& r : info.changedReceivers) std::printf("  changed: %s\n", r.c_str());
        for (const std::string& r : info.unbakedReceivers) std::printf("  not baked: %s\n", r.c_str());
        std::printf("volumes: %u of %zu as baked\n", info.currentVolumes, m.volumes.size());
        for (const std::string& v : info.changedVolumes) std::printf("  changed: %s\n", v.c_str());
        if (info.geometryChanged) std::printf("static geometry changed since the bake\n");
    }
    std::printf(
        "reflections %s%s: %s%s%s\n", reflectionAsset.c_str(),
        reflections.asset.empty() ? " (not named by BakedLighting yet)" : "",
        shockwave::ReflectionProbesRuntime::stateName(reflections.state), reflections.reason.empty() ? "" : ", ",
        reflections.reason.c_str());
    std::printf(
        "reflection probes: %u in the scene, %.1f MB of GPU memory at their resolutions\n", reflections.probes,
        double(reflections.budgetBytes) / (1024.0 * 1024.0));
    if (reflections.state == shockwave::ReflectionInfo::State::Ready ||
        reflections.state == shockwave::ReflectionInfo::State::Stale) {
        std::printf(
            "captured: %zu probes, %.1f KB, by %s; %u of %u as captured\n", reflections.manifest.probes.size(),
            double(reflections.payloadBytes) / 1024.0, reflections.manifest.tool.c_str(), reflections.currentProbes,
            reflections.probes);
        for (const std::string& p : reflections.changedProbes) std::printf("  changed: %s\n", p.c_str());
        for (const std::string& p : reflections.uncapturedProbes) std::printf("  not captured: %s\n", p.c_str());
        if (reflections.sceneChanged) std::printf("static objects, lights or the bake changed since the capture\n");
    }
    for (const std::string& p : idProblems) std::printf("id problem: %s\n", p.c_str());
    for (const std::string& w : description.warnings) std::printf("warning: %s\n", w.c_str());
    for (const std::string& w : reflections.warnings) std::printf("warning: %s\n", w.c_str());
    return 0;
}

void Player::startReflectionCapture() {
    m_reflectionCapture = std::make_unique<shockwave::ReflectionCapture>();
    std::string error;
    if (!m_reflectionCapture->start(
            m_world, engine().assets(), engine().renderer(), engine().assets().contentRoot(),
            m_project.relative(m_project.startupScene), error)) {
        std::fprintf(
            stderr, "Player: %s: can't capture reflections: %s\n", m_project.relative(m_project.startupScene).c_str(),
            error.c_str());
        m_reflectionCapture.reset();
        m_exitCode = 1;
        engine().quit();
    }
}

bool Player::driveReflectionCapture(shockwave::Renderer& renderer) {
    shockwave::ReflectionCapture& capture = *m_reflectionCapture;
    if (capture.render(m_world, renderer, engine().jobs(), engine().assets(), &m_bakedLighting)) {
        // A line each probe (and once while writing), not every face.
        std::string stage = capture.stage();
        if (const size_t face = stage.find(", face"); face != std::string::npos) stage.resize(face);
        if (stage != m_reflectionStage) {
            m_reflectionStage = stage;
            std::printf("%3d%% %s\n", int(capture.fraction() * 100.0f), stage.c_str());
            std::fflush(stdout);
        }
        return true;
    }
    const std::string scene = m_project.relative(m_project.startupScene);
    if (!capture.ok()) {
        std::fprintf(stderr, "Player: %s: reflection capture failed: %s\n", scene.c_str(), capture.error().c_str());
        m_exitCode = 1;
    } else {
        std::fputs(capture.report().c_str(), stdout);
        const shockwave::BakedLighting* component = nullptr;
        m_world.each<const shockwave::BakedLighting>(
            [&](shockwave::Entity, const shockwave::BakedLighting& b) { component = component ? component : &b; });
        if (!component || component->reflections != capture.manifest())
            std::printf(
                "note: the scene draws them once an entity has BakedLighting { reflections \"%s\" } (the editor's "
                "Lighting window adds it)\n",
                capture.manifest().c_str());
    }
    m_reflectionCapture.reset();
    engine().quit();
    return true;
}

int Player::lightingAssignIds() {
    const fs::path scene = lightingScene();
    const fs::path output = m_tool.output.empty() ? scene : m_tool.output;
    ToolScene loaded;
    std::string error;
    if (!shockwave::loadScene(loaded.world, engine().assets(), scene.string(), error, loaded.hooks)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    for (const std::string& problem : shockwave::checkLightingIds(loaded.world))
        std::printf("before: %s\n", problem.c_str());
    const size_t changed = shockwave::assignLightingIds(loaded.world);
    if (changed == 0 && output == scene) {
        std::printf("%s: every bake participant already has an id; unchanged\n", m_project.relative(scene).c_str());
        return 0;
    }
    if (!shockwave::saveScene(loaded.world, engine().assets(), output.string(), error, loaded.hooks)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    std::printf("%zu ids assigned; wrote %s\n", changed, output.string().c_str());
    return 0;
}

int Player::bakeLightingTool() {
    const fs::path scene = lightingScene();
    ToolScene loaded;
    std::string error;
    if (!shockwave::loadScene(loaded.world, engine().assets(), scene.string(), error, loaded.hooks)) {
        std::fprintf(stderr, "Player: %s\n", error.c_str());
        return 1;
    }
    shockwave::LightingBakeSettings settings;
    shockwave::parseLightingQuality(m_tool.bakeQuality, settings.quality);
    if (m_tool.keySeedGiven) {
        if (m_tool.keySeed > UINT32_MAX) throw UsageError("--bake-lighting --seed takes a number up to 4294967295");
        settings.seed = uint32_t(m_tool.keySeed);
    }
    shockwave::LightingBakeReport report;
    shockwave::LightingBakeProgress progress;
    bool ok = false;
    std::atomic<bool> finished{false};
    // On a thread of its own, so progress can be shown: a line each 10%.
    std::thread worker([&] {
        ok = shockwave::bakeLighting(
            loaded.world, engine().assets(), engine().assets().contentRoot(), m_project.relative(scene), settings,
            report, error, &progress);
        finished = true;
    });
    for (int shown = -1; !finished.load();) {
        const int tenth = int(progress.fraction.load() * 10.0f);
        if (tenth != shown) {
            shown = tenth;
            std::printf("%3d%% %s\n", tenth * 10, progress.stage().c_str());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    worker.join();
    if (!ok) {
        std::fprintf(stderr, "Player: %s: bake failed: %s\n", m_project.relative(scene).c_str(), error.c_str());
        return 1;
    }
    std::fputs(report.text().c_str(), stdout);
    const shockwave::BakedLighting* component = nullptr;
    loaded.world.each<const shockwave::BakedLighting>(
        [&](shockwave::Entity, const shockwave::BakedLighting& b) { component = component ? component : &b; });
    if (!component || component->asset != report.manifest)
        std::printf(
            "note: the scene draws it once an entity has BakedLighting { asset \"%s\" } (the editor's Bake "
            "window adds it)\n",
            report.manifest.c_str());
    return 0;
}

}   // namespace player
