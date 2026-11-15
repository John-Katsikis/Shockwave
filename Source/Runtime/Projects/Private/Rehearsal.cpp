// Release rehearsal records and their gate (see Rehearsal.h, docs/ReleaseRehearsal.md).
#include <Projects/Rehearsal.h>

#include <Projects/Project.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace shockwave {

namespace {

std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

// One line per value: a newline in a note would split the record.
std::string oneLine(std::string text) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

bool splitLine(const std::string& line, std::vector<std::string>& words) {
    words.clear();
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '#') break;
        std::string word;
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    word += line[i + 1];
                    i += 2;
                } else if (line[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                } else {
                    word += line[i++];
                }
            }
            if (!closed) return false;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') word += line[i++];
        }
        words.push_back(std::move(word));
    }
    return true;
}

std::string slug(std::string_view text) {
    std::string out;
    for (const char raw : text) {
        const char c = (raw >= 'A' && raw <= 'Z') ? char(raw - 'A' + 'a') : raw;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out += c;
        else if ((c == ' ' || c == '-' || c == '_' || c == '.') && !out.empty() && out.back() != '-')
            out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 48) out.resize(48);
    return out;
}

// A platform is written bare only when it reads back as one word.
bool plainWord(const std::string& text) {
    if (text.empty()) return false;
    for (const char c : text)
        if (c == ' ' || c == '\t' || c == '"' || c == '#' || c == '\\' || c == '\r' || c == '\n') return false;
    return true;
}

// "2026-11-20T15:00:00Z": UTC, to the second, so records sort by time as text.
bool utcTimestamp(const std::string& t) {
    if (t.size() != 20) return false;
    for (size_t i = 0; i < t.size(); ++i) {
        const char want = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' : i == 19 ? 'Z' : '0';
        if (want == '0' ? !(t[i] >= '0' && t[i] <= '9') : t[i] != want) return false;
    }
    const int month = std::stoi(t.substr(5, 2)), day = std::stoi(t.substr(8, 2));
    const int hour = std::stoi(t.substr(11, 2)), minute = std::stoi(t.substr(14, 2)), second = std::stoi(t.substr(17, 2));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 60;
}

using R = RehearsalRecord;
struct TextKey {
    const char* name;
    std::string R::*member;
    bool word;   // written bare (a platform), not quoted
};
constexpr TextKey kTextKeys[] = {
    {"tester", &R::tester, false},   {"recorded", &R::recorded, false}, {"machine", &R::machine, false},
    {"platform", &R::platform, true}, {"package", &R::package, false},   {"version", &R::version, false},
    {"build_id", &R::buildId, false},
};

}   // namespace

std::filesystem::path rehearsalFolder(const Project& project) { return project.root / "Release" / "Rehearsals"; }

std::string rehearsalFileName(const RehearsalRecord& record) {
    std::string name = record.recorded.size() >= 10 ? record.recorded.substr(0, 10) : std::string();
    for (char c : name)
        if (!((c >= '0' && c <= '9') || c == '-')) {
            name.clear();
            break;
        }
    std::string tester = slug(record.tester);
    if (tester.empty()) tester = "tester";
    name += (name.empty() ? "" : "-") + tester;
    const std::string platform = slug(record.platform);
    if (!platform.empty()) name += "-" + platform;
    return name + ".swrehearsal";
}

std::string rehearsalText(const RehearsalRecord& record) {
    std::string out = "# A release rehearsal (docs/ReleaseRehearsal.md): a new tester on a clean machine, given\n"
                      "# only the package and the player instructions. Every developer intervention is listed.\n"
                      "shockwave_rehearsal 1\n";
    for (const TextKey& key : kTextKeys) {
        const std::string& value = record.*key.member;
        out += std::string(key.name) + " " +
               (key.word && plainWord(value) ? value : quoted(oneLine(value))) + "\n";
    }
    for (const std::string& i : record.interventions) out += "intervention " + quoted(oneLine(i)) + "\n";
    for (const std::string& n : record.notes) out += "note " + quoted(oneLine(n)) + "\n";
    out += std::string("result ") + (record.passed ? "passed" : "failed") + "\n";
    return out;
}

bool parseRehearsal(std::string_view text, std::string_view shown, RehearsalRecord& out, Diagnostic& error) {
    RehearsalRecord result;
    std::istringstream in{std::string(text)};
    std::string line;
    std::vector<std::string> words;
    bool seen[std::size(kTextKeys)] = {};
    bool seenResult = false;
    int lineNumber = 0;
    bool header = false;
    auto fail = [&](const std::string& object, const std::string& message) {
        error = {"RehearsalUnreadable", DiagnosticSeverity::Warning, std::string(shown), lineNumber, object, message};
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!splitLine(line, words)) return fail("", "a quote isn't closed");
        if (words.empty()) continue;
        const std::string key = words[0];
        if (!header) {
            if (key != "shockwave_rehearsal" || words.size() != 2)
                return fail("", "expected shockwave_rehearsal 1 as the first line");
            if (words[1] != "1")
                return fail("", "unsupported rehearsal record version " + words[1] + " (this build reads 1)");
            header = true;
            continue;
        }
        if (words.size() != 2) return fail(key, key + " takes one value");
        const std::string& value = words[1];
        if (key == "intervention" || key == "note") {
            if (value.empty()) return fail(key, key + " is empty");
            (key == "intervention" ? result.interventions : result.notes).push_back(value);
            continue;
        }
        if (key == "result") {
            if (seenResult) return fail(key, "duplicate key result");
            if (value != "passed" && value != "failed") return fail(key, "result is passed or failed, not " + value);
            result.passed = value == "passed";
            seenResult = true;
            continue;
        }
        const auto found = std::find_if(
            std::begin(kTextKeys), std::end(kTextKeys), [&](const TextKey& k) { return key == k.name; });
        if (found == std::end(kTextKeys)) return fail(key, "unknown key " + key);
        const size_t index = size_t(found - std::begin(kTextKeys));
        if (seen[index]) return fail(key, "duplicate key " + key);
        seen[index] = true;
        result.*(found->member) = value;
    }
    if (!header) return fail("", "expected shockwave_rehearsal 1 as the first line");
    lineNumber = 0;
    for (size_t i = 0; i < std::size(kTextKeys); ++i)
        if (!seen[i]) return fail(kTextKeys[i].name, std::string("missing ") + kTextKeys[i].name);
    if (!seenResult) return fail("result", "missing result");
    for (const char* required : {"tester", "recorded", "machine", "platform", "package", "version"}) {
        const auto key = std::find_if(
            std::begin(kTextKeys), std::end(kTextKeys), [&](const TextKey& k) { return std::string_view(k.name) == required; });
        if ((result.*(key->member)).empty()) return fail(required, std::string(required) + " is empty");
    }
    if (!utcTimestamp(result.recorded))
        return fail("recorded", "recorded is a UTC time like 2026-11-20T15:00:00Z, not " + result.recorded);
    // A hand edit can't promote a rehearsal someone had to rescue.
    if (result.passed && !result.interventions.empty())
        return fail(
            "result",
            "result says passed, but a developer intervened: record it as failed, fix what needed the help, and "
            "rehearse again");
    out = std::move(result);
    return true;
}

bool readRehearsal(const std::filesystem::path& file, std::string_view shown, RehearsalRecord& out, Diagnostic& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = {"RehearsalUnreadable", DiagnosticSeverity::Warning, std::string(shown), 0, "", "can't open it"};
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parseRehearsal(text.str(), shown, out, error);
}

bool writeRehearsal(const std::filesystem::path& file, const RehearsalRecord& record, std::string& error) {
    // Only what reads back: the gate must never see a record its reader would refuse.
    const std::string text = rehearsalText(record);
    {
        RehearsalRecord back;
        Diagnostic problem;
        if (!parseRehearsal(text, file.filename().string(), back, problem)) {
            error = file.string() + ": " + problem.message;
            return false;
        }
    }
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        error = file.string() + " already exists: rehearsals are never overwritten";
        return false;
    }
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    const std::filesystem::path temp = file.string() + "." +
                                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                       "-" + std::to_string(std::random_device{}()) + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            error = file.string() + ": couldn't write it";
            return false;
        }
    }
    // A hard link fails rather than replace a record another writer just made. Where the
    // file system has no hard links, fall back to a rename after the check above.
    std::filesystem::create_hard_link(temp, file, ec);
    if (!ec) {
        std::filesystem::remove(temp, ec);
        error.clear();
        return true;
    }
    if (ec == std::errc::file_exists) {
        error = file.string() + " already exists: rehearsals are never overwritten";
        std::filesystem::remove(temp, ec);
        return false;
    }
    ec.clear();
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    error.clear();
    return true;
}

RehearsalRecords readRehearsalRecords(const Project& project) {
    RehearsalRecords out;
    const std::filesystem::path folder = rehearsalFolder(project);
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return out;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".swrehearsal") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        const std::string shown = project.relative(file);
        RehearsalRecord record;
        Diagnostic error;
        if (readRehearsal(file, shown, record, error)) {
            out.records.push_back(std::move(record));
            out.files.push_back(shown);
        } else {
            out.problems.push_back(error);
        }
    }
    return out;
}

RehearsalGate rehearsalGate(
    std::string_view version, std::span<const std::string> platforms, std::span<const RehearsalRecord> records,
    std::span<const std::string> files, std::string_view currentBuildId) {
    RehearsalGate gate;
    int newest = -1;
    size_t otherVersion = 0, otherBuild = 0, otherPlatform = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        const RehearsalRecord& r = records[i];
        if (r.version != version) {
            ++otherVersion;
            continue;
        }
        if (std::find(platforms.begin(), platforms.end(), r.platform) == platforms.end()) {
            ++otherPlatform;
            continue;
        }
        if (!currentBuildId.empty() && !r.buildId.empty() && r.buildId != currentBuildId) {
            ++otherBuild;
            continue;
        }
        // ISO 8601 UTC sorts as text; ties go to the later file.
        if (newest < 0 || r.recorded >= records[size_t(newest)].recorded) newest = int(i);
    }
    const std::string handout =
        "a new tester on a clean machine and profile, given only the package and the player instructions "
        "(docs/ReleaseRehearsal.md)";
    if (newest < 0) {
        std::string why = "no rehearsal of " + std::string(version) + " yet: needs " + handout;
        if (otherBuild) why += "; " + std::to_string(otherBuild) + " of another build (rehearse this one)";
        if (otherVersion) why += "; " + std::to_string(otherVersion) + " of another version";
        if (otherPlatform) why += "; " + std::to_string(otherPlatform) + " on a platform the contract doesn't name";
        gate.reason = why;
        return gate;
    }
    const RehearsalRecord& r = records[size_t(newest)];
    const std::string where = (size_t(newest) < files.size() ? files[size_t(newest)] + ": " : std::string()) +
                              r.tester + " on " + r.machine + " (" + r.platform + ", " + r.recorded + ")";
    gate.record = newest;
    if (r.passed) {
        gate.verdict = RehearsalVerdict::Passed;
        gate.reason = where + " played it with no developer intervention";
        return gate;
    }
    gate.verdict = RehearsalVerdict::Failed;
    if (r.interventions.empty()) {
        gate.reason = where + " didn't finish it";
    } else {
        gate.reason = where + ": " + std::to_string(r.interventions.size()) + " intervention" +
                      (r.interventions.size() == 1 ? "" : "s") + ": " + r.interventions.front();
        if (r.interventions.size() > 1) gate.reason += "; ...";
    }
    gate.reason += ". Fix it, then rehearse again with a new tester";
    return gate;
}

}   // namespace shockwave
