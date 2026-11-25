// Foliage data: see Assets/FoliageData.h and docs/Foliage.md.
#include <Assets/FoliageData.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

constexpr float kMinDistance = 1.0f, kMaxDistance = 5000.0f;

// Words of a line, a quoted string kept whole (without its quotes); # starts a comment.
bool splitLine(std::string_view line, std::vector<std::string>& out, bool& indented, std::string& error) {
    out.clear();
    indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] == '#') break;
        if (line[i] == '"') {
            const size_t close = line.find('"', i + 1);
            if (close == std::string_view::npos) {
                error = "a quote isn't closed";
                return false;
            }
            out.emplace_back(line.substr(i + 1, close - i - 1));
            i = close + 1;
            continue;
        }
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        out.emplace_back(line.substr(i, j - i));
        i = j;
    }
    return true;
}

bool number(const std::string& text, float& out) {
    const char* end = text.data() + text.size();
    auto [p, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && p == end && std::isfinite(out);
}

// The shortest text that reads back as the same float.
std::string formatFloat(float v) {
    char text[32];
    for (int digits = 1; digits <= 9; ++digits) {
        std::snprintf(text, sizeof(text), "%.*g", digits, static_cast<double>(v));
        if (std::strtof(text, nullptr) == v &&
            (!std::strchr(text, 'e') || std::fabs(v) >= 1e9f || std::fabs(v) < 1e-4f))
            break;
    }
    return text;
}

bool readBytes(const fs::path& path, std::vector<uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool replaceFile(const fs::path& path, const void* data, size_t size, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path temporary = path;
    temporary += ".saving";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out) {
            error = "can't write " + temporary.generic_string();
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        error = "can't replace " + path.generic_string();
        return false;
    }
    return true;
}

void putFloat(std::vector<uint8_t>& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

float getFloat(const uint8_t* p) {
    const uint32_t bits = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

}   // namespace

bool isFoliageFile(const fs::path& path) { return path.extension() == ".swfoliage"; }

bool parseFoliage(std::string_view text, FoliageData& foliage, std::string& error) {
    foliage = {};
    int lineNumber = 0;
    bool header = false, seenInstances = false;
    struct Seen {
        bool distance = false, fade = false, shadows = false, wind = false;
    };
    std::vector<Seen> seen;
    std::vector<std::string> w;
    auto fail = [&](const std::string& why) {
        error = lineNumber > 0 ? "line " + std::to_string(lineNumber) + ": " + why : why;
        return false;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::string why;
        bool indented = false;
        if (!splitLine(line, w, indented, why)) return fail(why);
        if (w.empty()) {
            if (end == text.size()) break;
            continue;
        }
        const size_t count = w.size() - 1;
        const std::string& key = w[0];
        auto typeKey = [&](bool Seen::*flag) {
            if (foliage.types.empty()) return fail(key + " needs a type line above it");
            if (seen.back().*flag)
                return fail(key + " given twice for type " + std::to_string(foliage.types.size() - 1));
            seen.back().*flag = true;
            return true;
        };
        if (!header) {
            if (key != "swfoliage") return fail("the first line must be \"swfoliage 1\"");
            if (w.size() != 2 || w[1] != "1") return fail("only swfoliage version 1 is known");
            header = true;
        } else if (key == "instances") {
            if (seenInstances) return fail("instances given twice");
            seenInstances = true;
            if (count != 1 || w[1].empty()) return fail("instances needs one quoted path");
            foliage.instancesFile = w[1];
        } else if (key == "type") {
            if (count != 1 || w[1].empty()) return fail("type needs one quoted model path");
            if (foliage.types.size() >= kFoliageMaxTypes) return fail("at most 256 types");
            FoliageType type;
            type.model = w[1];
            foliage.types.push_back(type);
            seen.emplace_back();
        } else if (key == "distance") {
            if (!typeKey(&Seen::distance)) return false;
            float v = 0;
            if (count != 1 || !number(w[1], v)) return fail("distance needs one number");
            if (v < kMinDistance || v > kMaxDistance) return fail("distance must be 1 to 5000 metres");
            foliage.types.back().distance = v;
        } else if (key == "fade") {
            if (!typeKey(&Seen::fade)) return false;
            float v = 0;
            if (count != 1 || !number(w[1], v)) return fail("fade needs one number");
            if (v < 0.0f || v > kMaxDistance) return fail("fade must be 0 to 5000 metres");
            foliage.types.back().fade = v;
        } else if (key == "shadows") {
            if (!typeKey(&Seen::shadows)) return false;
            if (count != 1 || (w[1] != "true" && w[1] != "false")) return fail("shadows must be true or false");
            foliage.types.back().shadows = w[1] == "true";
        } else if (key == "wind") {
            if (!typeKey(&Seen::wind)) return false;
            float v = 0;
            if (count != 1 || !number(w[1], v)) return fail("wind needs one number");
            if (v < 0.0f || v > 1.0f) return fail("wind must be 0 to 1");
            foliage.types.back().wind = v;
        } else {
            return fail("unknown key " + key);
        }
        if (end == text.size()) break;
    }
    lineNumber = 0;
    if (!header) return fail("empty: the first line must be \"swfoliage 1\"");
    if (!seenInstances) return fail("instances is missing");
    for (size_t i = 0; i < foliage.types.size(); ++i)
        if (foliage.types[i].fade > foliage.types[i].distance)
            return fail("type " + std::to_string(i) + "'s fade is longer than its distance");
    return true;
}

std::string foliageText(const FoliageData& f) {
    std::string out = "swfoliage 1\n";
    out += "instances \"" + f.instancesFile + "\"\n";
    for (const FoliageType& t : f.types) {
        out += "type \"" + t.model + "\"\n";
        out += "  distance " + formatFloat(t.distance) + "\n";
        out += "  fade " + formatFloat(t.fade) + "\n";
        out += std::string("  shadows ") + (t.shadows ? "true" : "false") + "\n";
        out += "  wind " + formatFloat(t.wind) + "\n";
    }
    return out;
}

bool decodeFoliageInstances(
    std::span<const uint8_t> bytes, size_t typeCount, std::vector<FoliageInstance>& instances, std::string& error) {
    if (bytes.size() % kFoliageInstanceBytes != 0) {
        error = "has " + std::to_string(bytes.size()) + " bytes, not a multiple of 24";
        return false;
    }
    const size_t n = bytes.size() / kFoliageInstanceBytes;
    std::vector<FoliageInstance> out(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = bytes.data() + i * kFoliageInstanceBytes;
        FoliageInstance& in = out[i];
        in.type = static_cast<uint16_t>(p[0] | (p[1] << 8));
        if (in.type >= typeCount) {
            error = "instance " + std::to_string(i) + " is type " + std::to_string(in.type) + "; there " +
                    (typeCount == 1 ? "is 1 type" : "are " + std::to_string(typeCount) + " types");
            return false;
        }
        in.position = {getFloat(p + 4), getFloat(p + 8), getFloat(p + 12)};
        in.yaw = getFloat(p + 16);
        in.scale = getFloat(p + 20);
        if (!std::isfinite(in.position.x) || !std::isfinite(in.position.y) || !std::isfinite(in.position.z) ||
            !std::isfinite(in.yaw) || !std::isfinite(in.scale)) {
            error = "instance " + std::to_string(i) + " isn't a finite number";
            return false;
        }
        in.random = foliageRandom(static_cast<uint32_t>(i));
    }
    instances = std::move(out);
    return true;
}

std::vector<uint8_t> encodeFoliageInstances(std::span<const FoliageInstance> instances) {
    std::vector<uint8_t> out;
    out.reserve(instances.size() * kFoliageInstanceBytes);
    for (const FoliageInstance& in : instances) {
        out.push_back(static_cast<uint8_t>(in.type & 0xff));
        out.push_back(static_cast<uint8_t>(in.type >> 8));
        out.push_back(0);
        out.push_back(0);
        putFloat(out, in.position.x);
        putFloat(out, in.position.y);
        putFloat(out, in.position.z);
        putFloat(out, in.yaw);
        putFloat(out, in.scale);
    }
    return out;
}

bool readFoliage(const fs::path& file, const fs::path& content, FoliageData& foliage, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readBytes(file, bytes)) {
        error = "can't open " + file.generic_string();
        return false;
    }
    FoliageData next;
    if (!parseFoliage(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), next, error))
        return false;
    std::vector<uint8_t> raw;
    if (!readBytes(content / next.instancesFile, raw)) {
        error = "the instances file " + next.instancesFile + " is missing";
        return false;
    }
    if (!decodeFoliageInstances(raw, next.types.size(), next.instances, error)) {
        error = "the instances file " + next.instancesFile + " " + error;
        return false;
    }
    sortFoliage(next);
    foliage = std::move(next);
    return true;
}

bool writeFoliage(const fs::path& file, const fs::path& content, const FoliageData& foliage, std::string& error) {
    if (foliage.instancesFile.empty()) {
        error = "the foliage names no instances file";
        return false;
    }
    for (const FoliageInstance& in : foliage.instances)
        if (in.type >= foliage.types.size()) {
            error = "an instance is type " + std::to_string(in.type) + " of " + std::to_string(foliage.types.size());
            return false;
        }
    const std::vector<uint8_t> raw = encodeFoliageInstances(foliage.instances);
    if (!replaceFile(content / foliage.instancesFile, raw.data(), raw.size(), error)) return false;
    const std::string text = foliageText(foliage);
    return replaceFile(file, text.data(), text.size(), error);
}

void sortFoliage(FoliageData& f) {
    auto cellOf = [](const FoliageInstance& in) {
        return glm::ivec2(
            int(std::floor(in.position.x / kFoliageCellMetres)), int(std::floor(in.position.z / kFoliageCellMetres)));
    };
    std::stable_sort(f.instances.begin(), f.instances.end(), [&](const FoliageInstance& a, const FoliageInstance& b) {
        if (a.type != b.type) return a.type < b.type;
        const glm::ivec2 ca = cellOf(a), cb = cellOf(b);
        if (ca.y != cb.y) return ca.y < cb.y;
        return ca.x < cb.x;
    });
    f.cells.clear();
    for (uint32_t i = 0; i < f.instances.size(); ++i) {
        const FoliageInstance& in = f.instances[i];
        const glm::ivec2 c = cellOf(in);
        if (f.cells.empty() || f.cells.back().type != in.type || f.cells.back().coord != c) {
            FoliageCell cell;
            cell.type = in.type;
            cell.coord = c;
            cell.first = i;
            cell.bounds = {in.position, in.position};
            f.cells.push_back(cell);
        }
        FoliageCell& cell = f.cells.back();
        ++cell.count;
        cell.maxScale = std::max(cell.maxScale, std::abs(in.scale));
        cell.bounds.min = glm::min(cell.bounds.min, in.position);
        cell.bounds.max = glm::max(cell.bounds.max, in.position);
    }
}

float foliageRandom(uint32_t index) {
    // A 32-bit integer hash (lowbias32), to 0..1 with 24 bits.
    uint32_t x = index + 0x9e3779b9u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return float(x >> 8) / float(1u << 24);
}

float foliageFade(float distance, float maxDistance, float fade) {
    if (distance >= maxDistance) return 1.0f;
    if (!(fade > 0.0f)) return 0.0f;
    return std::clamp((distance - (maxDistance - fade)) / fade, 0.0f, 1.0f);
}

glm::mat4 foliageMatrix(const FoliageInstance& in) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), in.position);
    m = glm::rotate(m, in.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::scale(m, glm::vec3(in.scale));
}

float foliageCellDistance(const FoliageCell& cell, const glm::vec3& point) {
    const glm::vec3 nearest = glm::clamp(point, cell.bounds.min, cell.bounds.max);
    return glm::length(point - nearest);
}

}   // namespace shockwave
