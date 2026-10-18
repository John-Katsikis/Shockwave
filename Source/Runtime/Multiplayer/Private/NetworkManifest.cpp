#include <Multiplayer/NetworkManifest.h>

#include <Multiplayer/Replication.h>

#include <Network/Sha256.h>
#include <Network/Wire.h>
#include <Projects/Project.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace shockwave::net {

namespace fs = std::filesystem;

namespace {

bool validProtocolName(std::string_view name) {
    if (name.empty() || name.size() > kMaxGameProtocolBytes) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

bool parseId(const std::string& text, uint16_t& out) {
    uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < 1 || value > 65535) return false;
    out = uint16_t(value);
    return true;
}

std::string quote(std::string_view text) {
    std::ostringstream out;
    out << std::quoted(std::string(text));
    return out.str();
}

// Whether the real location of root/relative is inside root, following links.
bool staysInside(const fs::path& root, const fs::path& file) {
    std::error_code ec;
    const fs::path realRoot = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const fs::path real = fs::weakly_canonical(file, ec);
    if (ec) return false;
    auto r = realRoot.begin();
    auto f = real.begin();
    for (; r != realRoot.end() && !r->empty(); ++r, ++f)
        if (f == real.end() || *r != *f) return false;
    return true;
}

bool readFile(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) return false;
    out = std::move(text).str();
    return true;
}

}   // namespace

const ManifestScene* NetworkManifest::scene(uint16_t id) const {
    for (const ManifestScene& s : scenes)
        if (s.id == id) return &s;
    return nullptr;
}

const ManifestPrefab* NetworkManifest::prefab(uint16_t id) const {
    for (const ManifestPrefab& p : prefabs)
        if (p.id == id) return &p;
    return nullptr;
}

bool parseNetworkManifest(std::string_view text, std::string_view shown, NetworkManifest& out, std::string& error) {
    NetworkManifest result;
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    std::string line;
    int lineNumber = 0;
    bool header = false;
    auto fail = [&](const std::string& message) {
        error = std::string(shown) + ":" + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    auto quotedValue = [](std::istringstream& row, std::string& value) {
        row >> std::ws;
        return row.peek() == '"' && bool(row >> std::quoted(value));
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && inQuotes) {
                ++i;
                continue;
            }
            if (line[i] == '"') inQuotes = !inQuotes;
            if (line[i] == '#' && !inQuotes) {
                line.erase(i);
                break;
            }
        }
        std::istringstream row(line);
        std::string key, extra;
        if (!(row >> key)) continue;
        if (!header) {
            std::string version;
            if (key != "shockwave_network_manifest" || !(row >> version) || (row >> extra))
                return fail("expected shockwave_network_manifest 1 as the first line");
            if (version != "1")
                return fail("unsupported network manifest version " + version + " (this build reads 1)");
            header = true;
            continue;
        }
        if (key == "game_protocol") {
            if (result.gameProtocolLine != 0)
                return fail("duplicate game_protocol (first on line " + std::to_string(result.gameProtocolLine) + ")");
            if (!quotedValue(row, result.gameProtocol) || (row >> extra))
                return fail("game_protocol takes one quoted name");
            if (!validProtocolName(result.gameProtocol))
                return fail("game_protocol must be 1..64 lowercase letters, digits and . _ -, like \"mygame.v1\"");
            result.gameProtocolLine = lineNumber;
        } else if (key == "scene") {
            std::string id;
            ManifestScene scene;
            if (!(row >> id) || !parseId(id, scene.id)) return fail("scene takes an ID from 1 to 65535, then a path");
            if (!quotedValue(row, scene.path) || (row >> extra)) return fail("scene takes an ID and one quoted path");
            scene.line = lineNumber;
            result.scenes.push_back(std::move(scene));
        } else if (key == "prefab") {
            std::string id;
            ManifestPrefab prefab;
            if (!(row >> id) || !parseId(id, prefab.id))
                return fail("prefab takes an ID from 1 to 65535, a path and a profile");
            if (!quotedValue(row, prefab.path) || !quotedValue(row, prefab.profile) || (row >> extra))
                return fail("prefab takes an ID, a quoted path and a quoted profile name");
            prefab.line = lineNumber;
            result.prefabs.push_back(std::move(prefab));
        } else {
            return fail("unknown manifest entry " + key + " (game_protocol, scene or prefab)");
        }
    }
    if (!header) return fail("expected shockwave_network_manifest 1 as the first line");
    out = std::move(result);
    error.clear();
    return true;
}

bool readNetworkManifest(const fs::path& file, NetworkManifest& out, std::string& error) {
    std::string text;
    if (!readFile(file, text)) {
        error = file.string() + ": can't read the network manifest";
        return false;
    }
    return parseNetworkManifest(text, file.string(), out, error);
}

std::string networkManifestText(const NetworkManifest& manifest) {
    std::vector<ManifestScene> scenes = manifest.scenes;
    std::vector<ManifestPrefab> prefabs = manifest.prefabs;
    std::sort(scenes.begin(), scenes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(prefabs.begin(), prefabs.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::string out = "shockwave_network_manifest 1\ngame_protocol " + quote(manifest.gameProtocol) + "\n";
    for (const ManifestScene& s : scenes) out += "scene " + std::to_string(s.id) + " " + quote(s.path) + "\n";
    for (const ManifestPrefab& p : prefabs)
        out += "prefab " + std::to_string(p.id) + " " + quote(p.path) + " " + quote(p.profile) + "\n";
    return out;
}

bool writeNetworkManifest(const fs::path& file, const NetworkManifest& manifest, std::string& error) {
    // Only what parsing would accept.
    NetworkManifest check;
    const std::string text = networkManifestText(manifest);
    if (!parseNetworkManifest(text, file.string(), check, error)) return false;
    return writeFileAtomically(file, text, error);
}

void validateNetworkManifest(
    const NetworkManifest& manifest, const Project& project, const ReplicationRegistry* registry,
    std::string_view shown, std::vector<NetProblem>& problems) {
    auto at = [&](int line) { return std::string(shown) + (line > 0 ? ":" + std::to_string(line) : "") + ": "; };
    auto error = [&](int line, const std::string& message) { problems.push_back({false, at(line) + message}); };
    if (manifest.gameProtocol.empty())
        error(0, "no game_protocol; name the gameplay protocol, like game_protocol \"mygame.v1\"");
    if (manifest.scenes.empty()) error(0, "lists no scenes; a session needs at least one");
    for (size_t i = 0; i < manifest.scenes.size(); ++i) {
        const ManifestScene& s = manifest.scenes[i];
        for (size_t j = 0; j < i; ++j) {
            if (manifest.scenes[j].id == s.id)
                error(
                    s.line, "scene ID " + std::to_string(s.id) + " is already used on line " +
                                std::to_string(manifest.scenes[j].line));
            if (manifest.scenes[j].path == s.path) error(s.line, "scene " + s.path + " is listed twice");
        }
        std::string why;
        if (!validRelativeNetworkPath(s.path, why)) {
            error(s.line, "scene path \"" + s.path + "\" " + why);
            continue;
        }
        if (!s.path.ends_with(".swscene")) error(s.line, "scene " + s.path + " isn't a .swscene");
        const fs::path file = project.root / s.path;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec))
            error(s.line, "scene " + s.path + " doesn't exist");
        else if (!staysInside(project.root, file))
            error(s.line, "scene " + s.path + " leads outside the project (through a link)");
    }
    for (size_t i = 0; i < manifest.prefabs.size(); ++i) {
        const ManifestPrefab& p = manifest.prefabs[i];
        for (size_t j = 0; j < i; ++j) {
            if (manifest.prefabs[j].id == p.id)
                error(
                    p.line, "prefab ID " + std::to_string(p.id) + " is already used on line " +
                                std::to_string(manifest.prefabs[j].line));
            if (manifest.prefabs[j].path == p.path) error(p.line, "prefab " + p.path + " is listed twice");
        }
        std::string why;
        if (!validRelativeNetworkPath(p.path, why)) {
            error(p.line, "prefab path \"" + p.path + "\" " + why);
            continue;
        }
        if (!p.path.ends_with(".swprefab")) error(p.line, "prefab " + p.path + " isn't a .swprefab");
        const fs::path file = project.assetFolder / p.path;
        std::error_code ec;
        if (!fs::is_regular_file(file, ec))
            error(p.line, "prefab " + p.path + " doesn't exist in " + project.relative(project.assetFolder));
        else if (!staysInside(project.assetFolder, file))
            error(p.line, "prefab " + p.path + " leads outside Content (through a link)");
        if (registry && registry->frozen() && !registry->profile(p.profile))
            error(p.line, "prefab " + p.path + ": profile " + p.profile + " isn't registered");
    }
}

// ---- Content digest -----------------------------------------------------------------------

bool contentTextFile(std::string_view path) {
    static constexpr std::string_view kText[] = {
        ".swscene", ".swprefab", ".swcircuit", ".swnetmanifest", ".swnetwork", ".swimport", ".swinput",
        ".swtheme", ".rml",      ".rcss",      ".txt",           ".md",        ".json",     ".csv"};
    for (const std::string_view ext : kText)
        if (path.ends_with(ext)) return true;
    return false;
}

Digest contentDigestOf(std::string_view gameProtocol, std::vector<ContentFile> files) {
    std::sort(files.begin(), files.end(), [](const ContentFile& a, const ContentFile& b) { return a.path < b.path; });
    Sha256 hash;
    hash.update("shockwave_content_digest 1\n");
    hash.update("game_protocol ");
    hash.update(gameProtocol);
    hash.update("\n");
    for (ContentFile& file : files) {
        if (contentTextFile(file.path)) {
            std::string normal;
            normal.reserve(file.bytes.size());
            for (size_t i = 0; i < file.bytes.size(); ++i)
                if (!(file.bytes[i] == '\r' && i + 1 < file.bytes.size() && file.bytes[i + 1] == '\n'))
                    normal += file.bytes[i];
            file.bytes = std::move(normal);
        }
        hash.update(file.path);
        const uint8_t zero = 0;
        hash.update(std::span(&zero, 1));
        hash.updateU64(file.bytes.size());
        hash.update(file.bytes);
    }
    return hash.finish();
}

bool computeContentDigest(
    const Project& project, const NetworkManifest& manifest, std::string_view manifestPath, ContentDigest& out,
    std::vector<NetProblem>& problems) {
    const size_t before = problems.size();
    std::vector<ContentFile> files;
    auto add = [&](const fs::path& file) {
        ContentFile content;
        content.path = project.relative(file);
        if (!readFile(file, content.bytes)) {
            problems.push_back({false, content.path + ": can't read it for the content digest"});
            return;
        }
        for (const ContentFile& f : files)
            if (f.path == content.path) return;
        files.push_back(std::move(content));
    };
    std::error_code ec;
    if (fs::is_directory(project.assetFolder, ec)) {
        fs::recursive_directory_iterator it(project.assetFolder, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path& path = it->path();
            const std::string name = path.filename().string();
            if (!name.empty() && name[0] == '.') {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_symlink(ec)) {
                problems.push_back(
                    {false, project.relative(path) + ": links aren't allowed in network content (copy the file in)"});
                continue;
            }
            if (it->is_regular_file(ec)) add(path);
        }
        if (ec) problems.push_back({false, project.relative(project.assetFolder) + ": " + ec.message()});
    }
    for (const ManifestScene& scene : manifest.scenes) {
        std::string why;
        if (validRelativeNetworkPath(scene.path, why) && fs::is_regular_file(project.root / scene.path, ec))
            add(project.root / scene.path);
    }
    if (!manifestPath.empty() && fs::is_regular_file(project.root / manifestPath, ec)) add(project.root / manifestPath);
    if (problems.size() != before) return false;
    out = {};
    for (const ContentFile& f : files) out.bytes += f.bytes.size();
    out.digest = contentDigestOf(manifest.gameProtocol, files);
    std::sort(files.begin(), files.end(), [](const ContentFile& a, const ContentFile& b) { return a.path < b.path; });
    for (const ContentFile& f : files) out.files.push_back(f.path);
    return true;
}

}   // namespace shockwave::net
