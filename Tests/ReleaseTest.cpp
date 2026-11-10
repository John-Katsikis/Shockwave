// The release contract (S00 of docs/ShippingImplementation.md, docs/Release.md):
// Config/Release.swrelease parsing and writing, every range, NaN and overflow, duplicate
// keys and list entries, missing keys, an unknown version, the rules between keys and with
// the project's other settings, the gates worked out from them, a project without a
// contract, and the JSON report. No game module, window or GPU.
#include <Projects/Project.h>
#include <Projects/ReleaseSettings.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace shockwave;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

const char* kGood = "shockwave_release 1\n"
                    "# a comment\n"
                    "application_id \"com.example.game\"\n"
                    "release_version \"1.2.0\"\n"
                    "platform linux-x86_64\n"
                    "platform windows-x86_64\n"
                    "mode offline\n"
                    "mode online   # with a comment\n"
                    "max_players 12\n"
                    "target_fps 60\n"
                    "simulation_hz 60\n"
                    "graphics_tier high\n"
                    "cpu_budget_mib 8192\n"
                    "gpu_budget_mib 4096\n"
                    "shared_budget_mib 0\n"
                    "loading_budget_mib 1024\n"
                    "transition_timeout_seconds 45.5\n"
                    "acceptance_scenario \"Tests/complete_game.json\"\n";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at == std::string::npos) {
        std::fprintf(stderr, "test bug: '%s' isn't in the contract\n", from.c_str());
        ++failures;
        return text;
    }
    return text.replace(at, from.size(), to);
}

bool parses(const std::string& text, Diagnostic* error = nullptr) {
    ReleaseFile file;
    Diagnostic problem;
    const bool ok = parseRelease(text, "Config/Release.swrelease", file, problem);
    if (error) *error = problem;
    return ok;
}

// The code of the first error from parsing text, or "" when it parses.
std::string parseCode(const std::string& text) {
    Diagnostic error;
    return parses(text, &error) ? std::string() : error.code;
}

ReleaseSettings good() {
    ReleaseFile file;
    Diagnostic error;
    if (!parseRelease(kGood, "Release.swrelease", file, error)) {
        std::fprintf(stderr, "the good contract doesn't parse: %s\n", error.text().c_str());
        ++failures;
    }
    return file.settings;
}

std::vector<Diagnostic> validate(const ReleaseSettings& settings, const ReleaseInputs* inputs = nullptr) {
    std::vector<Diagnostic> diagnostics;
    validateRelease(settings, inputs, "Config/Release.swrelease", nullptr, diagnostics);
    return diagnostics;
}

bool has(const std::vector<Diagnostic>& diagnostics, const std::string& code, bool error = true) {
    for (const Diagnostic& d : diagnostics)
        if (d.code == code && d.error() == error) return true;
    return false;
}

// Inputs that agree with the good contract: an online game over Steam with its own AppID.
ReleaseInputs agreeing() {
    ReleaseInputs in;
    in.projectTickRate = 60;
    in.packageFile = "Config/Package.swpackage";
    in.packageVersion = "1.2.0";
    in.networkPresent = true;
    in.networkEnabled = true;
    in.networkSteam = true;
    in.networkMaxPlayers = 12;
    in.steamAppId = 123456;
    in.graphicsMemoryBudgetMb = 512;
    in.acceptanceScenarioExists = true;
    in.buildConfig = "Release";
    return in;
}

const ReleaseGate* findGate(const ReleaseReport& report, const std::string& id) {
    for (const ReleaseGate& gate : report.gates)
        if (gate.id == id) return &gate;
    return nullptr;
}

ReleaseReport reportFor(const ReleaseSettings& settings, const ReleaseInputs& inputs) {
    ReleaseReport report;
    report.present = report.readable = true;
    report.file = "Config/Release.swrelease";
    report.settings = settings;
    validateRelease(settings, &inputs, report.file, nullptr, report.diagnostics);
    releaseGates(report, inputs);
    return report;
}

void testRoundTrip() {
    ReleaseFile file;
    Diagnostic error;
    CHECK(parseRelease(kGood, "Release.swrelease", file, error));
    const ReleaseSettings& s = file.settings;
    CHECK(s.applicationId == "com.example.game");
    CHECK(s.releaseVersion == "1.2.0");
    CHECK((s.platforms == std::vector<std::string>{"linux-x86_64", "windows-x86_64"}));
    CHECK(s.hasMode(ReleaseMode::Offline) && s.hasMode(ReleaseMode::Online));
    CHECK(s.maxPlayers == 12 && s.targetFps == 60 && s.simulationHz == 60);
    CHECK(s.graphicsTier == ReleaseGraphicsTier::High);
    CHECK(s.cpuBudgetMib == 8192 && s.gpuBudgetMib == 4096 && s.sharedBudgetMib == 0);
    CHECK(s.loadingBudgetMib == 1024 && s.transitionTimeoutSeconds == 45.5);
    CHECK(s.acceptanceScenario == "Tests/complete_game.json");
    CHECK(file.line("application_id") == 3 && file.line("platform") == 5 && file.line("mode") == 7);
    CHECK(file.line("acceptance_scenario") == 18);
    // Written and read back: exactly the same, and the same text again.
    const std::string text = releaseText(s);
    ReleaseFile again;
    CHECK(parseRelease(text, "Release.swrelease", again, error));
    CHECK(again.settings == s);
    CHECK(releaseText(again.settings) == text);
    // CRLF line endings read the same.
    std::string crlf;
    for (const char c : std::string(kGood)) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    CHECK(parses(crlf));
    CHECK(validate(s).empty());

    // Through a file, as the editor saves it.
    const fs::path dir = fs::temp_directory_path() / "shockwave_release_test";
    fs::remove_all(dir);
    std::string why;
    CHECK(writeRelease(dir / "Config" / "Release.swrelease", s, why));
    ReleaseFile read;
    CHECK(readRelease(dir / "Config" / "Release.swrelease", "Config/Release.swrelease", read, error));
    CHECK(read.settings == s);
    // A contract with errors isn't written, and leaves the old file alone.
    ReleaseSettings bad = s;
    bad.maxPlayers = 3;
    CHECK(
        !writeRelease(dir / "Config" / "Release.swrelease", bad, why) && why.find("max_players") != std::string::npos);
    CHECK(readRelease(dir / "Config" / "Release.swrelease", "Config/Release.swrelease", read, error));
    CHECK(read.settings == s);
    CHECK(
        !readRelease(dir / "missing.swrelease", "missing.swrelease", read, error) && error.code == "ReleaseUnreadable");
    fs::remove_all(dir);
}

void testSyntax() {
    CHECK(parseCode("") == "ReleaseHeader");
    CHECK(parseCode(replaced(kGood, "shockwave_release 1", "shockwave_release")) == "ReleaseHeader");
    CHECK(parseCode(replaced(kGood, "shockwave_release 1", "shockwave_release 1 extra")) == "ReleaseHeader");
    // A newer format fails clearly, naming the version it found.
    Diagnostic error;
    CHECK(!parses(replaced(kGood, "shockwave_release 1", "shockwave_release 3"), &error));
    CHECK(
        error.code == "ReleaseUnsupportedVersion" && error.line == 1 &&
        error.message.find("version 3") != std::string::npos);
    CHECK(parseCode(replaced(kGood, "target_fps 60", "target_fps 60\nframe_rate 60")) == "ReleaseUnknownKey");
    // A repeated key, with both lines named.
    CHECK(!parses(replaced(kGood, "target_fps 60", "target_fps 60\ntarget_fps 90"), &error));
    CHECK(
        error.code == "ReleaseDuplicateKey" && error.line == 11 && error.object == "target_fps" &&
        error.message.find("line 10") != std::string::npos);
    // platform and mode repeat, but never with the same value.
    CHECK(!parses(replaced(kGood, "platform windows-x86_64", "platform linux-x86_64"), &error));
    CHECK(error.code == "ReleaseInvalidValue" && error.line == 6 && error.message.find("twice") != std::string::npos);
    CHECK(parseCode(replaced(kGood, "mode online", "mode offline")) == "ReleaseInvalidValue");
    CHECK(parseCode(replaced(kGood, "platform windows-x86_64", "platform windows")) == "ReleaseInvalidValue");
    CHECK(parseCode(replaced(kGood, "mode online", "mode coop")) == "ReleaseInvalidValue");
    // Every key is required; the error names the one left out.
    CHECK(!parses(replaced(kGood, "shared_budget_mib 0\n", ""), &error));
    CHECK(error.code == "ReleaseMissingKey" && error.object == "shared_budget_mib" && error.line == 0);
    CHECK(
        parseCode(replaced(replaced(kGood, "platform linux-x86_64\n", ""), "platform windows-x86_64\n", "")) ==
        "ReleaseMissingKey");
    // Quoted keys must be quoted; others take one value.
    CHECK(parseCode(replaced(kGood, "\"com.example.game\"", "com.example.game")) == "ReleaseInvalidValue");
    CHECK(parseCode(replaced(kGood, "target_fps 60", "target_fps 60 120")) == "ReleaseInvalidValue");
    CHECK(parseCode(replaced(kGood, "target_fps 60", "target_fps")) == "ReleaseInvalidValue");
    // Plain output: file:line: message.
    CHECK(!parses(replaced(kGood, "target_fps 60", "target_fps 20"), &error));
    CHECK(error.text().rfind("Config/Release.swrelease:10: target_fps must be a whole number from 30 to 360", 0) == 0);
}

void testRanges() {
    auto bad = [](const std::string& from, const std::string& to) {
        return parseCode(replaced(kGood, from, to)) == "ReleaseInvalidValue";
    };
    auto fine = [](const std::string& from, const std::string& to) { return parses(replaced(kGood, from, to)); };
    // application_id: reverse-domain, lowercase, stable.
    CHECK(fine("com.example.game", "io.studio-name.my_game2"));
    CHECK(bad("com.example.game", "game"));
    CHECK(bad("com.example.game", "Com.Example.Game"));
    CHECK(bad("com.example.game", "com..game"));
    CHECK(bad("com.example.game", "com.example."));
    CHECK(bad("com.example.game", "com.2example"));
    CHECK(bad("com.example.game", "com.example game"));
    CHECK(bad("com.example.game", "com.exämple.game"));
    CHECK(bad("com.example.game", ""));
    CHECK(bad("com.example.game", "com." + std::string(130, 'a')));
    // Versions.
    CHECK(fine("\"1.2.0\"", "\"2\""));
    CHECK(fine("\"1.2.0\"", "\"1.2.3.4-rc.1\""));
    CHECK(bad("\"1.2.0\"", "\"1.2.3.4.5\""));
    CHECK(bad("\"1.2.0\"", "\"v1\""));
    CHECK(bad("\"1.2.0\"", "\"1..2\""));
    CHECK(bad("\"1.2.0\"", "\"1.0-\""));
    CHECK(bad("\"1.2.0\"", "\"1.0 beta\""));
    // Whole numbers: in range, no signs, fractions, exponents or overflow.
    CHECK(fine("max_players 12", "max_players 64"));
    CHECK(bad("max_players 12", "max_players 0"));
    CHECK(bad("max_players 12", "max_players 66"));
    CHECK(bad("max_players 12", "max_players -2"));
    CHECK(bad("max_players 12", "max_players 12.0"));
    CHECK(bad("max_players 12", "max_players 1e1"));
    CHECK(bad("cpu_budget_mib 8192", "cpu_budget_mib 99999999999999999999999"));
    CHECK(bad("cpu_budget_mib 8192", "cpu_budget_mib 4294967296"));
    CHECK(bad("cpu_budget_mib 8192", "cpu_budget_mib 32"));
    CHECK(bad("gpu_budget_mib 4096", "gpu_budget_mib 2000000"));
    CHECK(bad("target_fps 60", "target_fps 361"));
    CHECK(bad("simulation_hz 60", "simulation_hz 0"));
    CHECK(bad("loading_budget_mib 1024", "loading_budget_mib 8"));
    CHECK(fine("shared_budget_mib 0", "shared_budget_mib 16384"));
    // Seconds: finite, in range.
    CHECK(bad("transition_timeout_seconds 45.5", "transition_timeout_seconds nan"));
    CHECK(bad("transition_timeout_seconds 45.5", "transition_timeout_seconds inf"));
    CHECK(bad("transition_timeout_seconds 45.5", "transition_timeout_seconds 0"));
    CHECK(bad("transition_timeout_seconds 45.5", "transition_timeout_seconds 601"));
    CHECK(bad("transition_timeout_seconds 45.5", "transition_timeout_seconds 1e999"));
    CHECK(bad("graphics_tier high", "graphics_tier epic"));
    // The scenario: a .json inside the project.
    CHECK(bad("Tests/complete_game.json", "../outside.json"));
    CHECK(bad("Tests/complete_game.json", "/abs/complete_game.json"));
    CHECK(bad("Tests/complete_game.json", "C:/complete_game.json"));
    CHECK(bad("Tests/complete_game.json", "Tests\\\\complete_game.json"));
    CHECK(bad("Tests/complete_game.json", "Tests/complete_game.txt"));
    CHECK(bad("Tests/complete_game.json", ""));
}

void testRules() {
    // Offline only: one player.
    ReleaseSettings s = good();
    s.modes = {ReleaseMode::Offline};
    CHECK(has(validate(s), "ReleasePlayerCount"));
    s.maxPlayers = 1;
    CHECK(validate(s).empty());
    // Online: an even count from 2.
    s = good();
    for (const uint32_t players : {1u, 3u, 13u}) {
        s.maxPlayers = players;
        CHECK(has(validate(s), "ReleasePlayerCount"));
    }
    s.maxPlayers = 2;
    CHECK(validate(s).empty());
    // Budgets.
    s = good();
    s.loadingBudgetMib = s.cpuBudgetMib + 1;
    CHECK(has(validate(s), "ReleaseLoadingBudget"));
    s = good();
    s.sharedBudgetMib = 8192;   // less than 8192 + 4096
    CHECK(has(validate(s), "ReleaseMemoryBudget"));
    s.sharedBudgetMib = 12288;
    CHECK(validate(s).empty());
    // Settings made in the editor are checked the same way: empty lists, empty ID.
    s = good();
    s.platforms.clear();
    CHECK(has(validate(s), "ReleaseMissingKey"));
    s = good();
    s.applicationId.clear();
    CHECK(has(validate(s), "ReleaseInvalidValue"));
    s = good();
    s.platforms.push_back("linux-x86_64");
    CHECK(has(validate(s), "ReleaseInvalidValue"));
    s = good();
    s.transitionTimeoutSeconds = std::numeric_limits<double>::quiet_NaN();
    CHECK(has(validate(s), "ReleaseInvalidValue"));
}

void testSharedRules() {
    const ReleaseSettings s = good();
    ReleaseInputs in = agreeing();
    CHECK(validate(s, &in).empty());

    in.projectTickRate = 120;
    CHECK(has(validate(s, &in), "ReleaseTickRate"));
    in = agreeing();
    in.packageVersion = "1.0.0";
    CHECK(has(validate(s, &in), "ReleaseVersionMismatch"));
    in = agreeing();
    in.packageVersion.clear();   // the package file doesn't read: --check reports that instead
    CHECK(validate(s, &in).empty());

    // Online needs multiplayer on, with the same player count.
    in = agreeing();
    in.networkMaxPlayers = 16;
    const auto mismatch = validate(s, &in);
    CHECK(has(mismatch, "ReleaseNetworkPlayers"));
    CHECK(mismatch.size() == 1 && mismatch[0].object == "max_players");
    in = agreeing();
    in.networkPresent = false;
    CHECK(has(validate(s, &in), "ReleaseNetworkMissing"));
    in = agreeing();
    in.networkEnabled = false;
    CHECK(has(validate(s, &in), "ReleaseNetworkMissing"));
    in = agreeing();
    in.networkError = "Config/Network.swnetwork doesn't read";
    CHECK(has(validate(s, &in), "ReleaseNetworkInvalid"));
    // Valve's test AppID: a warning for --check, a failed gate for qualification.
    in = agreeing();
    in.steamAppId = 480;
    CHECK(has(validate(s, &in), "ReleaseDevelopmentAppId", false) && !hasErrors(validate(s, &in)));
    // Offline only, but the network settings turn multiplayer on.
    ReleaseSettings offline = s;
    offline.modes = {ReleaseMode::Offline};
    offline.maxPlayers = 1;
    in = agreeing();
    CHECK(has(validate(offline, &in), "ReleaseNetworkUnexpected"));
    in.networkEnabled = false;
    CHECK(validate(offline, &in).empty());
    // The graphics memory budget must fit in the GPU budget.
    in = agreeing();
    in.graphicsMemoryBudgetMb = 8192;
    CHECK(has(validate(s, &in), "ReleaseGraphicsBudget"));
    in = agreeing();
    in.acceptanceScenarioExists = false;
    CHECK(has(validate(s, &in), "ReleaseScenarioMissing", false) && !hasErrors(validate(s, &in)));
}

void testGates() {
    ReleaseInputs in = agreeing();
    ReleaseReport report = reportFor(good(), in);
    CHECK(findGate(report, "release_contract")->status == GateStatus::Passed);
    CHECK(findGate(report, "project_settings")->status == GateStatus::Passed);
    CHECK(findGate(report, "release_build")->status == GateStatus::Passed);
    CHECK(findGate(report, "steam_app_id")->status == GateStatus::Passed);
    // What later batches measure stays unverified, so nothing qualifies yet.
    for (const char* id :
         {"user_paths", "level_catalog", "level_travel", "acceptance_scenario", "saves", "player_settings",
          "cooked_package", "performance", "release_tier", "online_lifecycle", "crash_recovery",
          "platform:linux-x86_64", "platform:windows-x86_64"}) {
        const ReleaseGate* gate = findGate(report, id);
        CHECK(gate && gate->status == GateStatus::Unverified && !gate->reason.empty() && !gate->batch.empty());
    }
    CHECK(!findGate(report, "platform:macos-arm64"));
    CHECK(!report.qualified());
    // In batch order.
    CHECK(report.gates[4].id == "user_paths" && report.gates[5].id == "level_catalog");

    // user_paths (S01) is measured once the caller checks the per-user folders.
    {
        ReleaseInputs paths = agreeing();
        paths.userPathsChecked = true;
        paths.userPathsApplicationId = good().applicationId;
        paths.userPathsRoot = "/home/player/.local/share/" + good().applicationId;
        ReleaseReport measured = reportFor(good(), paths);
        CHECK(findGate(measured, "user_paths")->status == GateStatus::Passed);
        CHECK(findGate(measured, "user_paths")->reason.find(paths.userPathsRoot) != std::string::npos);
        paths.userPathsApplicationId = "shockwave.other";
        measured = reportFor(good(), paths);
        CHECK(findGate(measured, "user_paths")->status == GateStatus::Failed);
        CHECK(findGate(measured, "user_paths")->reason.find("shockwave.other") != std::string::npos);
        paths.userPathsApplicationId = good().applicationId;
        paths.userPathsProblem = "there's no per-user data folder (HOME isn't set)";
        measured = reportFor(good(), paths);
        CHECK(findGate(measured, "user_paths")->status == GateStatus::Failed);
        CHECK(findGate(measured, "user_paths")->reason == paths.userPathsProblem);
    }

    // A shared rule fails project_settings, not the contract.
    in.packageVersion = "9.9";
    report = reportFor(good(), in);
    CHECK(findGate(report, "release_contract")->status == GateStatus::Passed);
    CHECK(findGate(report, "project_settings")->status == GateStatus::Failed);
    CHECK(findGate(report, "project_settings")->reason.find("9.9") != std::string::npos);
    // And a contract rule fails the contract.
    ReleaseSettings s = good();
    s.maxPlayers = 13;
    in = agreeing();
    in.networkMaxPlayers = 13;
    report = reportFor(s, in);
    CHECK(findGate(report, "release_contract")->status == GateStatus::Failed);

    // Builds: optimised passes, Debug fails, unknown is unverified.
    in = agreeing();
    in.buildConfig = "Debug";
    CHECK(findGate(reportFor(good(), in), "release_build")->status == GateStatus::Failed);
    in.buildConfig = "";
    CHECK(findGate(reportFor(good(), in), "release_build")->status == GateStatus::Unverified);
    in.buildConfig = "RelWithDebInfo";
    CHECK(findGate(reportFor(good(), in), "release_build")->status == GateStatus::Passed);

    // Valve's test AppID can't qualify.
    in = agreeing();
    in.steamAppId = 480;
    CHECK(findGate(reportFor(good(), in), "steam_app_id")->status == GateStatus::Failed);
    // Offline: online gates don't apply.
    s = good();
    s.modes = {ReleaseMode::Offline};
    s.maxPlayers = 1;
    in = agreeing();
    in.networkEnabled = false;
    report = reportFor(s, in);
    CHECK(findGate(report, "steam_app_id")->status == GateStatus::NotApplicable);
    CHECK(findGate(report, "online_lifecycle")->status == GateStatus::NotApplicable);
    CHECK(findGate(report, "project_settings")->status == GateStatus::Passed);
}

void testProjects() {
    const fs::path dir = fs::temp_directory_path() / "shockwave_release_project";
    fs::remove_all(dir);
    fs::create_directories(dir / "Scenes");
    std::ofstream(dir / "Scenes" / "main.swscene") << "";
    Project project;
    std::string error;
    CHECK(createProject(dir, "Release Test", "sample", project, error));
    // Without a contract the per-user folders are named after the game module, never the
    // display name; with one, after its application_id.
    CHECK(fallbackApplicationId(project) == "shockwave.sample");
    CHECK(projectApplicationId(project) == "shockwave.sample");
    project.name = "Renamed Game";
    CHECK(projectApplicationId(project) == "shockwave.sample");
    Project odd = project;
    odd.gameModule = "My Game!";
    CHECK(fallbackApplicationId(odd) == "shockwave.mygame");
    odd.gameModule = "!!";
    CHECK(fallbackApplicationId(odd) == "shockwave.game");
    // A legacy project without a contract: reported as such, never an error.
    ReleaseReport report = releaseReport(project, agreeing());
    CHECK(!report.present && !report.readable && report.diagnostics.empty());
    CHECK(report.gates.size() == 1 && report.gates[0].status == GateStatus::Unverified);
    CHECK(!report.qualified());
    CHECK(releaseReportText(report).find("release none") == 0);
    // A contract on disk, checked against the project; the scenario's file is looked for.
    fs::create_directories(releasePath(project).parent_path());
    std::ofstream(releasePath(project)) << kGood;
    CHECK(projectApplicationId(project) == "com.example.game");
    ReleaseInputs in = agreeing();
    in.acceptanceScenarioExists = true;   // replaced by what's on disk
    report = releaseReport(project, in);
    CHECK(report.present && report.readable);
    CHECK(has(report.diagnostics, "ReleaseScenarioMissing", false));
    const Diagnostic* scenario = nullptr;
    for (const Diagnostic& d : report.diagnostics)
        if (d.code == "ReleaseScenarioMissing") scenario = &d;
    CHECK(scenario && scenario->line == 18 && scenario->file == "Config/Release.swrelease");
    fs::create_directories(dir / "Tests");
    std::ofstream(dir / "Tests" / "complete_game.json") << "{}";
    report = releaseReport(project, in);
    CHECK(report.diagnostics.empty());
    // The JSON report: format 1, every gate, and no ordinary text mixed in.
    const std::string json = releaseReportJson(report, "Release \"Test\"");
    CHECK(json.rfind("{\n  \"format\": 1,", 0) == 0);
    CHECK(json.find("\"project\": \"Release \\\"Test\\\"\"") != std::string::npos);
    CHECK(json.find("\"platform\": [\"linux-x86_64\", \"windows-x86_64\"]") != std::string::npos);
    CHECK(json.find("\"transition_timeout_seconds\": 45.5") != std::string::npos);
    CHECK(json.find("\"id\": \"platform:windows-x86_64\", \"status\": \"unverified\"") != std::string::npos);
    CHECK(json.find("\"qualified\": false") != std::string::npos);
    CHECK(json.back() == '\n' && json[json.size() - 2] == '}');
    // A broken contract: one diagnostic with file and line, and a failed contract gate.
    std::ofstream(releasePath(project)) << replaced(kGood, "max_players 12", "max_players many");
    report = releaseReport(project, in);
    CHECK(report.present && !report.readable && report.diagnostics.size() == 1);
    if (!report.diagnostics.empty()) {
        CHECK(report.diagnostics[0].line == 9 && report.diagnostics[0].code == "ReleaseInvalidValue");
        CHECK(diagnosticJson(report.diagnostics[0]).find("\"line\": 9") != std::string::npos);
    }
    CHECK(report.gates.size() == 1 && report.gates[0].status == GateStatus::Failed);
    fs::remove_all(dir);
}

void testSchema() {
    const std::string schema = releaseSchema();
    for (const ReleaseField& field : releaseFields())
        CHECK(schema.find(std::string("| `") + field.key + "` |") != std::string::npos);
    for (const char* code : {"ReleaseMissingKey", "ReleaseNetworkPlayers", "ReleaseDevelopmentAppId"})
        CHECK(schema.find(code) != std::string::npos);
    CHECK(schema == releaseSchema());   // stable
    CHECK(diagnosticJsonString("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
}

}   // namespace

int main() {
    testRoundTrip();
    testSyntax();
    testRanges();
    testRules();
    testSharedRules();
    testGates();
    testProjects();
    testSchema();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("release contract: all checks passed\n");
    return 0;
}
