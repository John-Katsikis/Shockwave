#pragma once

// Project > Release readiness: edits Config/Release.swrelease, the release contract
// (docs/Release.md), and shows every release gate as `player --release-info` works it out.
// Same descriptor table, rules and gates as the command line; nothing is ticked by hand.

#include <Engine/ReleaseTier.h>
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <filesystem>
#include <string>

namespace editor {

class ReleaseWindow {
public:
    bool open = false;
    // Draws the window while it's open. Main thread, once a frame. `playing` disables saving.
    void draw(const shockwave::Project& project, bool playing);

private:
    void load(const shockwave::Project& project);
    void watchFile(const shockwave::Project& project);
    bool save(const shockwave::Project& project);
    void create(const shockwave::Project& project);
    void drawFields();
    void drawGates(const shockwave::ReleaseReport& report);
    void drawPlayerData(const shockwave::Project& project, const std::string& applicationId);
    // S10: the graphics release tier and the performance records the gates above judge.
    void drawTier(const shockwave::Project& project, bool playing);
    void drawPerformance();
    // Unresolved crash reports from the game's runs and an unclean last run (S12).
    void drawFatalErrors(const shockwave::Project& project);

    std::filesystem::path m_project;   // the project the contract belongs to
    bool m_present = false, m_savedPresent = false;
    shockwave::ReleaseSettings m_edit, m_saved;
    std::string m_loadError;   // the file exists but doesn't read: shown, never overwritten
    std::string m_saveError;
    std::filesystem::file_time_type m_fileTime{};
    double m_nextWatch = 0.0;
    shockwave::ReleaseInputs m_inputs;   // the project's other settings, re-read with the file
    shockwave::ReleaseTierReport m_tier;   // Config/ReleaseTier.swtier, re-read with the inputs
    std::string m_tierError;   // creating it failed
    std::string m_importResult;   // what the last Import development saves did
    bool m_importFailed = false;
    std::string m_resolveError;   // Mark resolved failed: why
};

// The project's other settings as the contract's shared rules see them: tick rate, package
// version, network and graphics settings, and this engine's build.
shockwave::ReleaseInputs releaseInputs(const shockwave::Project& project);
// Fills in the user_paths inputs: the installed per-user folders for `applicationId`.
void userPathsInputs(
    const shockwave::Project& project, const std::string& applicationId, shockwave::ReleaseInputs& inputs);

}   // namespace editor
