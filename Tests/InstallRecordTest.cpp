// Install records and the platform gates (S13, docs/Release.md#platforms): .swinstall
// records round-trip and refuse what they don't know (or a "passed" a step contradicts);
// the gate passes only on a full record of the current optimised release build played on
// real hardware, fails on the newest failed one and says what's missing otherwise. Also a
// package's marker (readPackageInfo, its entry point) and the architecture packaging reads
// from each ELF file. No window, GPU or game module.
#include <Packaging/Package.h>
#include <Projects/InstallRecord.h>

#include "../Source/Runtime/Packaging/Private/BinaryInfo.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A record that qualifies its platform.
InstallRecord full() {
    InstallRecord r;
    r.machine = "Test desk";
    r.recorded = "2026-11-15T10:00:00Z";
    r.platform = "linux-x86_64";
    r.os = "Linux 6.8.0";
    r.distribution = "Ubuntu 24.04 LTS";
    r.glibc = "2.39";
    r.cpu = "A CPU";
    r.renderer = "vulkan";
    r.gpu = "A GPU";
    r.gpuKind = "dedicated";
    r.gpuDriver = "560.1";
    r.package = "Game-1.0.0-linux-x86_64";
    r.version = "1.0.0";
    r.buildConfig = "Release";
    r.buildId = "00000000000000aa";
    r.contentId = "00000000000000bb";
    r.packageSha256 = std::string(64, 'a');
    r.files = 10;
    r.bytes = 1000;
    r.previousPackage = "Game-0.9.0-linux-x86_64";
    r.previousVersion = "0.9.0";
    r.previousBuildId = "00000000000000a9";
    r.previousSha256 = std::string(64, 'c');
    r.readOnly = r.spacesInPath = r.unicodePath = r.otherFolder = r.cleanEnvironment = true;
    for (const char* name : {"install", "check", "scenario run.json", "user data", "update", "downgrade", "reinstall"})
        r.steps.push_back({name, InstallStepResult::Passed, 1.5, std::string("did ") + name});
    r.passed = true;
    return r;
}

void testRecords() {
    const InstallRecord r = full();
    const std::string text = installText(r);
    InstallRecord back;
    Diagnostic error;
    CHECK(parseInstall(text, "a.swinstall", back, error));
    CHECK(back == r);

    // The update's keys are optional: an older record without them still reads.
    {
        InstallRecord noUpdate = r;
        noUpdate.previousPackage = noUpdate.previousVersion = noUpdate.previousBuildId = noUpdate.previousSha256 = "";
        std::string older;   // as part 1 wrote them: no previous_ lines at all
        const std::string all = installText(noUpdate);
        for (size_t at = 0, end; at < all.size(); at = end + 1) {
            end = all.find('\n', at);
            const std::string line = all.substr(at, end - at + 1);
            if (line.rfind("previous_", 0) != 0) older += line;
        }
        CHECK(parseInstall(older, "a.swinstall", back, error) && back == noUpdate);
        CHECK(contains(text, "previous_version \"0.9.0\""));
    }

    // Strict: an unknown key, a missing one, a step that doesn't read.
    CHECK(
        !parseInstall(text + "colour blue\n", "a.swinstall", back, error) && error.code == "InstallUnreadable" &&
        contains(error.message, "unknown key colour"));
    std::string noMachine = text;
    const size_t at = noMachine.find("\nmachine ") + 1;
    noMachine.erase(at, noMachine.find('\n', at) - at + 1);
    CHECK(!parseInstall(noMachine, "a.swinstall", back, error) && contains(error.message, "missing machine"));
    CHECK(!parseInstall(text + "step \"x\" maybe 1 \"\"\n", "a.swinstall", back, error));
    CHECK(
        !parseInstall("shockwave_install 2\n", "a.swinstall", back, error) &&
        contains(error.message, "unsupported install record version 2"));
    // A hand edit can't promote a failed run.
    InstallRecord bad = full();
    bad.steps[1].result = InstallStepResult::Failed;
    bad.passed = false;
    std::string promoted = installText(bad);
    promoted.replace(promoted.find("result failed"), 13, "result passed");
    CHECK(!parseInstall(promoted, "a.swinstall", back, error) && contains(error.message, "a step failed"));
    // Values with newlines stay on one line.
    InstallRecord multi = full();
    multi.steps[0].detail = "two\nlines";
    CHECK(parseInstall(installText(multi), "a.swinstall", back, error) && back.steps[0].detail == "two lines");

    CHECK(installFileName("Yianni's desktop", "linux-x86_64") == "yiannis-desktop-linux-x86-64.swinstall");
    CHECK(installFileName("ü", "") == "machine.swinstall");

    // Written through a temporary file, read back the same.
    const fs::path file = fs::temp_directory_path() / "shockwave-install-record-test" / "a.swinstall";
    std::string why;
    CHECK(writeInstall(file, r, why));
    CHECK(readInstall(file, "a.swinstall", back, error) && back == r);
    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
}

void testGate() {
    const std::string build = "00000000000000aa";
    std::vector<InstallRecord> records;
    std::vector<std::string> files;
    auto gate = [&](std::string_view platform = "linux-x86_64") {
        return installGate(platform, records, files, build);
    };

    CHECK(gate().verdict == InstallVerdict::Unverified && contains(gate().reason, "no install record"));
    records.push_back(full());
    files.push_back("Release/Installs/a.swinstall");
    CHECK(installProblems(records[0], build).empty());
    CHECK(gate().verdict == InstallVerdict::Passed && gate().record == 0);
    // Another platform's record proves nothing here.
    CHECK(gate("windows-x86_64").verdict == InstallVerdict::Unverified);

    // What doesn't count, and says why.
    auto problem = [&](InstallRecord r, const std::string& expected) {
        records = {r};
        const InstallGate g = gate();
        if (g.verdict == InstallVerdict::Passed || !contains(g.reason, expected))
            std::fprintf(stderr, "expected \"%s\" in: %s\n", expected.c_str(), g.reason.c_str());
        CHECK(g.verdict != InstallVerdict::Passed && contains(g.reason, expected));
        return g;
    };
    InstallRecord r = full();
    r.renderer = "null";
    r.gpuKind = "none";
    CHECK(problem(r, "null renderer").verdict == InstallVerdict::Unverified);
    r = full();
    r.gpuKind = "software";
    r.gpu = "llvmpipe";
    problem(r, "software GPU");
    r = full();
    r.buildConfig = "Debug";
    problem(r, "Debug build");
    r = full();
    r.buildId.clear();
    problem(r, "development package");
    r = full();
    r.buildId = "00000000000000cc";
    CHECK(
        problem(r, "it's build 00000000000000cc; the game is build 00000000000000aa now").verdict ==
        InstallVerdict::Unverified);
    r = full();
    r.unicodePath = false;
    problem(r, "installed the hard way");
    r = full();
    r.steps[4].result = InstallStepResult::Skipped;   // update
    problem(r, "no \"update\" step");
    problem(r, "--update-from");
    r = full();
    r.steps[5].result = InstallStepResult::Skipped;   // downgrade
    problem(r, "no \"downgrade\" step");
    r = full();
    r.steps.erase(r.steps.begin() + 2);   // the scenario
    problem(r, "no scenario");
    r = full();
    r.steps[1] = {"check", InstallStepResult::Failed, 1, "exit code 1"};
    r.passed = false;
    CHECK(problem(r, "step \"check\" failed: exit code 1").verdict == InstallVerdict::Failed);

    // A failure counts only while it's the newest of the build; any full record passes.
    InstallRecord older = full();
    older.recorded = "2026-11-14T10:00:00Z";
    older.renderer = "null";
    older.gpuKind = "none";
    records = {older, r};
    files = {"a", "b"};
    CHECK(gate().verdict == InstallVerdict::Failed && gate().record == 1);
    records.push_back(full());
    files.push_back("c");
    CHECK(gate().verdict == InstallVerdict::Passed && gate().record == 2);
}

// What the scenarios proved (S16): kept in the record, read back strictly, and the gates
// that read them.
void testEvidence() {
    const std::string build = "00000000000000aa";
    InstallRecord r = full();
    r.proofs = {{"level_travel", "menu -> a -> b -> menu (run.json)"}, {"saves", "continued in a"}};
    InstallRecord back;
    Diagnostic error;
    const std::string text = installText(r);
    CHECK(contains(text, "proves level_travel \"menu -> a -> b -> menu (run.json)\"\nproves saves"));
    CHECK(parseInstall(text, "a.swinstall", back, error) && back == r && back.proof("saves"));
    CHECK(!back.proof("player_settings"));
    auto withLine = [&](const std::string& line) {
        std::string t = text;
        t.insert(t.find("result "), line + "\n");
        return t;
    };
    CHECK(!parseInstall(withLine("proves saves \"again\""), "a.swinstall", back, error) &&
          contains(error.message, "proved twice"));
    CHECK(!parseInstall(withLine("proves performance \"fast\""), "a.swinstall", back, error) &&
          contains(error.message, "no install record proves the gate performance"));
    CHECK(!parseInstall(withLine("proves acceptance_scenario \"x\""), "a.swinstall", back, error));
    CHECK(!parseInstall(withLine("proves saves"), "a.swinstall", back, error));
    CHECK(evidenceGates().size() == 5);

    std::vector<InstallRecord> records;
    std::vector<std::string> files;
    const std::vector<std::string> platforms = {"linux-x86_64"};
    auto gate = [&](std::string_view id, std::span<const std::string> on) {
        return evidenceGate(id, "Tests/flow/run.json", on, records, files, build);
    };
    CHECK(gate("level_travel", platforms).verdict == InstallVerdict::Unverified &&
          contains(gate("level_travel", platforms).reason, "through two levels"));
    // A null-renderer run proves the logic: these gates don't need a GPU.
    r.renderer = "null";
    r.gpuKind = "none";
    records.push_back(r);
    files.push_back("Release/Installs/a.swinstall");
    const InstallGate travel = gate("level_travel", platforms);
    CHECK(travel.verdict == InstallVerdict::Passed && travel.record == 0 && contains(travel.reason, "null renderer") &&
          contains(travel.reason, "menu -> a -> b -> menu"));
    CHECK(gate("saves", platforms).verdict == InstallVerdict::Passed);
    CHECK(gate("player_settings", platforms).verdict == InstallVerdict::Unverified &&
          contains(gate("player_settings", platforms).reason, "none of its scenarios proves it"));
    // The acceptance scenario: a passed step that played the contract's file.
    CHECK(gate("acceptance_scenario", platforms).verdict == InstallVerdict::Passed);
    CHECK(evidenceGate("acceptance_scenario", "Tests/flow/other.json", platforms, records, files, build).verdict ==
          InstallVerdict::Unverified);
    // Another platform, another build, a development package, a debug build, a failed run: none count.
    CHECK(gate("level_travel", std::vector<std::string>{"windows-x86_64"}).verdict == InstallVerdict::Unverified);
    CHECK(gate("level_travel", {}).verdict == InstallVerdict::Passed);
    auto spoiled = [&](auto change, const std::string& why) {
        InstallRecord bad = r;
        change(bad);
        records = {bad};
        const InstallGate g = gate("level_travel", platforms);
        CHECK(g.verdict == InstallVerdict::Unverified && contains(g.reason, why));
    };
    spoiled([](InstallRecord& x) { x.buildId = "00000000000000ff"; }, "not the current build");
    spoiled([](InstallRecord& x) { x.buildId.clear(); }, "development package");
    spoiled([](InstallRecord& x) { x.buildConfig = "Debug"; }, "a Debug build");
    spoiled(
        [](InstallRecord& x) {
            x.steps[1].result = InstallStepResult::Failed;
            x.passed = false;
        },
        "a step failed");
}

void testPackageInfo() {
    const fs::path package = fs::temp_directory_path() / "shockwave-install-package-test" / "Game-1.0.0-linux-x86_64";
    std::error_code ec;
    fs::remove_all(package.parent_path(), ec);
    fs::create_directories(package / "Binaries", ec);
    PackageInfo info;
    std::string error;
    CHECK(!readPackageInfo(package, info, error) && contains(error, "not a Shockwave package"));
    std::ofstream(package / "Binaries" / "shockwave-package.txt")
        << "# a comment\nshockwave_package_info 1\nproject \"../Game/Game.swproject\"\nname \"Space Ace\"\n"
           "executable \"SpaceAce\"\nversion \"1.2.0\"\nplatform \"linux-x86_64\"\nbuild_type \"Release\"\n"
           "release true\nbuild_id 00000000000000aa\nsomething_newer 1\n";
    CHECK(readPackageInfo(package, info, error));
    CHECK(
        info.name == "Space Ace" && info.executable == "SpaceAce" && info.version == "1.2.0" && info.release &&
        info.buildId == "00000000000000aa" && info.buildType == "Release");
    CHECK(packageEntryPoint(package, info) == package / "SpaceAce");
    info.platform = "windows-x86_64";
    CHECK(packageEntryPoint(package, info) == package / "Binaries" / "SpaceAce.exe");
    // A marker can't point anywhere else.
    for (const char* sneaky : {"../../bin/sh", "/bin/sh", "a\\b", "C:x", "..", ""}) {
        info.executable = sneaky;
        CHECK(packageEntryPoint(package, info).empty());
    }
    std::ofstream(package / "Binaries" / "shockwave-package.txt") << "shockwave_package_info 9\n";
    CHECK(!readPackageInfo(package, info, error));
    fs::remove_all(package.parent_path(), ec);
}

void testArchitecture() {
    // Only the ELF header: packaging refuses a file built for another machine.
    const fs::path dir = fs::temp_directory_path() / "shockwave-install-elf-test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto elf = [&](const char* name, char elfClass, unsigned short machine) {
        std::string header(64, '\0');
        header[0] = 0x7f;
        header[1] = 'E';
        header[2] = 'L';
        header[3] = 'F';
        header[4] = elfClass;
        header[5] = 1;   // little-endian
        header[0x12] = char(machine & 0xff);
        header[0x13] = char(machine >> 8);
        std::ofstream(dir / name, std::ios::binary) << header;
        return packaging::readBinaryInfo(dir / name);
    };
    const packaging::BinaryInfo arm = elf("arm", 2, 183);
    CHECK(arm.elf && arm.elfClass == 2 && arm.machine == 183);
    const packaging::BinaryInfo x86 = elf("x86", 1, 3);
    CHECK(x86.elf && x86.elfClass == 1 && x86.machine == 3 && !x86.binary);
    CHECK(packaging::elfMachineName(183) == "AArch64" && packaging::elfMachineName(62) == "x86-64");
    std::ofstream(dir / "text") << "not a program";
    CHECK(!packaging::readBinaryInfo(dir / "text").elf);
    fs::remove_all(dir, ec);
}

}   // namespace

int main() {
    testRecords();
    testGate();
    testEvidence();
    testPackageInfo();
    testArchitecture();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("install records: all checks passed\n");
    return 0;
}
