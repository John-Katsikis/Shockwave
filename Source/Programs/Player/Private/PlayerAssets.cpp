// Asset dependencies and residency (S04, docs/AssetResidency.md): --asset-dependencies,
// the dependency part of --check, level leases in travel and --load-report.
#include "Player.h"

#include <Assets/AssetFolder.h>
#include <Engine/Engine.h>
#include <GameFramework/LevelAssets.h>
#include <Projects/Diagnostic.h>

#include <cstdio>
#include <fstream>

namespace player {

namespace {

bool writeText(const fs::path& out, const std::string& text) {
    if (out == "-") {
        std::fputs(text.c_str(), stdout);
        return true;
    }
    std::ofstream file(out, std::ios::binary);
    file << text;
    file.close();
    if (!file) {
        std::fprintf(stderr, "Player: can't write %s\n", out.string().c_str());
        return false;
    }
    std::printf("Wrote %s\n", out.string().c_str());
    return true;
}

std::string json(std::string_view text) { return shockwave::diagnosticJsonString(text); }

}   // namespace

shockwave::AssetGraph& Player::assetGraph() {
    if (!m_assetGraph) m_assetGraph = shockwave::makeProjectAssetGraph(m_project, m_assets.get(), &engine().assets());
    return *m_assetGraph;
}

int Player::assetDependencies() {
    shockwave::AssetGraph& graph = assetGraph();
    shockwave::LevelCatalog catalog;
    std::vector<shockwave::Diagnostic> ignored;   // --levels and --check report the catalog
    if (!shockwave::projectLevels(m_project, catalog, ignored)) catalog = shockwave::implicitLevelCatalog(m_project);
    std::vector<std::pair<std::string, std::string>> roots;
    for (const shockwave::LevelEntry& level : catalog.levels) {
        if (!m_tool.level.empty() && level.id != m_tool.level) continue;
        roots.emplace_back(level.id, graph.idFor(m_project.root / level.scene));
    }
    if (!m_tool.level.empty() && roots.empty()) {
        std::fprintf(stderr, "Player: --level %s: no such level\n", m_tool.level.c_str());
        return 1;
    }
    // Code-made levels have the game circuit too.
    const std::string gameCircuit = m_project.gameCircuit.empty() ? std::string() : graph.idFor(m_project.gameCircuit);
    if (!gameCircuit.empty() && m_tool.level.empty()) roots.emplace_back("game circuit", gameCircuit);
    const fs::path& out = m_tool.benchmarkJson;
    if (!out.empty()) {
        const std::string text = shockwave::assetDependenciesJson(graph, roots, m_assets.get(), &engine().assets());
        if (!writeText(out, text)) return 1;
        return text.find("\"ok\": true") != std::string::npos ? 0 : 1;
    }
    std::vector<shockwave::Diagnostic> diagnostics;
    for (const auto& [name, id] : roots) {
        const auto costs = shockwave::assetCosts(graph, id, m_assets.get(), &engine().assets());
        uint64_t cpu = 0, gpu = 0;
        for (const auto& c : costs) {
            cpu += c.cpuBytes;
            gpu += c.gpuBytes;
        }
        const shockwave::AssetNode* node = graph.find(id);
        std::printf(
            "%s %s: %zu asset%s, CPU %s, GPU %s, closure %016llx\n", name == "game circuit" ? "root" : "level",
            name.c_str(), costs.size(), costs.size() == 1 ? "" : "s", shockwave::formatAssetBytes(cpu).c_str(),
            shockwave::formatAssetBytes(gpu).c_str(), static_cast<unsigned long long>(node ? node->closureHash : 0));
        for (const auto& c : costs)
            std::printf(
                "  %s %s (%s%s%s)\n", shockwave::assetKindName(c.kind), c.id.c_str(),
                shockwave::formatAssetBytes(c.cpuBytes).c_str(), c.gpuBytes ? ", GPU " : "",
                c.gpuBytes ? shockwave::formatAssetBytes(c.gpuBytes).c_str() : "");
        for (auto& d : graph.diagnostics(id))
            if (std::none_of(diagnostics.begin(), diagnostics.end(), [&](const shockwave::Diagnostic& o) {
                    return o.code == d.code && o.file == d.file && o.line == d.line && o.message == d.message;
                }))
                diagnostics.push_back(std::move(d));
    }
    size_t errors = 0;
    for (const auto& d : diagnostics) {
        errors += d.error();
        std::printf("%s %s: %s\n", shockwave::diagnosticSeverityName(d.severity), d.code.c_str(), d.text().c_str());
    }
    std::printf("%s\n", errors ? (std::to_string(errors) + " problem" + (errors == 1 ? "" : "s")).c_str() : "ok");
    return errors ? 1 : 0;
}

void Player::checkAssetGraph(const std::vector<fs::path>& scenes, std::vector<std::string>& problems) {
    shockwave::AssetGraph& graph = assetGraph();
    std::vector<std::string> roots;
    for (const fs::path& scene : scenes)
        if (const std::string id = graph.idFor(fs::absolute(scene)); !id.empty()) roots.push_back(id);
    if (!m_project.gameCircuit.empty()) roots.push_back(graph.idFor(m_project.gameCircuit));
    std::vector<shockwave::Diagnostic> found;
    size_t assets = 0;
    for (const std::string& root : roots) {
        graph.add(root);
        for (auto& d : graph.diagnostics(root))
            if (std::none_of(found.begin(), found.end(), [&](const shockwave::Diagnostic& o) {
                    return o.code == d.code && o.file == d.file && o.line == d.line && o.message == d.message;
                }))
                found.push_back(std::move(d));
    }
    assets = graph.nodes().size();
    const size_t before = problems.size();
    for (const shockwave::Diagnostic& d : found) {
        if (d.error())
            problems.push_back(d.text() + " (" + d.code + ")");
        else
            std::printf("warning %s (%s)\n", d.text().c_str(), d.code.c_str());
    }
    if (problems.size() == before) std::printf("ok asset dependencies (%zu assets)\n", assets);
}

shockwave::LevelHost::Hooks Player::withAssetLeases(shockwave::LevelHost::Hooks hooks) {
    if (!m_tool.assetResidency) return hooks;
    hooks.assets = [this](
                       const shockwave::LevelRequest& request, const shockwave::World&,
                       std::vector<shockwave::AssetCost>& assets, std::string& error) {
        fs::path scene = sceneForLevel(request.level);
        if (scene.empty()) scene = m_project.startupScene;
        shockwave::AssetGraph& graph = assetGraph();
        const std::string id = graph.idFor(fs::absolute(scene));
        if (id.empty()) return true;   // a scene from outside the project: nothing to lease
        assets = shockwave::assetCosts(graph, id, m_assets.get(), &engine().assets());
        // On demand the scene is read after the current level ends, so a broken link is
        // refused now, while that level can still play on.
        if (m_assetLoader)
            for (const shockwave::Diagnostic& d : graph.diagnostics(id))
                if (d.error()) {
                    error = d.text() + " (" + d.code + ")";
                    return false;
                }
        return true;
    };
    return hooks;
}

bool Player::loadAssetsOnDemand() const {
    const std::string& mode = m_tool.assetLoading.empty() ? m_project.assetLoading : m_tool.assetLoading;
    return mode == "levels" && m_tool.assetResidency && m_tool.mode == Tool::Mode::Play && !m_tool.optimise &&
           !m_tool.captureReflections;
}

shockwave::AssetResidency::Lease Player::sceneAssets(const std::string& owner, const fs::path& file) {
    if (!m_assetLoader) return {};
    shockwave::AssetGraph& graph = assetGraph();
    const std::string id = graph.idFor(fs::absolute(file));
    if (id.empty()) return {};
    auto costs = shockwave::assetCosts(graph, id, m_assets.get(), &engine().assets());
    for (const shockwave::Diagnostic& d : graph.diagnostics(id))
        if (d.error()) throw std::runtime_error(d.text() + " (" + d.code + ")");
    auto lease = m_residency.acquire(owner, costs);
    std::string error;
    if (!m_residency.load(lease, error)) throw std::runtime_error(m_project.relative(file) + ": " + error);
    return lease;
}

void Player::recordLoad(const shockwave::LoadReport& report) {
    m_loadReports.push_back(report);
    SW_LOG(
        shockwave::LogGame, Info, "Level \"{}\" {}: {:.1f} ms, {} assets ({} shared, {} new, {} retired), GPU {}",
        report.request.level, shockwave::travelStatusName(report.status), report.totalMs, report.assets, report.shared,
        report.added, report.retired, shockwave::formatAssetBytes(report.residentGpuBytes));
    if (m_assetLoader)
        SW_LOG(
            shockwave::LogGame, Info, "Level \"{}\": loaded {}, unloaded {}, {} GPU frees pending",
            report.request.level, report.loadedAssets, report.unloadedAssets, report.pendingFrees);
}

void Player::writeLoadReport() {
    if (m_tool.loadReport.empty()) return;
    std::string text = "{\n  \"format\": 1,\n  \"mode\": \"load_report\",\n";
    text += "  \"project\": " + json(m_project.name) + ",\n";
    text += std::string("  \"residency\": ") + (m_tool.assetResidency ? "true" : "false") + ",\n";
    text += std::string("  \"asset_loading\": ") + (m_assetLoader ? "\"levels\"" : "\"all\"") + ",\n";
    text += "  \"budget\": {\"cpu_bytes\": " + std::to_string(m_tool.assetBudget.cpuBytes) +
            ", \"gpu_bytes\": " + std::to_string(m_tool.assetBudget.gpuBytes) + "},\n";
    text += "  \"loads\": [";
    for (size_t i = 0; i < m_loadReports.size(); ++i) {
        const shockwave::LoadReport& r = m_loadReports[i];
        char phases[512];
        std::snprintf(
            phases, sizeof(phases),
            "{\"validate\": %.3f, \"read\": %.3f, \"prepare\": %.3f, \"retire\": %.3f, \"upload\": %.3f, "
            "\"start\": %.3f}",
            r.validateMs, r.readMs, r.prepareMs, r.retireMs, r.uploadMs, r.startMs);
        char total[32];
        std::snprintf(total, sizeof(total), "%.3f", r.totalMs);
        text +=
            std::string(i ? ",\n    " : "\n    ") + "{\"level\": " + json(r.request.level) +
            ", \"spawn\": " + json(r.request.spawn) + ", \"from\": " + json(r.from) +
            ", \"epoch\": " + std::to_string(r.epoch) + ", \"status\": " + json(shockwave::travelStatusName(r.status)) +
            ", \"error\": " + json(r.error) + ", \"phases_ms\": " + phases + ", \"total_ms\": " + total +
            ", \"assets\": " + std::to_string(r.assets) + ", \"shared\": " + std::to_string(r.shared) +
            ", \"added\": " + std::to_string(r.added) + ", \"retired\": " + std::to_string(r.retired) +
            ", \"added_cpu_bytes\": " + std::to_string(r.addedCpuBytes) +
            ", \"added_gpu_bytes\": " + std::to_string(r.addedGpuBytes) +
            ", \"retired_cpu_bytes\": " + std::to_string(r.retiredCpuBytes) +
            ", \"retired_gpu_bytes\": " + std::to_string(r.retiredGpuBytes) +
            ", \"resident_cpu_bytes\": " + std::to_string(r.residentCpuBytes) +
            ", \"resident_gpu_bytes\": " + std::to_string(r.residentGpuBytes) +
            ", \"peak_cpu_bytes\": " + std::to_string(r.peakCpuBytes) +
            ", \"peak_gpu_bytes\": " + std::to_string(r.peakGpuBytes) +
            ", \"loaded\": " + std::to_string(r.loadedAssets) + ", \"unloaded\": " + std::to_string(r.unloadedAssets) +
            ", \"pending_frees\": " + std::to_string(r.pendingFrees) + "}";
    }
    text += m_loadReports.empty() ? "],\n" : "\n  ],\n";
    text += "  \"resident\": {\"assets\": " + std::to_string(m_residency.size()) +
            ", \"cpu_bytes\": " + std::to_string(m_residency.cpuBytes()) +
            ", \"gpu_bytes\": " + std::to_string(m_residency.gpuBytes()) +
            ", \"leases\": " + std::to_string(m_residency.leases()) +
            ", \"retired_total\": " + std::to_string(m_residency.retiredCount()) +
            ", \"loaded_total\": " + std::to_string(m_residency.loadedCount()) +
            ", \"unloaded_total\": " + std::to_string(m_residency.unloadedCount()) +
            ", \"pending_frees\": " + std::to_string(engine().renderer().pendingFrees()) + "}\n}\n";
    if (!writeText(m_tool.loadReport, text)) m_exitCode = 1;
}

}   // namespace player
