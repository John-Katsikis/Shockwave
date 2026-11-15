// Release rehearsals (S15, docs/ReleaseRehearsal.md): .swrehearsal records round-trip and
// refuse what they don't know, and a "passed" that an intervention contradicts. The
// release_rehearsal gate passes on the newest record of this version, platform and build
// when it passed, fails when it didn't, and stays unverified without one. No window, GPU
// or game module.
#include <Projects/Project.h>
#include <Projects/Rehearsal.h>
#include <Projects/ReleaseSettings.h>

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

RehearsalRecord passed() {
    RehearsalRecord r;
    r.tester = "Tester B";
    r.recorded = "2026-11-20T15:00:00Z";
    r.machine = "Borrowed laptop, new user";
    r.platform = "linux-x86_64";
    r.package = "TwoRooms-1.0.0-linux-x86_64";
    r.version = "1.0.0";
    r.buildId = "eb9dfd1a69453e97";
    r.notes = {"found Continue without help"};
    r.passed = true;
    return r;
}

bool parses(const std::string& text, RehearsalRecord& out, Diagnostic& error) {
    return parseRehearsal(text, "R.swrehearsal", out, error);
}

void testFormat() {
    RehearsalRecord r = passed();
    r.notes.push_back("a \"quoted\" word\nand a second line");
    const std::string text = rehearsalText(r);
    RehearsalRecord back;
    Diagnostic error;
    CHECK(parses(text, back, error));
    r.notes.back() = "a \"quoted\" word and a second line";   // one line per value
    CHECK(back == r);
    CHECK(rehearsalFileName(r) == "2026-11-20-tester-b-linux-x86-64.swrehearsal");

    // Strict: unknown and repeated keys, a missing key, an empty required value, the header.
    const std::string base = rehearsalText(passed());
    auto refused = [&](const std::string& t, const std::string& why) {
        RehearsalRecord out = passed();
        out.tester = "unchanged";
        Diagnostic e;
        const bool ok = parses(t, out, e);
        CHECK(!ok);
        CHECK(e.code == "RehearsalUnreadable");
        CHECK(contains(e.message, why));
        CHECK(out.tester == "unchanged");   // a failed read changes nothing
    };
    refused(base + "colour blue\n", "unknown key colour");
    refused(base + "tester \"again\"\n", "duplicate key tester");
    refused("shockwave_rehearsal 2\n", "unsupported rehearsal record version 2");
    refused("tester \"x\"\n", "expected shockwave_rehearsal 1");
    std::string noMachine = base;
    noMachine.replace(noMachine.find("machine "), noMachine.find('\n', noMachine.find("machine ")) -
                                                      noMachine.find("machine ") + 1, "");
    refused(noMachine, "missing machine");
    std::string emptyTester = base;
    emptyTester.replace(emptyTester.find("tester \"Tester B\""), 17, "tester \"\"");
    refused(emptyTester, "tester is empty");
    // A rescued rehearsal can't be promoted by hand.
    std::string rescued = base;
    rescued.insert(rescued.find("result "), "intervention \"copied libvulkan by hand\"\n");
    refused(rescued, "a developer intervened");
    rescued.replace(rescued.find("result passed"), 13, "result failed");
    RehearsalRecord failedRecord;
    CHECK(parses(rescued, failedRecord, error));
    CHECK(!failedRecord.passed && failedRecord.interventions.size() == 1);
}

void testGate() {
    const std::vector<std::string> platforms{"linux-x86_64"};
    const std::string build = "eb9dfd1a69453e97";
    std::vector<RehearsalRecord> records;
    std::vector<std::string> files;
    RehearsalGate g = rehearsalGate("1.0.0", platforms, records, files, build);
    CHECK(g.verdict == RehearsalVerdict::Unverified && contains(g.reason, "no rehearsal of 1.0.0 yet"));

    records = {passed()};
    files = {"Release/Rehearsals/a.swrehearsal"};
    g = rehearsalGate("1.0.0", platforms, records, files, build);
    CHECK(g.verdict == RehearsalVerdict::Passed && g.record == 0 && contains(g.reason, "no developer intervention"));

    // A newer rehearsal that needed help fails it, naming the intervention.
    RehearsalRecord rescued = passed();
    rescued.recorded = "2026-11-21T09:00:00Z";
    rescued.passed = false;
    rescued.interventions = {"set the save folder by hand"};
    records.push_back(rescued);
    files.push_back("Release/Rehearsals/b.swrehearsal");
    g = rehearsalGate("1.0.0", platforms, records, files, build);
    CHECK(g.verdict == RehearsalVerdict::Failed && g.record == 1 && contains(g.reason, "set the save folder by hand"));

    // Another version, platform or build doesn't count, and says so.
    g = rehearsalGate("1.1.0", platforms, records, files, build);
    CHECK(g.verdict == RehearsalVerdict::Unverified && contains(g.reason, "2 of another version"));
    g = rehearsalGate("1.0.0", std::vector<std::string>{"windows-x86_64"}, records, files, build);
    CHECK(g.verdict == RehearsalVerdict::Unverified && contains(g.reason, "platform the contract doesn't name"));
    g = rehearsalGate("1.0.0", platforms, records, files, "0011223344556677");
    CHECK(g.verdict == RehearsalVerdict::Unverified && contains(g.reason, "of another build"));
    // An unknown build (no cook here) doesn't make a record stale.
    g = rehearsalGate("1.0.0", platforms, records, files, "");
    CHECK(g.verdict == RehearsalVerdict::Failed);
}

void testFilesAndReport() {
    const fs::path root = fs::temp_directory_path() / "shockwave-rehearsal-test";
    fs::remove_all(root);
    fs::create_directories(root);
    Project project;
    project.file = root / "Game.swproject";
    project.root = root;
    std::string error;
    const RehearsalRecord r = passed();
    CHECK(writeRehearsal(rehearsalFolder(project) / rehearsalFileName(r), r, error));
    {
        std::ofstream broken(rehearsalFolder(project) / "zz-broken.swrehearsal");
        broken << "shockwave_rehearsal 1\nwhat\n";
    }
    const RehearsalRecords read = readRehearsalRecords(project);
    CHECK(read.records.size() == 1 && read.records[0] == r);
    CHECK(read.files.size() == 1 && contains(read.files[0], "Release/Rehearsals/2026-11-20-tester-b"));
    CHECK(read.problems.size() == 1 && read.problems[0].code == "RehearsalUnreadable");

    // Through the release gates.
    ReleaseReport report;
    report.present = report.readable = true;
    report.settings.releaseVersion = "1.0.0";
    report.settings.platforms = {"linux-x86_64"};
    ReleaseInputs inputs;
    auto rehearsalStatus = [&]() {
        releaseGates(report, inputs);
        for (const ReleaseGate& g : report.gates)
            if (g.id == "release_rehearsal") {
                CHECK(g.batch == "S15");
                return g.status;
            }
        CHECK(false);
        return GateStatus::NotApplicable;
    };
    CHECK(rehearsalStatus() == GateStatus::Unverified);   // not read
    inputs.rehearsalChecked = true;
    inputs.rehearsalRecords = read.records;
    inputs.rehearsalFiles = read.files;
    CHECK(rehearsalStatus() == GateStatus::Passed);
    CHECK(!report.qualified());   // the other gates still need their evidence
    fs::remove_all(root);
}

}   // namespace

int main() {
    testFormat();
    testGate();
    testFilesAndReport();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("release rehearsal tests passed\n");
    return 0;
}
