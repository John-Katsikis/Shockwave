#pragma once

// Build > Package game: edits Config/Package.swpackage and makes a game folder for
// players in the background, with the same packageProject() as `player --package`.

#include <Packaging/Package.h>
#include <Projects/Project.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace editor {

class PackageWindow {
public:
    ~PackageWindow();

    bool open = false;
    // Draws the window while it's open. Main thread, once a frame.
    void draw(const shockwave::Project& project);
    bool running() const { return m_running; }

private:
    struct Line {
        shockwave::LogLevel level;
        std::string text;
    };

    void load(const shockwave::Project& project);
    void watchSettingsFile(const shockwave::Project& project);
    bool save(const shockwave::Project& project);
    void start(const shockwave::Project& project);
    void drawSettings(const shockwave::Project& project);
    void drawProgress();
    void drawResult();
    void drawLog();

    // Settings: an edited copy, saved to the file when packaging starts or on Save.
    std::filesystem::path m_project;   // the project the settings belong to
    shockwave::PackageSettings m_edit, m_saved;
    bool m_development = false;   // this run only, never saved: a test build with Steam AppID 480
    std::string m_excludeText, m_outputText;
    std::string m_settingsError;
    std::filesystem::file_time_type m_settingsTime{};
    double m_nextWatch = 0.0;
    std::mutex m_browseMutex;
    std::optional<std::string> m_browsed;   // a folder picked in the dialog, from its thread

    // The running package, written by its thread and read here under m_mutex.
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_cancel{false};
    std::mutex m_mutex;
    std::vector<Line> m_lines;
    bool m_scrollToEnd = false;
    shockwave::PackageStep m_step = shockwave::PackageStep::Done;
    float m_fraction = 0.0f;
    std::optional<shockwave::PackageResult> m_result;
    shockwave::Project m_jobProject;
    shockwave::PackageSettings m_jobSettings;
    shockwave::PackageSteam m_jobSteam;
};

}   // namespace editor
