// Project > Release readiness: see ReleaseWindow.h and docs/Release.md.
#include "ReleaseWindow.h"

#include <Core/Log.h>
#include <Engine/GraphicsSettings.h>
#include <Multiplayer/NetworkSettings.h>
#include <Packaging/Package.h>
#include <UI/Gui.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
namespace net = shockwave::net;
using shockwave::GateStatus;
using shockwave::ReleaseField;
using shockwave::ReleaseFieldKind;
using shockwave::ReleaseSettings;

namespace {

SW_LOG_CATEGORY(LogRelease);

double seconds() { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

ImVec4 gateColor(GateStatus status) {
    const gui::Theme& theme = gui::currentTheme();
    switch (status) {
    case GateStatus::Passed: return gui::semanticColor(theme.accent);
    case GateStatus::Failed: return gui::semanticColor(theme.error);
    case GateStatus::Unverified: return gui::semanticColor(theme.warning);
    case GateStatus::NotApplicable: return gui::semanticColor(theme.textDisabled);
    }
    return gui::semanticColor(theme.text);
}

// "com.example.<module>": a starting point people replace with their own domain.
std::string suggestedApplicationId(const shockwave::Project& project) {
    std::string part;
    for (const char c : project.gameModule)
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') part += c;
    if (part.empty() || part[0] < 'a' || part[0] > 'z') part = "game" + part;
    return "com.example." + part;
}

}   // namespace

shockwave::ReleaseInputs releaseInputs(const shockwave::Project& project) {
    shockwave::ReleaseInputs inputs;
    inputs.projectTickRate = project.tickRate;
    inputs.buildConfig = shockwave::engineBuildConfig();
    shockwave::PackageSettings package;
    std::string error;
    inputs.packageFile = project.relative(shockwave::packageSettingsPath(project));
    if (shockwave::loadPackageSettings(project, package, error)) inputs.packageVersion = package.version;
    const fs::path network = net::networkSettingsPath(project);
    inputs.networkFile = project.relative(network);
    std::error_code ec;
    inputs.networkPresent = fs::exists(network, ec);
    if (inputs.networkPresent) {
        net::NetworkSettingsFile file;
        if (!net::readNetworkSettings(network, file, error)) {
            inputs.networkError = inputs.networkFile + " doesn't read (" + error + ")";
        } else {
            inputs.networkEnabled = file.settings.enabled;
            inputs.networkSteam = file.settings.transport == net::NetTransportKind::Steam;
            inputs.networkMaxPlayers = file.settings.maxPlayers;
            inputs.steamAppId = file.settings.steamAppId;
        }
    }
    shockwave::GraphicsSettings graphics;
    if (shockwave::projectGraphicsSettings(project, graphics, error))
        inputs.graphicsMemoryBudgetMb = graphics.memoryBudgetMb;
    inputs.levelCatalogPresent = fs::exists(project.root / "Config" / "Levels.swlevels", ec);
    return inputs;
}

// ---- The file -----------------------------------------------------------------------

void ReleaseWindow::load(const shockwave::Project& project) {
    m_project = project.file;
    m_loadError.clear();
    m_saveError.clear();
    m_inputs = releaseInputs(project);
    const fs::path path = shockwave::releasePath(project);
    std::error_code ec;
    m_present = m_savedPresent = fs::exists(path, ec);
    m_edit = m_saved = {};
    if (m_present) {
        shockwave::ReleaseFile file;
        shockwave::Diagnostic error;
        if (shockwave::readRelease(path, project.relative(path), file, error))
            m_edit = m_saved = file.settings;
        else
            m_loadError = error.text();
    }
    m_fileTime = fs::last_write_time(path, ec);
}

// The contract is shared with tools and AI agents: when it changes on disk and nothing is
// being edited here, the window shows the new one. The other settings it's checked against
// are read again at the same time.
void ReleaseWindow::watchFile(const shockwave::Project& project) {
    if (m_project != project.file) {
        load(project);
        return;
    }
    const double now = seconds();
    if (now < m_nextWatch) return;
    m_nextWatch = now + 1.0;
    std::error_code ec;
    const auto time = fs::last_write_time(shockwave::releasePath(project), ec);
    const bool editing = m_present != m_savedPresent || !(m_edit == m_saved);
    if (time != m_fileTime && !editing) load(project);
    m_inputs = releaseInputs(project);
}

bool ReleaseWindow::save(const shockwave::Project& project) {
    const fs::path path = shockwave::releasePath(project);
    if (!shockwave::writeRelease(path, m_edit, m_saveError)) {
        SW_LOG(LogRelease, Error, "Couldn't save the release contract: {}", m_saveError);
        return false;
    }
    m_saved = m_edit;
    m_savedPresent = true;
    std::error_code ec;
    m_fileTime = fs::last_write_time(path, ec);
    SW_LOG(LogRelease, Info, "Saved {}", project.relative(path));
    return true;
}

// A new contract, filled in from what the project already says; nothing is saved until Save.
void ReleaseWindow::create(const shockwave::Project& project) {
    ReleaseSettings s;
    s.applicationId = suggestedApplicationId(project);
    if (!m_inputs.packageVersion.empty()) s.releaseVersion = m_inputs.packageVersion;
    s.platforms = {shockwave::packagePlatform()};
    s.modes = {shockwave::ReleaseMode::Offline};
    if (m_inputs.networkEnabled) {
        s.modes.push_back(shockwave::ReleaseMode::Online);
        s.maxPlayers = m_inputs.networkMaxPlayers;
    }
    s.simulationHz = uint32_t(std::clamp(std::lround(project.tickRate), 1L, 1000L));
    s.gpuBudgetMib = std::max<uint32_t>(s.gpuBudgetMib, m_inputs.graphicsMemoryBudgetMb);
    s.acceptanceScenario = "Tests/complete_game.json";
    m_edit = s;
    m_present = true;
}

// ---- Window -------------------------------------------------------------------------

void ReleaseWindow::draw(const shockwave::Project& project, bool playing) {
    if (!open) return;
    watchFile(project);
    ImGui::SetNextWindowSize(ImVec2(640, 680), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Release readiness", &open)) {
        ImGui::End();
        return;
    }
    const ImVec4 errorColor = gui::semanticColor(gui::currentTheme().error);
    const ImVec4 warningColor = gui::semanticColor(gui::currentTheme().warning);
    const std::string shown = project.relative(shockwave::releasePath(project));
    ImGui::TextDisabled("%s (player --release-info)", shown.c_str());

    if (!m_loadError.empty()) {
        ImGui::TextColored(errorColor, "%s", m_loadError.c_str());
        ImGui::TextDisabled("Fix the file by hand; the editor won't overwrite it.");
        if (gui::button("Reload")) load(project);
        ImGui::End();
        return;
    }

    shockwave::ReleaseReport report;
    report.file = shown;
    report.present = report.readable = m_present;
    if (!m_present) {
        ImGui::TextWrapped("No release contract: this is a development project, and everything works as it is. "
                           "A release needs one to say what it promises: its ID, platforms, modes, player counts "
                           "and budgets.");
        if (gui::button("Create release contract")) create(project);
        ImGui::SetItemTooltip("Fill one in from the project's settings; nothing is written until Save");
    } else {
        drawFields();
        report.settings = m_edit;
        shockwave::ReleaseInputs inputs = m_inputs;
        std::error_code ec;
        inputs.acceptanceScenarioExists =
            !m_edit.acceptanceScenario.empty() && fs::is_regular_file(project.root / m_edit.acceptanceScenario, ec);
        // The same rules --check applies, against the project's saved settings.
        shockwave::validateRelease(m_edit, &inputs, shown, nullptr, report.diagnostics);
        for (const shockwave::Diagnostic& d : report.diagnostics)
            ImGui::TextColored(d.error() ? errorColor : warningColor, "%s", d.text().c_str());
        shockwave::releaseGates(report, inputs);

        const bool changed = !m_savedPresent || !(m_edit == m_saved);
        ImGui::BeginDisabled(!changed || shockwave::hasErrors(report.diagnostics) || playing);
        if (gui::button("Save")) save(project);
        ImGui::EndDisabled();
        if (playing) ImGui::SetItemTooltip("Stop Play first");
        ImGui::SameLine();
        ImGui::BeginDisabled(!changed);
        if (gui::button("Revert")) {
            m_edit = m_saved;
            m_present = m_savedPresent;
        }
        ImGui::EndDisabled();
        if (!m_saveError.empty()) ImGui::TextColored(errorColor, "%s", m_saveError.c_str());
    }
    if (!m_present) shockwave::releaseGates(report, m_inputs);
    drawGates(report);
    ImGui::End();
}

void ReleaseWindow::drawFields() {
    if (!gui::beginPropertyGrid("release")) return;
    for (const ReleaseField& field : shockwave::releaseFields()) {
        gui::propertyLabel(field.label);
        ImGui::PushID(field.key);
        switch (field.kind) {
        case ReleaseFieldKind::Id:
        case ReleaseFieldKind::Version:
        case ReleaseFieldKind::Path:
            ImGui::InputText("##v", &(m_edit.*std::get<std::string ReleaseSettings::*>(field.member)));
            break;
        case ReleaseFieldKind::Platforms: {
            auto& list = m_edit.*std::get<std::vector<std::string> ReleaseSettings::*>(field.member);
            for (const char* name : field.choices) {
                bool on = std::find(list.begin(), list.end(), name) != list.end();
                if (ImGui::Checkbox(name, &on)) {
                    if (on)
                        list.emplace_back(name);
                    else
                        std::erase(list, name);
                }
            }
            break;
        }
        case ReleaseFieldKind::Modes: {
            auto& list = m_edit.*std::get<std::vector<shockwave::ReleaseMode> ReleaseSettings::*>(field.member);
            for (size_t i = 0; i < field.choices.size(); ++i) {
                const auto mode = shockwave::ReleaseMode(i);
                bool on = std::find(list.begin(), list.end(), mode) != list.end();
                if (i) ImGui::SameLine();
                if (ImGui::Checkbox(field.choices[i], &on)) {
                    if (on)
                        list.push_back(mode);
                    else
                        std::erase(list, mode);
                }
            }
            break;
        }
        case ReleaseFieldKind::Choice: {
            auto& value = m_edit.*std::get<shockwave::ReleaseGraphicsTier ReleaseSettings::*>(field.member);
            const size_t index = size_t(value);
            if (ImGui::BeginCombo("##v", index < field.choices.size() ? field.choices[index] : "?")) {
                for (size_t i = 0; i < field.choices.size(); ++i)
                    if (ImGui::Selectable(field.choices[i], i == index)) value = shockwave::ReleaseGraphicsTier(i);
                ImGui::EndCombo();
            }
            break;
        }
        case ReleaseFieldKind::Integer: {
            // Shown as typed; out-of-range values are listed below, not clamped.
            auto& value = m_edit.*std::get<uint32_t ReleaseSettings::*>(field.member);
            int64_t v = value;
            if (ImGui::InputScalar("##v", ImGuiDataType_S64, &v, nullptr, nullptr, "%lld"))
                value = uint32_t(std::clamp<int64_t>(v, 0, int64_t(UINT32_MAX)));
            break;
        }
        case ReleaseFieldKind::Seconds:
            ImGui::InputDouble(
                "##v", &(m_edit.*std::get<double ReleaseSettings::*>(field.member)), 1.0, 10.0, "%.1f s");
            break;
        }
        ImGui::SetItemTooltip("%s (%s)", field.meaning, field.key);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void ReleaseWindow::drawGates(const shockwave::ReleaseReport& report) {
    ImGui::SeparatorText("Gates");
    size_t counts[4] = {};
    for (const shockwave::ReleaseGate& gate : report.gates) ++counts[int(gate.status)];
    ImGui::TextColored(
        gateColor(report.qualified() ? GateStatus::Passed : GateStatus::Unverified),
        "%s: %zu passed, %zu failed, %zu unverified, %zu not applicable",
        report.qualified() ? "Qualified" : "Not qualified yet", counts[0], counts[1], counts[2], counts[3]);
    ImGui::TextDisabled("Worked out from the project's files and evidence; nothing here is ticked by hand.");
    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("gates", 4, flags)) return;
    ImGui::TableSetupColumn("Gate");
    ImGui::TableSetupColumn("Status");
    ImGui::TableSetupColumn("Batch");
    ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const shockwave::ReleaseGate& gate : report.gates) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(gate.id.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(gateColor(gate.status), "%s", shockwave::gateStatusName(gate.status));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(gate.batch.c_str());
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", gate.reason.c_str());
    }
    ImGui::EndTable();
}

}   // namespace editor
