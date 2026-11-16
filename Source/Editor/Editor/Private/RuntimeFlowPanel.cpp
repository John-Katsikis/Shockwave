// The Runtime flow window (S15, docs/LevelLifetime.md#the-runtime-flow-window): during
// Play, the level playing and its epoch, the travel under way phase by phase, who has the
// input, and the travels so far with their codes. Restart level, Return to menu and Cancel
// go through the same GameFlow calls the game makes, so they refuse what the game would be
// refused (Busy, TooLateToCancel, ...).
#include "Editor.h"

#include <GameFramework/GameFlow.h>
#include <Input/Input.h>

#include <cstdio>
#include <string>

namespace editor {

namespace {

const char* sessionStateName(shockwave::PlaySession::State state) {
    switch (state) {
        case shockwave::PlaySession::State::Editing: return "editing";
        case shockwave::PlaySession::State::Playing: return "playing";
        case shockwave::PlaySession::State::Paused: return "paused";
    }
    return "?";
}

std::string levelText(const shockwave::LevelRequest& level) {
    if (level.level.empty()) return "(none)";
    return level.spawn.empty() ? level.level : level.level + " at " + level.spawn;
}

}   // namespace

void Editor::drawRuntimeFlowWindow() {
    ImGui::SetNextWindowSize(ImVec2(680, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Runtime flow", &m_showRuntimeFlow)) {
        ImGui::End();
        return;
    }
    shockwave::LevelHost* host = m_runtimeLevels.get();
    if (!host) {
        ImGui::TextWrapped(
            "Not playing. Press Play: this window then shows the level playing, its epoch, any travel phase "
            "by phase, who has the input, and every travel's result and code.");
        ImGui::End();
        return;
    }

    // ---- Now ----
    const shockwave::PlaySession* session = host->session();
    ImGui::SeparatorText("Now");
    if (ImGui::BeginTable("now", 2, ImGuiTableFlags_SizingFixedFit)) {
        auto row = [](const char* name, const std::string& value, const char* tip = nullptr) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", name);
            if (tip) ImGui::SetItemTooltip("%s", tip);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(value.c_str());
        };
        row("Level", levelText(host->currentLevel()));
        row("Epoch", std::to_string(host->currentEpoch()),
            "Goes up by one for every level started. A callback from a level that has ended carries an older "
            "epoch, and its travel requests are refused as StaleLevel.");
        row("Session",
            session ? std::string(sessionStateName(session->state())) + ", tick " + std::to_string(session->ticks())
                    : std::string("none"));
        row("Input", shockwave::inputLayerName(engine().input().layer()),
            "The highest input layer raised (docs/Input.md): gameplay, hud, menu, loading or modal. Gameplay "
            "actions only reach the game at gameplay or hud.");
        row("Travel mode", host->staged() ? "staged (two frames per change)" : "instant");
        row("Menu level", host->menuLevel().empty() ? "(none)" : host->menuLevel());
        if (!host->failure().empty()) row("Last failure", host->failure());
        ImGui::EndTable();
    }

    // ---- The travel under way ----
    ImGui::SeparatorText("Travel");
    const uint64_t active = host->activeTicket();
    if (active == 0) {
        ImGui::TextDisabled("None under way.");
    } else if (auto status = host->status(active)) {
        ImGui::Text(
            "Ticket %llu: %s -> %s (%s)", static_cast<unsigned long long>(active), status->from.c_str(),
            levelText({status->request.level, status->request.spawn}).c_str(),
            shockwave::travelReasonName(status->request.reason));
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "%s %.0f%%", shockwave::travelPhaseName(status->phase),
            status->progress * 100.0f);
        ImGui::ProgressBar(status->progress, ImVec2(-1, 0), overlay);
        ImGui::BeginDisabled(!status->cancellable);
        if (ImGui::Button("Cancel travel")) {
            const shockwave::TravelError error = host->cancel(active);
            if (error != shockwave::TravelError::None)
                log(std::string("Couldn't cancel the travel: ") + shockwave::travelErrorName(error));
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Until the current level starts retiring; after that the travel carries on "
                              "(TooLateToCancel).");
    }
    ImGui::BeginDisabled(!host->playing());
    if (ImGui::Button("Restart level")) {
        const shockwave::TravelTicket t = host->restartLevel();
        if (!t) log(std::string("Restart refused: ") + shockwave::travelErrorName(t.error) + ": " + t.message);
    }
    ImGui::SetItemTooltip("The current level again, at the spawn it was entered at (reason restart).");
    ImGui::SameLine();
    ImGui::BeginDisabled(host->menuLevel().empty());
    if (ImGui::Button("Return to menu")) {
        const shockwave::TravelTicket t = host->returnToMenu();
        if (!t) log(std::string("Return to menu refused: ") + shockwave::travelErrorName(t.error) + ": " + t.message);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The catalog's menu level (reason menu).");
    ImGui::EndDisabled();

    // ---- Travels so far ----
    ImGui::SeparatorText("Travels so far");
    const auto& results = host->results();
    if (results.empty()) {
        ImGui::TextDisabled("None yet.");
    } else if (ImGui::BeginTable("travels", 6,
                   ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                       ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ticket", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Epoch", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Why");
        ImGui::TableHeadersRow();
        for (auto it = results.rbegin(); it != results.rend(); ++it) {
            const auto status = host->status(it->id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", static_cast<unsigned long long>(it->id));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(levelText(it->request).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(shockwave::travelStatusName(it->status));
            if (status && status->durationMs > 0) ImGui::SetItemTooltip("%.1f ms", status->durationMs);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(status ? shockwave::travelErrorName(status->code) : "");
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(it->epoch));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextWrapped("%s", it->error.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}   // namespace editor
