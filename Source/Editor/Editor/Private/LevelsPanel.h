#pragma once

// Project > Levels: edits the level catalog (docs/Levels.md), the same file and rules as
// `player --levels` and `--check`. Add, remove and rename levels, set the entry, pick
// scenes, kinds, spawns and next levels; Validate loads every level's scene in an
// isolated world and follows the exits between them. Saving is atomic; changes undo.

#include <Projects/LevelCatalog.h>
#include <Projects/Project.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace editor {

class LevelsPanel {
public:
    struct Hooks {
        // Opens a scene through the editor's unsaved-changes handling.
        std::function<void(const std::filesystem::path&)> openScene;
        // Opens a scene and starts Play in it (when nothing unsaved is in the way).
        std::function<void(const std::filesystem::path&)> playScene;
        // Loads a scene into an isolated world and reads its level components.
        std::function<shockwave::LevelSceneFacts(const std::filesystem::path&)> sceneFacts;
        // Writes the project manifest (Create level catalog adds its `levels` key).
        std::function<bool(shockwave::Project&, std::string& error)> saveProject;
    };

    bool open = false;
    // Main thread, once a frame. `playing` disables everything that writes.
    void draw(shockwave::Project& project, bool playing, const Hooks& hooks);

    // What the last Validate found, for the release gate: empty before one.
    const std::map<std::string, shockwave::LevelSceneFacts>& facts() const { return m_facts; }

private:
    void load(const shockwave::Project& project);
    void watchFile(const shockwave::Project& project);
    bool save(const shockwave::Project& project);
    void create(shockwave::Project& project, const Hooks& hooks);
    void validate(const shockwave::Project& project, const Hooks& hooks);
    void change();   // records an undo step before an edit
    void drawLevel(const shockwave::Project& project, bool playing, const Hooks& hooks);
    void rename(const std::string& from, const std::string& to);

    std::filesystem::path m_project;
    shockwave::LevelCatalog m_edit, m_saved;
    bool m_loaded = false;
    std::vector<shockwave::Diagnostic> m_loadErrors;   // the file doesn't read: shown, never overwritten
    std::string m_message;   // the last save or create result
    bool m_messageError = false;
    std::filesystem::file_time_type m_fileTime{};
    bool m_changedOnDisk = false;
    double m_nextWatch = 0.0;
    std::vector<shockwave::LevelCatalog> m_undo, m_redo;
    int m_selected = 0;
    std::string m_renameText, m_preloadText;
    std::vector<std::string> m_scenes;   // the project's .swscene files, project-relative
    // The last Validate: diagnostics and each level's facts. Cleared by any edit.
    bool m_validated = false;
    std::vector<shockwave::Diagnostic> m_diagnostics;
    std::map<std::string, shockwave::LevelSceneFacts> m_facts;
};

}   // namespace editor
