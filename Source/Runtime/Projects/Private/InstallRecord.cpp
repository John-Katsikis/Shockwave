// Install records and the platform gates (see InstallRecord.h, docs/Release.md#platforms).
#include <Projects/InstallRecord.h>

#include <Projects/Project.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>

namespace shockwave {

namespace {

using R = InstallRecord;

std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << std::quoted(text);
    return out.str();
}

bool parseNumber(std::string_view text, double& out) {
    double value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) || value < 0) return false;
    out = value;
    return true;
}

bool parseWhole(std::string_view text, uint64_t& out) {
    uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return false;
    out = value;
    return true;
}

struct Key {
    const char* name;
    std::function<std::string(const R&)> write;
    std::function<bool(R&, const std::vector<std::string>&)> read;
    bool optional = false;   // added after version 1's first records; may be left out
};
Key optional(Key key) {
    key.optional = true;
    return key;
}

Key text(const char* name, std::string R::*member) {
    return {
        name, [member](const R& r) { return quoted(r.*member); },
        [member](R& r, const std::vector<std::string>& v) {
            if (v.size() != 1) return false;
            r.*member = v[0];
            return true;
        }};
}
Key word(const char* name, std::string R::*member) {
    return {
        name, [member](const R& r) { return (r.*member).empty() ? std::string("\"\"") : r.*member; },
        [member](R& r, const std::vector<std::string>& v) {
            if (v.size() != 1) return false;
            r.*member = v[0];
            return true;
        }};
}
Key whole(const char* name, uint64_t R::*member) {
    return {
        name, [member](const R& r) { return std::to_string(r.*member); },
        [member](R& r, const std::vector<std::string>& v) { return v.size() == 1 && parseWhole(v[0], r.*member); }};
}
Key flag(const char* name, bool R::*member) {
    return {
        name, [member](const R& r) { return std::string(r.*member ? "true" : "false"); },
        [member](R& r, const std::vector<std::string>& v) {
            if (v.size() != 1 || (v[0] != "true" && v[0] != "false")) return false;
            r.*member = v[0] == "true";
            return true;
        }};
}

const std::vector<Key>& keys() {
    static const std::vector<Key> table = {
        text("machine", &R::machine),
        text("recorded", &R::recorded),
        word("platform", &R::platform),
        text("os", &R::os),
        text("distribution", &R::distribution),
        text("glibc", &R::glibc),
        text("cpu", &R::cpu),
        word("renderer", &R::renderer),
        text("gpu", &R::gpu),
        word("gpu_kind", &R::gpuKind),
        text("gpu_driver", &R::gpuDriver),
        text("package", &R::package),
        text("version", &R::version),
        word("build_config", &R::buildConfig),
        text("build_id", &R::buildId),
        text("content_id", &R::contentId),
        word("package_sha256", &R::packageSha256),
        word("archive_sha256", &R::archiveSha256),
        whole("files", &R::files),
        whole("bytes", &R::bytes),
        optional(text("previous_package", &R::previousPackage)),
        optional(text("previous_version", &R::previousVersion)),
        optional(text("previous_build_id", &R::previousBuildId)),
        optional(word("previous_sha256", &R::previousSha256)),
        flag("read_only", &R::readOnly),
        flag("spaces_in_path", &R::spacesInPath),
        flag("unicode_path", &R::unicodePath),
        flag("other_folder", &R::otherFolder),
        flag("clean_environment", &R::cleanEnvironment),
        {"result", [](const R& r) { return std::string(r.passed ? "passed" : "failed"); },
         [](R& r, const std::vector<std::string>& v) {
             if (v.size() != 1 || (v[0] != "passed" && v[0] != "failed")) return false;
             r.passed = v[0] == "passed";
             return true;
         }},
    };
    return table;
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

// One line per value: newlines in a detail would split the record.
std::string oneLine(std::string text) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    return text;
}

bool optimised(std::string_view config) {
    return config == "Release" || config == "RelWithDebInfo" || config == "MinSizeRel";
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
    if (out.size() > 64) out.resize(64);
    return out;
}

}   // namespace

const char* installStepResultName(InstallStepResult result) {
    switch (result) {
    case InstallStepResult::Passed: return "passed";
    case InstallStepResult::Failed: return "failed";
    case InstallStepResult::Skipped: return "skipped";
    }
    return "failed";
}

const InstallStep* InstallRecord::step(std::string_view name) const {
    for (const InstallStep& s : steps)
        if (s.name == name) return &s;
    return nullptr;
}

std::filesystem::path installFolder(const Project& project) { return project.root / "Release" / "Installs"; }

std::string installFileName(std::string_view machine, std::string_view platform) {
    std::string name = slug(machine);
    if (name.empty()) name = "machine";
    const std::string p = slug(platform);
    return name + (p.empty() ? std::string() : "-" + p) + ".swinstall";
}

std::string installText(const InstallRecord& record) {
    std::string out = "# An installed package played on one machine (docs/Release.md#platforms), written by\n"
                      "# player --verify-install --install-record.\n"
                      "shockwave_install 1\n";
    const auto& table = keys();
    for (const Key& key : table) {
        if (std::string_view(key.name) == "result") continue;   // last, after the steps
        out += std::string(key.name) + " " + oneLine(key.write(record)) + "\n";
    }
    for (const InstallStep& s : record.steps)
        out += "step " + quoted(oneLine(s.name)) + " " + installStepResultName(s.result) + " " + number(s.seconds) +
               " " + quoted(oneLine(s.detail)) + "\n";
    out += std::string("result ") + (record.passed ? "passed" : "failed") + "\n";
    return out;
}

bool parseInstall(std::string_view text, std::string_view shown, InstallRecord& out, Diagnostic& error) {
    InstallRecord result;
    std::istringstream in{std::string(text)};
    std::string line;
    std::vector<std::string> words;
    std::vector<bool> seen(keys().size(), false);
    int lineNumber = 0;
    bool header = false;
    auto fail = [&](const std::string& object, const std::string& message) {
        error = {"InstallUnreadable", DiagnosticSeverity::Warning, std::string(shown), lineNumber, object, message};
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!splitLine(line, words)) return fail("", "a quote isn't closed");
        if (words.empty()) continue;
        const std::string key = words[0];
        const std::vector<std::string> values(words.begin() + 1, words.end());
        if (!header) {
            if (key != "shockwave_install" || values.size() != 1)
                return fail("", "expected shockwave_install 1 as the first line");
            if (values[0] != "1")
                return fail("", "unsupported install record version " + values[0] + " (this build reads 1)");
            header = true;
            continue;
        }
        if (key == "step") {
            InstallStep s;
            if (values.size() != 4 || !parseNumber(values[2], s.seconds))
                return fail(key, "step takes \"name\" passed|failed|skipped seconds \"detail\"");
            s.name = values[0];
            if (values[1] == "passed")
                s.result = InstallStepResult::Passed;
            else if (values[1] == "failed")
                s.result = InstallStepResult::Failed;
            else if (values[1] == "skipped")
                s.result = InstallStepResult::Skipped;
            else
                return fail(key, "a step is passed, failed or skipped, not " + values[1]);
            s.detail = values[3];
            result.steps.push_back(std::move(s));
            continue;
        }
        const auto& table = keys();
        const auto found = std::find_if(table.begin(), table.end(), [&](const Key& k) { return key == k.name; });
        if (found == table.end()) return fail(key, "unknown key " + key);
        const size_t index = size_t(found - table.begin());
        if (seen[index]) return fail(key, "duplicate key " + key);
        if (!found->read(result, values)) return fail(key, "bad value for " + key);
        seen[index] = true;
    }
    if (!header) return fail("", "expected shockwave_install 1 as the first line");
    for (size_t i = 0; i < seen.size(); ++i)
        if (!seen[i] && !keys()[i].optional) {
            lineNumber = 0;
            return fail(keys()[i].name, std::string("missing ") + keys()[i].name);
        }
    // "result passed" has to agree with the steps: a hand edit can't promote a failed run.
    const bool allPassed =
        !result.steps.empty() && std::all_of(result.steps.begin(), result.steps.end(), [](const InstallStep& s) {
            return s.result != InstallStepResult::Failed;
        });
    if (result.passed && !allPassed) {
        lineNumber = 0;
        return fail("result", "result says passed, but a step failed or there are none");
    }
    out = std::move(result);
    return true;
}

bool readInstall(const std::filesystem::path& file, std::string_view shown, InstallRecord& out, Diagnostic& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = {"InstallUnreadable", DiagnosticSeverity::Warning, std::string(shown), 0, "", "can't open it"};
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parseInstall(text.str(), shown, out, error);
}

bool writeInstall(const std::filesystem::path& file, const InstallRecord& record, std::string& error) {
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    const std::filesystem::path temp =
        file.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
    {
        const std::string text = installText(record);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            error = file.string() + ": couldn't write it";
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    error.clear();
    return true;
}

InstallRecords readInstallRecords(const Project& project) {
    InstallRecords out;
    const std::filesystem::path folder = installFolder(project);
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return out;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".swinstall") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        const std::string shown = project.relative(file);
        InstallRecord record;
        Diagnostic error;
        if (readInstall(file, shown, record, error)) {
            out.records.push_back(std::move(record));
            out.files.push_back(shown);
        } else {
            out.problems.push_back(error);
        }
    }
    return out;
}

// ---- The gate ---------------------------------------------------------------------------

std::span<const char* const> requiredInstallSteps() {
    static constexpr const char* steps[] = {"install", "check", "user data", "update", "downgrade", "reinstall"};
    return steps;
}

std::vector<std::string> installProblems(const InstallRecord& r, std::string_view currentBuildId) {
    std::vector<std::string> problems;
    for (const InstallStep& s : r.steps)
        if (s.result == InstallStepResult::Failed) problems.push_back("step \"" + s.name + "\" failed: " + s.detail);
    if (r.buildId.empty())
        problems.push_back("it's a development package; only a release package (player --package --release) counts");
    else if (!currentBuildId.empty() && r.buildId != currentBuildId)
        problems.push_back("it's build " + r.buildId + "; the game is build " + std::string(currentBuildId) + " now");
    if (!optimised(r.buildConfig))
        problems.push_back(
            "it's a " + (r.buildConfig.empty() ? std::string("unknown") : r.buildConfig) +
            " build; only an optimised build counts");
    if (r.renderer != "vulkan" || r.gpuKind == "none" || r.gpuKind.empty())
        problems.push_back(
            "it played on the " + (r.renderer.empty() ? std::string("unknown") : r.renderer) +
            " renderer, which proves the install but not the graphics; play it with a real GPU");
    else if (r.gpuKind == "software" || r.gpuKind == "virtual")
        problems.push_back("it played on " + r.gpu + ", a " + r.gpuKind + " GPU; only real hardware counts");
    if (!(r.readOnly && r.spacesInPath && r.unicodePath && r.otherFolder && r.cleanEnvironment))
        problems.push_back(
            "it wasn't installed the hard way (read-only, spaces and non-ASCII letters in the path, another "
            "working folder, a clean environment)");
    bool scenario = false;
    for (const InstallStep& s : r.steps)
        if (s.name.rfind("scenario ", 0) == 0 && s.result == InstallStepResult::Passed) scenario = true;
    if (!scenario) problems.push_back("no scenario was played");
    for (const char* name : requiredInstallSteps()) {
        const InstallStep* s = r.step(name);
        if (!s || s->result == InstallStepResult::Skipped)
            problems.push_back(
                std::string("no \"") + name + "\" step" +
                (std::string_view(name) == "update" || std::string_view(name) == "downgrade"
                     ? " (verify with --update-from <the previous package>)"
                     : ""));
    }
    return problems;
}

InstallGate installGate(
    std::string_view platform, std::span<const InstallRecord> records, std::span<const std::string> files,
    std::string_view currentBuildId) {
    InstallGate gate;
    auto shown = [&](int i) { return size_t(i) < files.size() ? files[size_t(i)] : records[size_t(i)].machine; };
    int newest = -1, newestCurrent = -1;
    for (int i = 0; i < int(records.size()); ++i) {
        const InstallRecord& r = records[size_t(i)];
        if (r.platform != platform) continue;
        if (newest < 0 || r.recorded > records[size_t(newest)].recorded) newest = i;
        const bool current = currentBuildId.empty() || r.buildId == currentBuildId;
        if (current && (newestCurrent < 0 || r.recorded > records[size_t(newestCurrent)].recorded)) newestCurrent = i;
        if (installProblems(r, currentBuildId).empty()) {
            gate.verdict = InstallVerdict::Passed;
            gate.record = i;
            gate.reason = r.machine + " (" + r.os + (r.distribution.empty() ? "" : ", " + r.distribution) +
                          "): installed, played, updated and reinstalled; " + shown(i);
            return gate;
        }
    }
    if (newest < 0) {
        gate.reason = "no install record for " + std::string(platform) +
                      " yet: run player --verify-install on that platform (docs/Release.md#platforms)";
        return gate;
    }
    if (newestCurrent < 0) {   // only other builds' records (or development packages)
        gate.record = newest;
        gate.reason = shown(newest) + " doesn't qualify " + std::string(platform) + ": " +
                      installProblems(records[size_t(newest)], currentBuildId).front();
        return gate;
    }
    const InstallRecord& r = records[size_t(newestCurrent)];
    gate.record = newestCurrent;
    const std::vector<std::string> problems = installProblems(r, currentBuildId);
    if (!r.passed) {
        gate.verdict = InstallVerdict::Failed;
        gate.reason = shown(newestCurrent) + ": " + (problems.empty() ? std::string("it failed") : problems.front());
        return gate;
    }
    gate.reason = shown(newestCurrent) + " doesn't qualify " + std::string(platform) + ": " + problems.front();
    return gate;
}

}   // namespace shockwave
