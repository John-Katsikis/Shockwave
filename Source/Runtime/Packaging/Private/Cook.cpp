// The cook manifest (docs/Cooking.md): Cook.swcook text, its IDs, JSON and verification.
#include <Packaging/Cook.h>
#include <Packaging/Package.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

// FNV-1a, 64 bits: the same hash the asset graph keeps (assetHash).
uint64_t fnv(std::string_view bytes, uint64_t h = 14695981039346656037ull) {
    for (const unsigned char c : bytes) h = (h ^ c) * 1099511628211ull;
    return h;
}

bool readFile(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    out = all.str();
    return !in.bad();
}

bool parseHex(const std::string& text, uint64_t& value) {
    if (text.size() != 16) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return ec == std::errc() && end == text.data() + text.size();
}

std::string quoted(std::string_view text) {
    std::ostringstream out;
    out << std::quoted(std::string(text));
    return out.str();
}

std::string jsonString(std::string_view text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\')
            out += '\\', out += char(c);
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else
            out += char(c);
    }
    return out + "\"";
}

// The lines the content ID covers: what the game is made of, not how it was built.
std::string contentText(const CookManifest& m) {
    std::ostringstream out;
    out << "application " << quoted(m.application) << "\n"
        << "game_module " << quoted(m.gameModule) << "\n"
        << "entry " << quoted(m.entryLevel) << "\n";
    for (const CookLevel& l : m.levels)
        out << "level " << l.id << " " << quoted(l.scene) << " " << hex64(l.closure) << "\n";
    for (const CookRoot& r : m.roots) out << "root " << quoted(r.id) << " " << quoted(r.why) << "\n";
    for (const CookAsset& a : m.assets) {
        out << "asset " << quoted(a.id) << " " << quoted(a.kind) << " " << a.bytes << " " << hex64(a.hash) << " "
            << hex64(a.closure);
        for (const std::string& level : a.levels) out << " " << level;
        out << "\n";
    }
    return out.str();
}

}   // namespace

std::string hex64(uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(value));
    return buf;
}

const CookAsset* CookManifest::asset(std::string_view id) const {
    const auto it = std::lower_bound(
        assets.begin(), assets.end(), id, [](const CookAsset& a, std::string_view key) { return a.id < key; });
    return it != assets.end() && it->id == id ? &*it : nullptr;
}

void CookManifest::computeIds() {
    std::sort(levels.begin(), levels.end(), [](const CookLevel& a, const CookLevel& b) { return a.id < b.id; });
    std::sort(roots.begin(), roots.end(), [](const CookRoot& a, const CookRoot& b) { return a.id < b.id; });
    std::sort(assets.begin(), assets.end(), [](const CookAsset& a, const CookAsset& b) { return a.id < b.id; });
    for (CookAsset& a : assets) std::sort(a.levels.begin(), a.levels.end());
    std::sort(licenses.begin(), licenses.end());
    contentId = fnv(contentText(*this));
    std::ostringstream build;
    build << hex64(contentId) << " " << target << " " << buildConfig << " " << engineVersion << " " << pluginAbi << " "
          << (release ? "release" : "development");
    for (const std::string& license : licenses) build << " " << license;
    buildId = fnv(build.str());
}

std::string cookManifestText(const CookManifest& m) {
    std::ostringstream out;
    out << "swcook " << m.version << "\n"
        << "# What this game's release is made of: player --cook wrote it, player --cook-info reads it.\n"
        << "# See docs/Cooking.md. Every list is sorted; nothing here depends on when or where it was made.\n"
        << "content_id " << hex64(m.contentId) << "\n"
        << "build_id " << hex64(m.buildId) << "\n"
        << "target " << m.target << "\n"
        << "build_config " << quoted(m.buildConfig) << "\n"
        << "engine_version " << quoted(m.engineVersion) << "\n"
        << "plugin_abi " << m.pluginAbi << "\n"
        << "release " << (m.release ? "true" : "false") << "\n";
    for (const std::string& license : m.licenses) out << "license " << quoted(license) << "\n";
    out << contentText(m);
    return out.str();
}

bool parseCookManifest(std::string_view text, CookManifest& manifest, std::string& error) {
    CookManifest m;
    std::istringstream in{std::string(text)};
    std::string line;
    int number = 0;
    bool header = false;
    uint64_t contentId = 0, buildId = 0;
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(number) + ": " + why;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        std::istringstream row(line);
        std::string key;
        if (!(row >> key) || key[0] == '#') continue;
        if (!header) {
            if (key != "swcook" || !(row >> m.version) || m.version != kCookManifestVersion)
                return fail("expected swcook " + std::to_string(kCookManifestVersion) + " first");
            header = true;
            continue;
        }
        std::string a, b;
        if (key == "content_id" || key == "build_id") {
            if (!(row >> a) || !parseHex(a, key == "content_id" ? contentId : buildId))
                return fail(key + " is 16 hex digits");
        } else if (key == "target") {
            if (!(row >> m.target)) return fail("target needs a platform");
        } else if (
            key == "build_config" || key == "engine_version" || key == "application" || key == "game_module" ||
            key == "entry" || key == "license") {
            if (!(row >> std::quoted(a))) return fail(key + " needs a quoted value");
            if (key == "build_config")
                m.buildConfig = a;
            else if (key == "engine_version")
                m.engineVersion = a;
            else if (key == "application")
                m.application = a;
            else if (key == "game_module")
                m.gameModule = a;
            else if (key == "entry")
                m.entryLevel = a;
            else
                m.licenses.push_back(a);
        } else if (key == "plugin_abi") {
            if (!(row >> m.pluginAbi)) return fail("plugin_abi is a number");
        } else if (key == "release") {
            if (!(row >> a) || (a != "true" && a != "false")) return fail("release is true or false");
            m.release = a == "true";
        } else if (key == "level") {
            CookLevel l;
            if (!(row >> l.id >> std::quoted(l.scene) >> a) || !parseHex(a, l.closure))
                return fail("level is: id \"scene\" closure");
            m.levels.push_back(std::move(l));
        } else if (key == "root") {
            CookRoot r;
            if (!(row >> std::quoted(r.id) >> std::quoted(r.why))) return fail("root is: \"id\" \"why\"");
            m.roots.push_back(std::move(r));
        } else if (key == "asset") {
            CookAsset asset;
            if (!(row >> std::quoted(asset.id) >> std::quoted(asset.kind) >> asset.bytes >> a >> b) ||
                !parseHex(a, asset.hash) || !parseHex(b, asset.closure))
                return fail("asset is: \"id\" \"kind\" bytes hash closure [levels]");
            std::string level;
            while (row >> level) asset.levels.push_back(level);
            m.assets.push_back(std::move(asset));
        } else
            return fail("unknown record '" + key + "'");
    }
    if (!header) return fail("expected swcook " + std::to_string(kCookManifestVersion) + " first");
    m.computeIds();
    if (m.contentId != contentId || m.buildId != buildId) {
        error = "its IDs don't match its records: it was edited or damaged after the cook";
        return false;
    }
    manifest = std::move(m);
    error.clear();
    return true;
}

bool loadCookManifest(const fs::path& file, CookManifest& manifest, std::string& error) {
    std::string text;
    if (!readFile(file, text)) {
        error = "can't read " + file.generic_string();
        return false;
    }
    if (!parseCookManifest(text, manifest, error)) {
        error = file.generic_string() + ": " + error;
        return false;
    }
    return true;
}

bool saveCookManifest(const fs::path& file, const CookManifest& manifest, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!(out << cookManifestText(manifest)) || !out.flush()) {
            error = "can't write " + temp.generic_string();
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        error = "can't replace " + file.generic_string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::string cookManifestJson(const CookManifest& m) {
    std::ostringstream out;
    out << "{\n  \"format\": \"swcook\",\n  \"version\": " << m.version << ",\n"
        << "  \"application\": " << jsonString(m.application) << ",\n"
        << "  \"game_module\": " << jsonString(m.gameModule) << ",\n"
        << "  \"target\": " << jsonString(m.target) << ",\n"
        << "  \"build_config\": " << jsonString(m.buildConfig) << ",\n"
        << "  \"engine_version\": " << jsonString(m.engineVersion) << ",\n"
        << "  \"plugin_abi\": " << m.pluginAbi << ",\n"
        << "  \"release\": " << (m.release ? "true" : "false") << ",\n"
        << "  \"content_id\": \"" << hex64(m.contentId) << "\",\n"
        << "  \"build_id\": \"" << hex64(m.buildId) << "\",\n"
        << "  \"entry_level\": " << jsonString(m.entryLevel) << ",\n  \"levels\": [";
    for (size_t i = 0; i < m.levels.size(); ++i) {
        const CookLevel& l = m.levels[i];
        out << (i ? "," : "") << "\n    {\"id\": " << jsonString(l.id) << ", \"scene\": " << jsonString(l.scene)
            << ", \"closure\": \"" << hex64(l.closure) << "\", \"assets\": [";
        bool first = true;
        for (const CookAsset& a : m.assets)
            if (std::binary_search(a.levels.begin(), a.levels.end(), l.id)) {
                out << (first ? "" : ", ") << jsonString(a.id);
                first = false;
            }
        out << "]}";
    }
    out << "\n  ],\n  \"roots\": [";
    for (size_t i = 0; i < m.roots.size(); ++i)
        out << (i ? "," : "") << "\n    {\"id\": " << jsonString(m.roots[i].id)
            << ", \"why\": " << jsonString(m.roots[i].why) << "}";
    out << "\n  ],\n  \"assets\": [";
    for (size_t i = 0; i < m.assets.size(); ++i) {
        const CookAsset& a = m.assets[i];
        out << (i ? "," : "") << "\n    {\"id\": " << jsonString(a.id) << ", \"kind\": " << jsonString(a.kind)
            << ", \"bytes\": " << a.bytes << ", \"hash\": \"" << hex64(a.hash) << "\", \"closure\": \""
            << hex64(a.closure) << "\", \"levels\": [";
        for (size_t j = 0; j < a.levels.size(); ++j) out << (j ? ", " : "") << jsonString(a.levels[j]);
        out << "]}";
    }
    out << "\n  ],\n  \"licenses\": [";
    for (size_t i = 0; i < m.licenses.size(); ++i) out << (i ? ", " : "") << jsonString(m.licenses[i]);
    out << "]\n}\n";
    return out.str();
}

fs::path cookManifestPath(const fs::path& projectRoot, std::string_view target) {
    return projectRoot / "Saved" / "Cook" / std::string(target) / "Cook.swcook";
}

std::vector<std::string> verifyCookedFiles(const CookManifest& manifest, const fs::path& root) {
    std::vector<std::string> problems;
    for (const CookAsset& a : manifest.assets) {
        if (a.kind == "builtin") continue;   // made in code: no file
        std::string bytes;
        const fs::path file = root / fs::path(std::u8string(a.id.begin(), a.id.end()));   // UTF-8 everywhere
        std::error_code ec;
        if (!fs::is_regular_file(file, ec) || !readFile(file, bytes)) {
            problems.push_back(a.id + ": missing");
            continue;
        }
        if (bytes.size() != a.bytes || fnv(bytes) != a.hash)
            problems.push_back(
                a.id + ": changed since the cook (" + std::to_string(bytes.size()) + " bytes, hash " +
                hex64(fnv(bytes)) + "; the manifest has " + std::to_string(a.bytes) + ", " + hex64(a.hash) + ")");
    }
    return problems;
}

CookReport cookReport(const fs::path& projectRoot, std::string_view target, bool verify) {
    CookReport report;
    report.file = cookManifestPath(projectRoot, target);
    std::error_code ec;
    if (!fs::is_regular_file(report.file, ec)) return report;
    if (!loadCookManifest(report.file, report.manifest, report.error)) return report;
    report.cooked = true;
    const CookManifest& m = report.manifest;
    for (const CookLevel& level : m.levels) {
        CookReport::Level row{level.id, level.scene};
        for (const CookAsset& a : m.assets) {
            if (std::find(a.levels.begin(), a.levels.end(), level.id) == a.levels.end()) continue;
            ++row.assets;
            row.bytes += a.bytes;
            if (a.levels.size() > 1) ++row.shared;
        }
        report.levels.push_back(std::move(row));
    }
    for (const CookAsset& a : m.assets) {
        if (a.kind == "builtin") continue;
        ++report.files;
        report.bytes += a.bytes;
    }
    if (verify) {
        report.verified = true;
        report.changed = verifyCookedFiles(m, projectRoot);
    }
    return report;
}

std::vector<std::string> cookTargets() { return {packagePlatform()}; }

}   // namespace shockwave
