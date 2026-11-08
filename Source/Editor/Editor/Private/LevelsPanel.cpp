// Project > Levels: see LevelsPanel.h and docs/Levels.md.
#include "LevelsPanel.h"

#include <Core/Log.h>
#include <UI/Gui.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <SDL3/SDL.h>

#include <algorithm>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::LevelCatalog;
using shockwave::LevelEntry;
using shockwave::LevelKind;

namespace {

SW_LOG_CATEGORY(LogLevels);

double seconds() { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

constexpr size_t kUndoSteps = 64;

std::vector<std::string> projectScenes(const shockwave::Project& project) {
    std::vector<std::string> scenes;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(project.root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name == "Saved" || name == "Source" || (!name.empty() && name[0] == '.'))) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().extension() == ".swscene")
            scenes.push_back(project.relative(it->path()));
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

// A level id not used yet: "level", "level-2", ...
std::string freshId(const LevelCatalog& catalog, const std::string& base) {
    if (!catalog.find(base)) return base;
    for (int n = 2;; ++n)
        if (const std::string id = base + "-" + std::to_string(n); !catalog.find(id)) return id;
}

}   // namespace

// ---- The file -----------------------------------------------------------------------

void LevelsPanel::load(const shockwave::Project& project) {
    m_project = project.file;
    m_loadErrors.clear();
    m_message.clear();
    m_changedOnDisk = false;
    m_validated = false;
    m_diagnostics.clear();
    m_facts.clear();
    m_undo.clear();
    m_redo.clear();
    m_scenes = projectScenes(project);
    m_edit = m_saved = {};
    m_loaded = shockwave::projectLevels(project, m_edit, m_loadErrors);
    if (m_loaded) m_saved = m_edit;
    std::error_code ec;
    m_fileTime = project.levelCatalog.empty() ? fs::file_time_type{} : fs::last_write_time(project.levelCatalog, ec);
    m_selected = std::clamp(m_selected, 0, std::max(0, int(m_edit.levels.size()) - 1));
}

// The catalog is shared with tools and AI agents: a change on disk is picked up when
// nothing is being edited here, and flagged when something is.
void LevelsPanel::watchFile(const shockwave::Project& project) {
    if (m_project != project.file) {
        load(project);
        return;
    }
    const double now = seconds();
    if (now < m_nextWatch || project.levelCatalog.empty()) return;
    m_nextWatch = now + 1.0;
    std::error_code ec;
    const auto time = fs::last_write_time(project.levelCatalog, ec);
    if (ec || time == m_fileTime) return;
    if (m_edit == m_saved)
        load(project);
    else
        m_changedOnDisk = true;
}

bool LevelsPanel::save(const shockwave::Project& project) {
    std::vector<shockwave::Diagnostic> problems;
    // Never write a catalog the player would refuse.
    LevelCatalog check;
    if (!shockwave::parseLevelCatalog(
            shockwave::levelCatalogText(m_edit), project.relative(project.levelCatalog), check, problems)) {
        m_message = problems.empty() ? "the catalog isn't valid" : problems.front().text();
        m_messageError = true;
        return false;
    }
    std::string error;
    if (!shockwave::writeLevelCatalog(project.levelCatalog, m_edit, error)) {
        m_message = error;
        m_messageError = true;
        SW_LOG(LogLevels, Error, "Couldn't save the level catalog: {}", error);
        return false;
    }
    m_saved = m_edit;
    m_changedOnDisk = false;
    std::error_code ec;
    m_fileTime = fs::last_write_time(project.levelCatalog, ec);
    m_message = "Saved " + project.relative(project.levelCatalog);
    m_messageError = false;
    SW_LOG(LogLevels, Info, "Saved {}", project.relative(project.levelCatalog));
    return true;
}

// A catalog from the startup scene; written at once, with the project's `levels` key.
void LevelsPanel::create(shockwave::Project& project, const Hooks& hooks) {
    LevelCatalog catalog;
    LevelEntry level;
    level.id = "main";
    level.scene = project.relative(project.startupScene);
    level.kind = LevelKind::Gameplay;
    catalog.entry = level.id;
    catalog.levels.push_back(std::move(level));
    const fs::path file = project.root / "Config" / "Levels.swlevels";
    std::error_code ec;
    if (fs::exists(file, ec)) {
        m_message = project.relative(file) + " already exists; add `levels \"Config/Levels.swlevels\"` to the project";
        m_messageError = true;
        return;
    }
    std::string error;
    if (!shockwave::writeLevelCatalog(file, catalog, error)) {
        m_message = error;
        m_messageError = true;
        return;
    }
    shockwave::Project updated = project;
    updated.levelCatalog = file;
    if (!hooks.saveProject || !hooks.saveProject(updated, error)) {
        fs::remove(file, ec);
        m_message = "Couldn't add the levels key to the project: " + error;
        m_messageError = true;
        return;
    }
    project = updated;
    load(project);
    m_message = "Created " + project.relative(file);
    m_messageError = false;
}

void LevelsPanel::validate(const shockwave::Project& project, const Hooks& hooks) {
    m_diagnostics.clear();
    m_facts.clear();
    const std::string shown = project.relative(project.levelCatalog);
    // The text as it would be saved, so Validate checks what Save would write.
    LevelCatalog parsed;
    if (!shockwave::parseLevelCatalog(shockwave::levelCatalogText(m_edit), shown, parsed, m_diagnostics)) {
        m_validated = true;
        return;
    }
    shockwave::validateLevelCatalog(project, m_edit, shown, m_diagnostics);
    std::map<std::string, shockwave::LevelSceneFacts> byScene;
    for (const LevelEntry& level : m_edit.levels) {
        auto it = byScene.find(level.scene);
        if (it == byScene.end()) {
            shockwave::LevelSceneFacts facts;
            std::error_code ec;
            if (!fs::is_regular_file(project.root / level.scene, ec))
                facts.loadError = "it doesn't exist";
            else if (hooks.sceneFacts)
                facts = hooks.sceneFacts(project.root / level.scene);
            if (facts.loaded && hooks.sceneProblems)
                for (shockwave::Diagnostic& d : hooks.sceneProblems(project.root / level.scene))
                    m_diagnostics.push_back(std::move(d));
            it = byScene.emplace(level.scene, std::move(facts)).first;
        }
        m_facts[level.id] = it->second;
    }
    shockwave::validateLevelLinks(m_edit, m_facts, shown, m_diagnostics);
    m_validated = true;
}

void LevelsPanel::change() {
    m_undo.push_back(m_edit);
    if (m_undo.size() > kUndoSteps) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_validated = false;
}

// A new id everywhere the catalog names the old one. Exits in scenes aren't rewritten:
// Validate lists the ones still naming the old id.
void LevelsPanel::rename(const std::string& from, const std::string& to) {
    if (from == to) return;
    change();
    for (LevelEntry& level : m_edit.levels) {
        if (level.id == from) level.id = to;
        if (level.next == from) level.next = to;
    }
    if (m_edit.entry == from) m_edit.entry = to;
}

// ---- New and duplicated levels --------------------------------------------------------

namespace {
// A scene file not taken yet: <folder>/<id>.swscene, then <id>-2.swscene, ...
fs::path freshSceneFile(const fs::path& folder, const std::string& id) {
    std::error_code ec;
    fs::path file = folder / (id + ".swscene");
    for (int n = 2; fs::exists(file, ec); ++n) file = folder / (id + "-" + std::to_string(n) + ".swscene");
    return file;
}
}   // namespace

void LevelsPanel::newLevel(const shockwave::Project& project, const Hooks& hooks) {
    const std::string id = freshId(m_edit, "level");
    const fs::path file = freshSceneFile(project.sceneFolder, id);
    std::string error;
    if (!hooks.newScene || !hooks.newScene(file, error)) {
        m_message = "Couldn't write " + project.relative(file) + (error.empty() ? "" : ": " + error);
        m_messageError = true;
        return;
    }
    change();
    LevelEntry level;
    level.id = id;
    level.scene = project.relative(file);
    level.kind = LevelKind::Gameplay;
    m_edit.levels.push_back(level);
    m_selected = int(m_edit.levels.size()) - 1;
    m_renameText.clear();
    m_scenes = projectScenes(project);
    m_message = "Wrote " + level.scene + "; Save adds level \"" + id + "\" to the catalog";
    m_messageError = false;
}

bool LevelsPanel::selectLevel(const std::string& id) {
    for (int i = 0; i < int(m_edit.levels.size()); ++i)
        if (m_edit.levels[i].id == id) {
            m_selected = i;
            m_renameText.clear();
            return true;
        }
    return false;
}

void LevelsPanel::duplicateLevel(const shockwave::Project& project) {
    const LevelEntry source = m_edit.levels[m_selected];
    const std::string id = freshId(m_edit, source.id);
    const fs::path from = project.root / source.scene;
    const fs::path file = freshSceneFile(from.parent_path(), id);
    std::error_code ec;
    if (!fs::copy_file(from, file, ec)) {
        m_message = "Couldn't copy " + source.scene + ": " + (ec ? ec.message() : std::string("it doesn't exist"));
        m_messageError = true;
        return;
    }
    change();
    LevelEntry level = source;   // same kind, spawn, next and preloads
    level.id = id;
    level.scene = project.relative(file);
    m_edit.levels.insert(m_edit.levels.begin() + m_selected + 1, level);
    ++m_selected;
    m_renameText.clear();
    m_scenes = projectScenes(project);
    m_message = "Copied " + source.scene + " to " + level.scene + "; link an exit or a next level to it, then Save";
    m_messageError = false;
}

// ---- Window -------------------------------------------------------------------------

void LevelsPanel::draw(shockwave::Project& project, bool playing, const Hooks& hooks) {
    if (!open) return;
    watchFile(project);
    ImGui::SetNextWindowSize(ImVec2(720, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Levels", &open)) {
        ImGui::End();
        return;
    }
    const ImVec4 errorColor = gui::semanticColor(gui::currentTheme().error);
    const ImVec4 warningColor = gui::semanticColor(gui::currentTheme().warning);
    if (!m_message.empty())
        ImGui::TextColored(
            m_messageError ? errorColor : gui::semanticColor(gui::currentTheme().textDisabled), "%s",
            m_message.c_str());

    if (project.levelCatalog.empty()) {
        ImGui::TextWrapped(
            "No level catalog: the startup scene (%s) is the game's only level, and everything works as it is. "
            "A catalog lists the game's levels, which one it starts in and where each one leads.",
            project.relative(project.startupScene).c_str());
        ImGui::BeginDisabled(playing);
        if (gui::button("Create level catalog")) create(project, hooks);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Writes Config/Levels.swlevels with the startup scene as level \"main\"");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s (player --levels)", project.relative(project.levelCatalog).c_str());
    if (!m_loaded) {
        for (const auto& d : m_loadErrors) ImGui::TextColored(errorColor, "%s", d.text().c_str());
        ImGui::TextDisabled("Fix the file by hand; the editor won't overwrite it.");
        if (gui::button("Reload")) load(project);
        ImGui::End();
        return;
    }
    if (m_changedOnDisk) {
        ImGui::TextColored(warningColor, "The file changed on disk. Save overwrites it; Revert loads it.");
    }

    // ---- Toolbar ----
    const bool changed = !(m_edit == m_saved);
    ImGui::BeginDisabled(playing);
    if (gui::button("New level")) newLevel(project, hooks);
    ImGui::SetItemTooltip("Write a new scene with a sky, a camera and ground, and add it as a gameplay level");
    ImGui::SameLine();
    if (gui::button("Add level")) {
        change();
        LevelEntry level;
        level.id = freshId(m_edit, "level");
        level.scene = m_scenes.empty() ? project.relative(project.startupScene) : m_scenes.front();
        m_edit.levels.push_back(level);
        m_selected = int(m_edit.levels.size()) - 1;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    if (gui::button("Save")) save(project);
    ImGui::SameLine();
    if (gui::button("Revert")) {
        load(project);
        m_message = "Reverted";
        m_messageError = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_undo.empty());
    if (gui::button("Undo")) {
        m_redo.push_back(m_edit);
        m_edit = m_undo.back();
        m_undo.pop_back();
        m_validated = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redo.empty());
    if (gui::button("Redo")) {
        m_undo.push_back(m_edit);
        m_edit = m_redo.back();
        m_redo.pop_back();
        m_validated = false;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();   // playing
    ImGui::SameLine();
    if (gui::button("Validate")) validate(project, hooks);
    ImGui::SetItemTooltip(
        "Load every level's scene, follow the exits between them and run the scene checks (as player --check does)");
    ImGui::SameLine();
    {
        const LevelEntry* entry = m_edit.find(m_edit.entry);
        ImGui::BeginDisabled(playing || !entry || !hooks.playScene);
        if (gui::button("Play from entry") && entry) hooks.playScene(project.root / entry->scene);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Open the entry level's scene and Play it: the game from its start");
    }
    if (playing) ImGui::TextDisabled("Stop Play to change the catalog.");

    // ---- Levels ----
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    const float tableHeight = std::min(
        ImGui::GetTextLineHeightWithSpacing() * (float(m_edit.levels.size()) + 1.5f),
        ImGui::GetContentRegionAvail().y * 0.4f);
    if (ImGui::BeginTable("levels", 5, flags, ImVec2(0, tableHeight))) {
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Scene");
        ImGui::TableSetupColumn("Next");
        ImGui::TableSetupColumn("Exits");
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(m_edit.levels.size()); ++i) {
            const LevelEntry& level = m_edit.levels[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = level.id + (level.id == m_edit.entry ? "  (entry)" : "");
            if (ImGui::Selectable(label.c_str(), m_selected == i, ImGuiSelectableFlags_SpanAllColumns)) {
                m_selected = i;
                m_renameText.clear();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hooks.openScene)
                hooks.openScene(project.root / level.scene);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(shockwave::levelKindName(level.kind));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(level.scene.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(level.next.empty() ? "-" : level.next.c_str());
            ImGui::TableNextColumn();
            const auto it = m_facts.find(level.id);
            if (!m_validated || it == m_facts.end()) {
                ImGui::TextDisabled("Validate to see");
            } else {
                std::string exits;
                for (const auto& exit : it->second.exits)
                    exits += (exits.empty() ? "" : ", ") + exit.destination +
                             (exit.spawn.empty() ? "" : " (" + exit.spawn + ")");
                ImGui::TextUnformatted(exits.empty() ? "none" : exits.c_str());
            }
        }
        ImGui::EndTable();
    }

    if (!m_edit.levels.empty()) {
        m_selected = std::clamp(m_selected, 0, int(m_edit.levels.size()) - 1);
        drawLevel(project, playing, hooks);
    }

    drawFindings(project, hooks);
    ImGui::End();
}

// What the last Validate found, under the kind of fix each needs, so "the level won't
// run" is never the whole story. Double-click a finding to open its file's scene.
void LevelsPanel::drawFindings(const shockwave::Project& project, const Hooks& hooks) {
    if (!m_validated) return;
    const ImVec4 errorColor = gui::semanticColor(gui::currentTheme().error);
    const ImVec4 warningColor = gui::semanticColor(gui::currentTheme().warning);
    ImGui::SeparatorText("Validate");
    size_t errors = 0;
    int counts[shockwave::kProblemKinds] = {};
    for (const auto& d : m_diagnostics) {
        errors += d.error();
        ++counts[int(shockwave::problemKindOf(d))];
    }
    if (m_diagnostics.empty()) {
        ImGui::TextColored(
            gui::semanticColor(gui::currentTheme().accent),
            "Every level loads, every link resolves and every check passes.");
        return;
    }
    ImGui::Text(
        "%zu error%s, %zu warning%s", errors, errors == 1 ? "" : "s", m_diagnostics.size() - errors,
        m_diagnostics.size() - errors == 1 ? "" : "s");
    // One filter button per kind that has findings.
    if (ImGui::RadioButton("All", m_kindFilter < 0)) m_kindFilter = -1;
    for (int kind = 0; kind < shockwave::kProblemKinds; ++kind) {
        if (counts[kind] == 0) continue;
        ImGui::SameLine();
        const std::string label = std::string(shockwave::problemKindName(shockwave::ProblemKind(kind))) + " (" +
                                  std::to_string(counts[kind]) + ")";
        if (ImGui::RadioButton(label.c_str(), m_kindFilter == kind)) m_kindFilter = kind;
    }
    if (m_kindFilter >= 0 && counts[m_kindFilter] == 0) m_kindFilter = -1;
    for (int kind = 0; kind < shockwave::kProblemKinds; ++kind) {
        if (counts[kind] == 0 || (m_kindFilter >= 0 && m_kindFilter != kind)) continue;
        ImGui::TextDisabled("%s", shockwave::problemKindTitle(shockwave::ProblemKind(kind)));
        ImGui::Indent();
        for (size_t i = 0; i < m_diagnostics.size(); ++i) {
            const auto& d = m_diagnostics[i];
            if (int(shockwave::problemKindOf(d)) != kind) continue;
            ImGui::PushID(int(i));
            ImGui::PushStyleColor(ImGuiCol_Text, d.error() ? errorColor : warningColor);
            ImGui::Selectable(d.text().c_str());
            ImGui::PopStyleColor();
            if (!d.code.empty()) ImGui::SetItemTooltip("%s", d.code.c_str());
            const fs::path file = project.root / d.file;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hooks.openScene &&
                file.extension() == ".swscene")
                hooks.openScene(file);
            ImGui::PopID();
        }
        ImGui::Unindent();
    }
}

void LevelsPanel::drawLevel(const shockwave::Project& project, bool playing, const Hooks& hooks) {
    LevelEntry& level = m_edit.levels[m_selected];
    ImGui::SeparatorText(("Level " + level.id).c_str());
    ImGui::BeginDisabled(playing);
    if (gui::beginPropertyGrid("level")) {
        gui::propertyLabel("ID");
        if (m_renameText.empty()) m_renameText = level.id;
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Rename").x - ImGui::GetStyle().FramePadding.x * 4);
        ImGui::InputText("##id", &m_renameText);
        const bool valid = shockwave::validLevelId(m_renameText);
        const bool taken = m_renameText != level.id && m_edit.find(m_renameText);
        ImGui::SameLine();
        ImGui::BeginDisabled(!valid || taken || m_renameText == level.id);
        if (gui::button("Rename")) {
            const std::string from = level.id, to = m_renameText;
            rename(from, to);
        }
        ImGui::EndDisabled();
        if (!valid) ImGui::SetItemTooltip("1 to 64 lowercase letters, digits, '-' and '_', starting with a letter");
        if (taken) ImGui::SetItemTooltip("Another level has that id");

        LevelEntry& current = m_edit.levels[m_selected];   // rename may have changed the vector's contents
        gui::propertyLabel("Entry");
        bool entry = m_edit.entry == current.id;
        ImGui::BeginDisabled(entry);
        if (ImGui::Checkbox("##entry", &entry) && entry) {
            change();
            m_edit.entry = m_edit.levels[m_selected].id;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The level the game starts in");

        gui::propertyLabel("Scene");
        if (ImGui::BeginCombo("##scene", current.scene.c_str())) {
            for (const std::string& scene : m_scenes)
                if (ImGui::Selectable(scene.c_str(), scene == current.scene) && scene != current.scene) {
                    change();
                    m_edit.levels[m_selected].scene = scene;
                }
            ImGui::EndCombo();
        }

        gui::propertyLabel("Kind");
        int kind = int(m_edit.levels[m_selected].kind);
        if (ImGui::Combo("##kind", &kind, "menu\0gameplay\0ending\0") && kind != int(m_edit.levels[m_selected].kind)) {
            change();
            m_edit.levels[m_selected].kind = LevelKind(kind);
        }

        gui::propertyLabel("Default spawn");
        {
            LevelEntry& l = m_edit.levels[m_selected];
            const auto facts = m_facts.find(l.id);
            if (ImGui::BeginCombo(
                    "##spawn", l.defaultSpawn.empty() ? "(the scene's own start)" : l.defaultSpawn.c_str())) {
                if (ImGui::Selectable("(the scene's own start)", l.defaultSpawn.empty()) && !l.defaultSpawn.empty()) {
                    change();
                    m_edit.levels[m_selected].defaultSpawn.clear();
                }
                if (facts != m_facts.end())
                    for (const auto& spawn : facts->second.spawns)
                        if (ImGui::Selectable(spawn.id.c_str(), spawn.id == l.defaultSpawn) &&
                            spawn.id != l.defaultSpawn) {
                            change();
                            m_edit.levels[m_selected].defaultSpawn = spawn.id;
                        }
                ImGui::EndCombo();
            }
            if (facts == m_facts.end()) ImGui::SetItemTooltip("Validate to pick from the scene's LevelSpawns");
        }

        gui::propertyLabel("Next");
        {
            const LevelEntry& l = m_edit.levels[m_selected];
            if (ImGui::BeginCombo("##next", l.next.empty() ? "(none)" : l.next.c_str())) {
                if (ImGui::Selectable("(none)", l.next.empty()) && !l.next.empty()) {
                    change();
                    m_edit.levels[m_selected].next.clear();
                }
                for (const LevelEntry& other : m_edit.levels) {
                    const std::string id = other.id;
                    if (ImGui::Selectable(id.c_str(), id == m_edit.levels[m_selected].next) &&
                        id != m_edit.levels[m_selected].next) {
                        change();
                        m_edit.levels[m_selected].next = id;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Where the game goes by default when the level is done; never automatic");
        }

        gui::propertyLabel("Preload");
        {
            int remove = -1;
            const auto& preload = m_edit.levels[m_selected].preload;
            for (int i = 0; i < int(preload.size()); ++i) {
                ImGui::PushID(i);
                ImGui::TextUnformatted(preload[i].c_str());
                ImGui::SameLine();
                if (gui::button("Remove")) remove = i;
                ImGui::PopID();
            }
            if (remove >= 0) {
                change();
                m_edit.levels[m_selected].preload.erase(m_edit.levels[m_selected].preload.begin() + remove);
            }
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Add").x - ImGui::GetStyle().FramePadding.x * 4);
            ImGui::InputTextWithHint("##preload", "Content/UI/loading.rml", &m_preloadText);
            ImGui::SameLine();
            ImGui::BeginDisabled(m_preloadText.empty());
            if (gui::button("Add")) {
                change();
                m_edit.levels[m_selected].preload.push_back(m_preloadText);
                m_preloadText.clear();
            }
            ImGui::EndDisabled();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();   // playing

    const fs::path scene = project.root / m_edit.levels[m_selected].scene;
    if (gui::button("Open scene") && hooks.openScene) hooks.openScene(scene);
    ImGui::SameLine();
    ImGui::BeginDisabled(playing);
    if (gui::button("Play level") && hooks.playScene) hooks.playScene(scene);
    ImGui::SetItemTooltip("Open the level's scene and Play it");
    ImGui::SameLine();
    if (gui::button("Duplicate level")) duplicateLevel(project);
    ImGui::SetItemTooltip("Copy the level's scene to a new file and add it as a new level beside this one");
    ImGui::SameLine();
    ImGui::BeginDisabled(m_edit.levels.size() <= 1);
    if (gui::button("Remove level")) {
        change();
        const std::string id = m_edit.levels[m_selected].id;
        m_edit.levels.erase(m_edit.levels.begin() + m_selected);
        for (LevelEntry& l : m_edit.levels)
            if (l.next == id) l.next.clear();
        if (m_edit.entry == id) m_edit.entry = m_edit.levels.front().id;
        m_selected = std::max(0, m_selected - 1);
        m_renameText.clear();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

}   // namespace editor
