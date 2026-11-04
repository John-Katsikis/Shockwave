// The Resource residency window (S04, docs/AssetResidency.md): what Play's levels hold
// resident, who holds each asset, the last level loads phase by phase, the budget, and
// the dependency check of the open scene.
#include "Editor.h"

#include <Assets/AssetResidency.h>
#include <GameFramework/LevelAssets.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace editor {

namespace {

bool matches(const std::string& text, const char* filter) {
    if (!filter[0]) return true;
    std::string a = text, b = filter;
    for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

}   // namespace

shockwave::AssetGraph& Editor::assetGraph() {
    // One graph per project; it keeps what it read and re-reads files that changed.
    if (!m_assetGraph || m_assetGraphRoot != m_project.root) {
        m_assetGraph = shockwave::makeProjectAssetGraph(m_project, m_assetFolder.get(), &engine().assets());
        m_assetGraphRoot = m_project.root;
    }
    return *m_assetGraph;
}

void Editor::drawResidencyWindow() {
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Resource residency", &m_showResidency)) {
        ImGui::End();
        return;
    }
    using shockwave::formatAssetBytes;
    const bool live = m_runtimeLevels && m_runtimeLevels->residency() == &m_residency;
    ImGui::Checkbox("Track level leases", &m_residencyEnabled);
    ImGui::SetItemTooltip("Each level holds a lease on what its scene needs, so travel can tell shared assets "
                          "from ones that retire, and refuse a level over the budget. Applies at the next Play.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputFloat("CPU MiB", &m_budgetCpuMiB, 0, 0, "%.0f");
    ImGui::SetItemTooltip("The most a level change may leave resident on the CPU; 0 for no limit. At the next Play.");
    m_budgetCpuMiB = std::max(0.0f, m_budgetCpuMiB);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::InputFloat("GPU MiB", &m_budgetGpuMiB, 0, 0, "%.0f");
    ImGui::SetItemTooltip("The most a level change may leave resident on the GPU; 0 for no limit. At the next Play.");
    m_budgetGpuMiB = std::max(0.0f, m_budgetGpuMiB);

    ImGui::Text(
        "%s: %zu assets in %zu lease%s, CPU %s, GPU %s; %llu retired so far", live ? "Playing" : "Not playing",
        m_residency.size(), m_residency.leases(), m_residency.leases() == 1 ? "" : "s",
        formatAssetBytes(m_residency.cpuBytes()).c_str(), formatAssetBytes(m_residency.gpuBytes()).c_str(),
        static_cast<unsigned long long>(m_residency.retiredCount()));

    // The last level loads, newest first.
    if (ImGui::CollapsingHeader("Level loads", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_loadReports.empty()) ImGui::TextDisabled("No level has loaded since the editor started. Press Play.");
        if (!m_loadReports.empty() &&
            ImGui::BeginTable(
                "loads", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                ImVec2(0, 140))) {
            for (const char* column :
                 {"Level", "Result", "Validate", "Read", "Prepare", "Retire", "Upload", "Start", "Shared", "New",
                  "Retired"})
                ImGui::TableSetupColumn(column);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (auto it = m_loadReports.rbegin(); it != m_loadReports.rend(); ++it) {
                const shockwave::LoadReport& r = *it;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text(
                    "%s%s%s", r.from.empty() ? "" : r.from.c_str(), r.from.empty() ? "" : " -> ",
                    r.request.level.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(shockwave::travelStatusName(r.status));
                if (!r.error.empty()) ImGui::SetItemTooltip("%s", r.error.c_str());
                for (double ms : {r.validateMs, r.readMs, r.prepareMs, r.retireMs, r.uploadMs, r.startMs}) {
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f ms", ms);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", r.shared);
                ImGui::TableNextColumn();
                ImGui::Text("%zu (%s)", r.added, formatAssetBytes(r.addedGpuBytes).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%zu (%s)", r.retired, formatAssetBytes(r.retiredGpuBytes).c_str());
            }
            ImGui::EndTable();
        }
    }

    // Who holds what.
    if (ImGui::CollapsingHeader("Resident assets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(240);
        ImGui::InputTextWithHint("##filter", "Filter by path or owner", m_residencyFilter, sizeof(m_residencyFilter));
        if (ImGui::BeginTable(
                "resident", 7,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                ImVec2(0, 200))) {
            for (const char* column : {"Asset", "Kind", "Owners", "Refs", "CPU", "GPU", "Pending free / last error"})
                ImGui::TableSetupColumn(column);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto& e : m_residency.entries()) {
                std::string owners;
                for (const std::string& o : e.owners) owners += (owners.empty() ? "" : ", ") + o;
                if (!matches(e.id, m_residencyFilter) && !matches(owners, m_residencyFilter)) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.id.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(shockwave::assetKindName(e.kind));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(owners.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", e.refs);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatAssetBytes(e.cpuBytes).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatAssetBytes(e.gpuBytes).c_str());
                ImGui::TableNextColumn();
                ImGui::Text(
                    "%s%s%s", formatAssetBytes(e.pendingFreeBytes).c_str(), e.lastError.empty() ? "" : "; ",
                    e.lastError.c_str());
            }
            ImGui::EndTable();
        }
    }

    // The open scene's dependencies, as player --check sees them.
    if (ImGui::CollapsingHeader("Open scene's dependencies")) {
        ImGui::BeginDisabled(m_scenePath.empty());
        if (ImGui::Button("Check dependencies")) {
            shockwave::AssetGraph& graph = assetGraph();
            const std::string id = graph.idFor(std::filesystem::absolute(m_scenePath));
            const auto costs = shockwave::assetCosts(graph, id, m_assetFolder.get(), &engine().assets());
            uint64_t cpu = 0, gpu = 0;
            for (const auto& c : costs) {
                cpu += c.cpuBytes;
                gpu += c.gpuBytes;
            }
            m_sceneDependencyProblems = graph.diagnostics(id);
            m_sceneDependencySummary = id + ": " + std::to_string(costs.size()) + " assets, CPU " +
                                       formatAssetBytes(cpu) + ", GPU " + formatAssetBytes(gpu);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Follows everything the saved scene reaches: missing files, wrong case, paths leaving "
                              "the project, cycles (the same as player --asset-dependencies).");
        if (m_scenePath.empty()) ImGui::TextDisabled("Save the scene first: the check reads it from disk.");
        if (!m_sceneDependencySummary.empty()) ImGui::TextUnformatted(m_sceneDependencySummary.c_str());
        if (!m_sceneDependencySummary.empty() && m_sceneDependencyProblems.empty()) ImGui::TextDisabled("No problems");
        for (const shockwave::Diagnostic& d : m_sceneDependencyProblems)
            ImGui::TextColored(
                d.error() ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s: %s", d.code.c_str(),
                d.text().c_str());
    }
    ImGui::End();
}

}   // namespace editor
