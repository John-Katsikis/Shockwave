// The Save Games window: the project's save slots (Saved/SaveGames/*.swsave) with what
// each holds. During Play it saves the running game to a slot and loads one back, the
// same way game.saves() does; while editing, "Play from save" starts Play and loads it.
// See docs/SaveGames.md.
#include "Editor.h"

#include <GameFramework/SaveGame.h>
#include <UI/Gui.h>

#include <imgui_stdlib.h>

#include <format>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;

namespace {

std::string playTimeText(double seconds) {
    const auto total = static_cast<long long>(seconds);
    if (total >= 3600) return std::format("{}h {:02}m", total / 3600, (total / 60) % 60);
    return std::format("{}m {:02}s", total / 60, total % 60);
}

}   // namespace

void Editor::drawSaveGamesWindow() {
    ImGui::SetNextWindowSize(ImVec2(760, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Save Games", &m_showSaveGames)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject) {
        ImGui::TextDisabled("Open a project to see its save games.");
        ImGui::End();
        return;
    }
    shockwave::SaveGames* saves = m_session ? m_session->saves() : nullptr;
    // Saving happens on a worker, so the list catches up a moment later by itself.
    m_saveListAge += ImGui::GetIO().DeltaTime;
    if (m_saveListAge > 1.0) {
        m_saveList = shockwave::listSaveGames(saveGamesFolder());
        m_saveListAge = 0;
    }

    // ---- Save the running game ----
    if (saves) {
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##slot", "slot name", &m_saveSlotEdit);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##title", "title (optional)", &m_saveTitleEdit);
        ImGui::SameLine();
        const bool valid = shockwave::validSaveSlot(m_saveSlotEdit);
        ImGui::BeginDisabled(!valid);
        if (gui::button("Save")) {
            if (saves->save(m_saveSlotEdit, m_saveTitleEdit)) {
                log("Saving the game to slot \"" + m_saveSlotEdit + "\"");
                m_saveSelected = m_saveSlotEdit;
            } else {
                warn(saves->lastError());
            }
            m_saveListAge = 0.8;   // re-read shortly, once the worker has written it
        }
        ImGui::EndDisabled();
        if (!valid && !m_saveSlotEdit.empty()) ImGui::TextDisabled("Slot names use letters, digits, spaces, - and _.");
    } else {
        ImGui::TextDisabled("Press Play to save the running game. Saved games can be played from here.");
    }
    ImGui::Separator();

    // ---- Slots ----
    if (m_saveList.empty()) {
        ImGui::TextDisabled("No save games yet in %s", m_project.relative(saveGamesFolder()).c_str());
        ImGui::End();
        return;
    }
    const shockwave::SaveInfo* selected = nullptr;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float tableHeight = ImGui::GetContentRegionAvail().y * 0.6f;
    if (ImGui::BeginTable("save_slots", 6, flags, ImVec2(0, tableHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        const auto stretch = ImGuiTableColumnFlags_WidthStretch;
        ImGui::TableSetupColumn("Slot", stretch, 1.1f);
        ImGui::TableSetupColumn("Title", stretch, 1.3f);
        ImGui::TableSetupColumn("Scene", stretch, 1.5f);
        ImGui::TableSetupColumn("Saved (UTC)", stretch, 1.5f);
        ImGui::TableSetupColumn("Play time", stretch, 0.7f);
        ImGui::TableSetupColumn("Entities", stretch, 0.6f);
        ImGui::TableHeadersRow();
        for (const shockwave::SaveInfo& info : m_saveList) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool isSelected = info.slot == m_saveSelected;
            if (isSelected) selected = &info;
            if (ImGui::Selectable(
                    info.slot.c_str(), isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                m_saveSelected = info.slot;
                m_saveSlotEdit = info.slot;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && saves) saves->load(info.slot);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(info.title.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(info.scene.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(info.savedAt.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(playTimeText(info.playTime).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", info.entities);
        }
        ImGui::EndTable();
    }

    // ---- The selected slot ----
    if (!selected) {
        ImGui::TextDisabled("Select a slot to load or delete it. Double-click loads it during Play.");
        ImGui::End();
        return;
    }
    const std::string slot = selected->slot;
    const fs::path file = selected->file;
    if (saves) {
        if (gui::button("Load")) {
            if (saves->load(slot))
                log("Loading slot \"" + slot + "\"");
            else
                warn(saves->lastError());
        }
    } else {
        ImGui::BeginDisabled(!m_gameModuleMatches);
        if (gui::button("Play from save")) {
            play();
            if (m_session && m_session->saves()) {
                if (m_session->saves()->load(slot))
                    log("Loading slot \"" + slot + "\"");
                else
                    warn(m_session->saves()->lastError());
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (gui::button("Delete")) {
        std::error_code ec;
        if (fs::remove(file, ec)) {
            log("Deleted save " + m_project.relative(file));
            m_saveSelected.clear();
        } else {
            warn("Couldn't delete " + m_project.relative(file) + (ec ? ": " + ec.message() : ""));
        }
        m_saveListAge = 1e9;
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_project.relative(file).c_str());
    if (gui::beginPropertyGrid("save_values")) {
        if (selected->values.empty()) {
            gui::propertyLabel("Values");
            ImGui::TextDisabled("none (game.saves().set adds them)");
        }
        for (const auto& [key, value] : selected->values) {
            gui::propertyLabel(key.c_str());
            ImGui::TextUnformatted(value.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}   // namespace editor
