// The player's flags for shipping: installs, diagnostics, cooking, levels, saves and user folders: rows of the table in PlayerFlags.h.
// Each body is what the flag did in main()'s old else-if chain, messages unchanged.
#include "PlayerFlags.h"

#include <Core/Diagnostics.h>

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Engine/Engine.h>
#include <Engine/ReleaseTier.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/GameModuleLibrary.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>
#include <Renderer/GpuSelection.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace player {

void addReleaseFlags(std::vector<Flag>& flags) {
    add(flags, {"--app-shell"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.appShell = a.next();
        shockwave::AppShellScreens screens;
        std::string error;
        if (!shockwave::parseAppShellScreens(s.tool.appShell, screens, error))
            throw player::UsageError("--app-shell: " + error);
        return {};
    });

    add(flags, {"--verify-install", "--install-record", "--install-dir", "--update-from"}, 1,
        [](ParseState& s, FlagArgs& a) -> std::optional<int> {
            const std::string value = a.next();
            if (value.empty()) throw player::UsageError(a.arg + " takes a value");
            if (a.arg == "--verify-install")
                s.install.package = value;
            else if (a.arg == "--install-record")
                s.install.machine = value;
            else if (a.arg == "--update-from")
                s.install.updateFrom = value;
            else
                s.install.workFolder = value;
            return {};
        });

    add(flags, {"--keep-install", "--verbose"}, 0, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        (a.arg == "--keep-install" ? s.install.keep : s.install.verbose) = true;
        return {};
    });

    add(flags, {"--diagnostics", "--safe-mode", "--no-crash-handler", "--no-log-file", "--no-recovery-prompt"}, 0,
        [](ParseState& s, FlagArgs& a) -> std::optional<int> {
            (a.arg == "--diagnostics"        ? s.diagnostics.info
             : a.arg == "--safe-mode"        ? s.diagnostics.safeMode
             : a.arg == "--no-crash-handler" ? s.diagnostics.noCrashHandler
             : a.arg == "--no-log-file"      ? s.diagnostics.noLogFile
                                             : s.diagnostics.noRecoveryPrompt) = true;
            return {};
        });

    add(flags, {"--inject-device-lost"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.diagnostics.injectDeviceLost = int(wholeNumber(a.next(), 1000000, "--inject-device-lost"));
        if (s.diagnostics.injectDeviceLost == 0)
            throw player::UsageError("--inject-device-lost takes a frame number from 1");
        return {};
    });

    add(flags, {"--symbolize", "--symbols", "--log-file"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::filesystem::path value = a.next();
        if (a.arg == "--symbolize")
            s.diagnostics.symbolize = value;
        else if (a.arg == "--symbols")
            s.diagnostics.symbols.push_back(value);
        else
            s.diagnostics.logFile = value;
        return {};
    });

    add(flags, {"--release-tier"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.releaseTier = true;
        return {};
    });

    add(flags, {"--cook", "--release"}, 0, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        if (a.arg == "--cook")
            s.tool.mode = Tool::Mode::Cook;
        else
            s.tool.release = true;
        return {};
    });

    add(flags, {"--target", "--cook-info", "--verify"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        if (a.arg == "--target")
            s.tool.cookTarget = value;
        else if (a.arg == "--cook-info")
            s.cookInfoFile = value;
        else
            s.cookVerify = value;
        return {};
    });

    add(flags, {"--transition-timeout", "--lap-tolerance"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        char* end = nullptr;
        const double value = std::strtod(a.next(), &end);
        const bool ok = end && !*end && value >= 0 && value <= 1e6;
        if (a.arg == "--transition-timeout") {
            if (!ok) throw player::UsageError("--transition-timeout takes seconds (0 turns it off)");
            s.tool.transitionTimeout = value;
        } else {
            if (!ok) throw player::UsageError("--lap-tolerance takes MiB");
            s.tool.lapToleranceMib = value;
        }
        return {};
    });

    add(flags, {"--pref"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto equals = value.find('=');
        if (equals == std::string::npos || equals == 0 || !shockwave::findPreference(value.substr(0, equals)))
            throw player::UsageError("--pref takes key=value with a preference key, like master_volume=0.5 (player "
                                     "--preferences lists them)");
        s.tool.preferenceSets.emplace_back(value.substr(0, equals), value.substr(equals + 1));
        return {};
    });

    add(flags, {"--preferences", "--no-user-preferences"}, 0, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        if (a.arg == "--preferences")
            s.preferencesInfo = true;
        else if (a.arg == "--no-user-preferences")
            s.noUserPreferences = true;
        else
            s.preferencesSchema = a.next();
        return {};
    });

    add(flags, {"--preferences-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        if (a.arg == "--preferences")
            s.preferencesInfo = true;
        else if (a.arg == "--no-user-preferences")
            s.noUserPreferences = true;
        else
            s.preferencesSchema = a.next();
        return {};
    });

    add(flags, {"--flow-test"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        if (s.tool.perfRun) throw player::UsageError("--perf-run plays its own scenario: drop --flow-test");
        s.tool.flowTest = a.next();
        s.install.scenarios.push_back(s.tool.flowTest);   // --verify-install plays each in order
        return {};
    });

    add(flags, {"--perf-run", "--laps", "--perf-record"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        if (a.arg == "--perf-run") {
            if (!s.tool.flowTest.empty() && !s.tool.perfRun)
                throw player::UsageError("--perf-run plays its own scenario: drop --flow-test");
            s.tool.perfRun = true;
            s.tool.flowTest = value;
        } else if (a.arg == "--laps") {
            s.tool.perfLaps = static_cast<uint32_t>(wholeNumber(value, 100, "--laps"));
            if (s.tool.perfLaps == 0) throw player::UsageError("--laps takes a whole number from 1 to 100");
        } else {
            if (value.empty()) throw player::UsageError("--perf-record takes the machine's name");
            s.tool.perfRecord = value;
        }
        return {};
    });

    add(flags, {"--cold-cache"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.coldCache = true;
        s.clearPipelineCache = true;
        return {};
    });

    add(flags, {"--save-info", "--save-migrate"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        (a.arg == "--save-info" ? s.saveInfoFile : s.saveMigrateFile) = a.next();
        return {};
    });

    add(flags, {"--user-dir"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.userPathsCommand.userDir = a.next();

        return {};
    });

    add(flags, {"--profile"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.userPathsCommand.profile = a.next();

        return {};
    });

    add(flags, {"--user-paths"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.userPathsCommand.info = true;

        return {};
    });

    add(flags, {"--import-legacy-saves"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.userPathsCommand.importLegacy = a.next();

        return {};
    });

    add(flags, {"--preview"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.userPathsCommand.preview = true;

        return {};
    });

    add(flags, {"--release-info"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.releaseInfo = true;

        return {};
    });

    add(flags, {"--release-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.releaseSchema = a.next();

        return {};
    });

    add(flags, {"--levels"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::Levels;

        return {};
    });

    add(flags, {"--asset-dependencies"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::AssetDependencies;

        return {};
    });

    add(flags, {"--load-report"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.loadReport = a.next();

        return {};
    });

    add(flags, {"--no-asset-residency"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.assetResidency = false;

        return {};
    });

    add(flags, {"--instant-travel"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.instantTravel = true;

        return {};
    });

    add(flags, {"--asset-loading"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.assetLoading = a.next();
        if (s.tool.assetLoading != "all" && s.tool.assetLoading != "levels")
            throw player::UsageError("--asset-loading takes all or levels");

        return {};
    });

    add(flags, {"--asset-budget-cpu", "--asset-budget-gpu"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        double mib = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), mib);
        if (ec != std::errc{} || end != value.data() + value.size() || mib < 0)
            throw player::UsageError(a.arg + " takes a size in MiB (0 for no limit)");
        (a.arg == "--asset-budget-cpu" ? s.tool.assetBudget.cpuBytes : s.tool.assetBudget.gpuBytes) =
            uint64_t(mib * 1024.0 * 1024.0);

        return {};
    });

    add(flags, {"--level-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.levelSchema = a.next();

        return {};
    });

    add(flags, {"--level"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.level = a.next();

        return {};
    });

    add(flags, {"--saves"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.listSaves = true;

        return {};
    });

    add(flags, {"--load"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.load = a.next();

        return {};
    });

    add(flags, {"--save"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.save = a.next();
        if (!shockwave::validSaveSlot(s.tool.save))
            throw std::runtime_error("--save takes a slot name: letters, digits, spaces, - and _");

        return {};
    });
}

}   // namespace player
