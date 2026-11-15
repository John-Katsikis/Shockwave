#pragma once

// Release rehearsals (S15, docs/ReleaseRehearsal.md): a new tester, on a clean machine and
// profile, given only the package and the short player instructions, plays the release.
// Every time a developer had to step in is written down. The record is the evidence for
// the release_rehearsal gate.
//
//   shockwave_rehearsal 1
//   tester "Tester B (never seen the game)"
//   recorded "2026-11-20T15:00:00Z"
//   machine "Borrowed laptop, new user account"
//   platform linux-x86_64
//   package "TwoRooms-1.0.0-linux-x86_64"
//   version "1.0.0"
//   build_id "eb9dfd1a69453e97"
//   intervention "had to install libvulkan1 before the game would start"
//   note "found Continue without help"
//   result failed
//
// Any intervention fails the rehearsal: setting a path, copying a library, editing a save,
// opening a debug console, explaining something the instructions should have said. Fix it,
// then rehearse again with a tester who hasn't seen the game. Reading is strict (unknown
// keys are errors with file:line) and changes nothing on failure; writing goes through a
// temporary file.

#include <Projects/Diagnostic.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

struct Project;

struct RehearsalRecord {
    std::string tester;   // who played: a label is enough ("Tester B"), not a full name
    std::string recorded;   // UTC, ISO 8601 to the second: "2026-11-20T15:00:00Z"
    std::string machine;   // which machine and profile, as people know it
    std::string platform;   // the package's: "linux-x86_64"
    std::string package;   // the package's folder name
    std::string version;   // its release_version
    std::string buildId;   // its cook's build ID; "" when unknown
    std::vector<std::string> interventions;   // each time a developer stepped in
    std::vector<std::string> notes;   // anything else worth keeping
    bool passed = false;   // played through with no intervention (result passed)
    bool operator==(const RehearsalRecord&) const = default;
};

// ---- Files ------------------------------------------------------------------------------

// Release/Rehearsals under the project, where records live.
std::filesystem::path rehearsalFolder(const Project& project);
// "2026-11-20-tester-b-linux-x86-64.swrehearsal": the date of `recorded`, the tester, the platform.
std::string rehearsalFileName(const RehearsalRecord& record);

std::string rehearsalText(const RehearsalRecord& record);
// The first problem stops it, into `error` with `shown` and the line. False leaves `out`.
bool parseRehearsal(std::string_view text, std::string_view shown, RehearsalRecord& out, Diagnostic& error);
bool readRehearsal(const std::filesystem::path& file, std::string_view shown, RehearsalRecord& out, Diagnostic& error);
// Refuses a record that wouldn't read back, and an existing file: records are never overwritten.
bool writeRehearsal(const std::filesystem::path& file, const RehearsalRecord& record, std::string& error);

struct RehearsalRecords {
    std::vector<RehearsalRecord> records;
    std::vector<std::string> files;   // project-relative, one per record
    std::vector<Diagnostic> problems;   // records that don't read (RehearsalUnreadable)
};
// Every .swrehearsal in the project's Release/Rehearsals, sorted by file name.
RehearsalRecords readRehearsalRecords(const Project& project);

// ---- The gate ---------------------------------------------------------------------------

enum class RehearsalVerdict { Passed, Failed, Unverified };
struct RehearsalGate {
    RehearsalVerdict verdict = RehearsalVerdict::Unverified;
    std::string reason;
    int record = -1;   // the record it rests on, or -1
};
// The release_rehearsal gate for this version on these platforms. It rests on the newest
// record (by `recorded`) of this version, for one of the platforms, of the current build
// when both builds are known (`currentBuildId` "" is unknown). Passed when that record
// passed, Failed when it didn't (its interventions say why), Unverified without one.
RehearsalGate rehearsalGate(
    std::string_view version, std::span<const std::string> platforms, std::span<const RehearsalRecord> records,
    std::span<const std::string> files, std::string_view currentBuildId);

}   // namespace shockwave
