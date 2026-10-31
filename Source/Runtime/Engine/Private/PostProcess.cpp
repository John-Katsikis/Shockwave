#include <Engine/PostProcess.h>

#include <Core/Log.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>
#include <Renderer/Renderer.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>

namespace shockwave {

namespace fs = std::filesystem;

void registerPostProcessComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    registerComponent<PostProcessVolume>(
        "PostProcessVolume",
        {
            field("profile", &PostProcessVolume::profile),
            field("enabled", &PostProcessVolume::enabled),
            field("unbounded", &PostProcessVolume::unbounded),
            field("half_extents", &PostProcessVolume::halfExtents, 0.05f, 0.01f, 100000.0f),
            field("priority", &PostProcessVolume::priority, 1.0f, -1000.0f, 1000.0f),
            field("blend_distance", &PostProcessVolume::blendDistance, 0.05f, 0.0f, 1000.0f),
            field("weight", &PostProcessVolume::weight, 0.01f, 0.0f, 1.0f),
        },
        "Rendering");
}

// ---- The schema ----

const std::vector<PostProcessParamInfo>& postProcessParams() {
    using T = PostProcessValueType;
    using V = PostProcessValues;
    static const std::vector<PostProcessParamInfo> params = {
        {"exposure",
         T::Float,
         offsetof(V, exposure),
         -16,
         16,
         {},
         "exposure",
         "stops",
         "Manual exposure: +1 doubles the brightness, -1 halves it. The scene's Environment exposure by default."},
        {"auto_exposure",
         T::Bool,
         offsetof(V, autoExposure),
         0,
         0,
         {},
         "auto_exposure",
         "",
         "Meters the scene and adapts exposure to it, within the limits below. Off keeps the manual exposure."},
        {"auto_compensation",
         T::Float,
         offsetof(V, autoCompensation),
         -8,
         8,
         {},
         "auto_exposure",
         "stops",
         "Added to the metered exposure: above 0 brighter than metered."},
        {"auto_min_ev",
         T::Float,
         offsetof(V, autoMinEv),
         -16,
         16,
         {},
         "auto_exposure",
         "stops",
         "The lowest exposure it may choose (for very bright scenes)."},
        {"auto_max_ev",
         T::Float,
         offsetof(V, autoMaxEv),
         -16,
         16,
         {},
         "auto_exposure",
         "stops",
         "The highest exposure it may choose (for very dark scenes)."},
        {"auto_target_gray",
         T::Float,
         offsetof(V, autoTargetGray),
         0.01f,
         1,
         {},
         "auto_exposure",
         "",
         "The scene's metered average becomes this grey (linear)."},
        {"auto_low_percentile",
         T::Float,
         offsetof(V, autoLowPercentile),
         0,
         1,
         {},
         "auto_exposure",
         "",
         "The darkest share of the screen ignored by metering."},
        {"auto_high_percentile",
         T::Float,
         offsetof(V, autoHighPercentile),
         0,
         1,
         {},
         "auto_exposure",
         "",
         "Metering ignores the screen past this share, brightest last: a muzzle flash or the sun doesn't darken it."},
        {"auto_brighten_time",
         T::Float,
         offsetof(V, autoBrightenTime),
         0,
         60,
         {},
         "auto_exposure",
         "seconds",
         "Time constant for raising exposure (going into the dark): about two thirds of the way there after it."},
        {"auto_darken_time",
         T::Float,
         offsetof(V, autoDarkenTime),
         0,
         60,
         {},
         "auto_exposure",
         "seconds",
         "Time constant for lowering exposure (going into bright light)."},
        {"auto_metering",
         T::Enum,
         offsetof(V, autoMetering),
         0,
         0,
         {"average", "center"},
         "auto_exposure",
         "",
         "Average weighs the whole screen; center weighs the middle most, the corners an eighth."},
        {"auto_reset_on_cut",
         T::Bool,
         offsetof(V, autoResetOnCut),
         0,
         0,
         {},
         "auto_exposure",
         "",
         "A camera cut jumps straight to the metered exposure instead of adapting."},
        {"white_point",
         T::Vec2,
         offsetof(V, whitePoint),
         0.01f,
         0.99f,
         {},
         "white_balance",
         "CIE xy",
         "Illuminant white to neutralize: a surface lit by this white comes out neutral. D65 (0.3127 0.3290) changes "
         "nothing; incandescent light is about 0.4476 0.4074."},
        {"lut",
         T::Path,
         offsetof(V, lut),
         0,
         0,
         {},
         "lut",
         "",
         "A .cube 3D LUT (17, 33 or 65 entries a side) inside Content, applied after tone mapping."},
        {"lut_encoding",
         T::Enum,
         offsetof(V, lutEncoding),
         0,
         0,
         {"srgb", "linear"},
         "lut",
         "",
         "What the LUT's input and output are: sRGB-encoded (most grading tools) or display-linear."},
        {"lut_intensity",
         T::Float,
         offsetof(V, lutIntensity),
         0,
         1,
         {},
         "lut",
         "",
         "How much of the graded colour is used: 0 none, 1 all of it."},
        {"bloom_intensity",
         T::Float,
         offsetof(V, bloomIntensity),
         0,
         1,
         {},
         "bloom",
         "",
         "Bright light spreading into its surroundings. The scene's Environment bloom by default."},
        {"fog_density",
         T::Float,
         offsetof(V, fogDensity),
         0,
         10,
         {},
         "fog",
         "1/m",
         "Extinction at fog_base_height: 0.01 hides things about 300 m away, 0.1 about 30 m. 0: no fog."},
        {"fog_height_falloff",
         T::Float,
         offsetof(V, fogHeightFalloff),
         0,
         10,
         {},
         "fog",
         "1/m",
         "How fast it thins with height: density falls to 1/e per 1/falloff metres up. 0: the same everywhere."},
        {"fog_base_height",
         T::Float,
         offsetof(V, fogBaseHeight),
         0,
         0,
         {},
         "fog",
         "m",
         "The height fog_density is measured at."},
        {"fog_max_density",
         T::Float,
         offsetof(V, fogMaxDensity),
         0,
         10,
         {},
         "fog",
         "1/m",
         "The most it may reach anywhere, deep below the base height too."},
        {"fog_color",
         T::Color3,
         offsetof(V, fogColor),
         0,
         1000,
         {},
         "fog",
         "linear",
         "The light it scatters from the surroundings: thick fog fades to this colour."},
        {"fog_albedo",
         T::Float,
         offsetof(V, fogAlbedo),
         0,
         1,
         {},
         "fog",
         "",
         "How much of the sun's and lamps' light it scatters toward the camera: 0 only its own colour."},
        {"fog_anisotropy",
         T::Float,
         offsetof(V, fogAnisotropy),
         -0.9f,
         0.9f,
         {},
         "fog",
         "",
         "Henyey-Greenstein g: above 0 the fog glows looking toward a light, below 0 looking away."},
        {"fog_start",
         T::Float,
         offsetof(V, fogStart),
         0,
         100000,
         {},
         "fog",
         "m",
         "Clear air between the camera and where the fog begins."},
        {"fog_sky_distance",
         T::Float,
         offsetof(V, fogSkyDistance),
         1,
         1000000,
         {},
         "fog",
         "m",
         "How far away the sky counts as."},
        {"fog_volumetric_distance",
         T::Float,
         offsetof(V, fogVolumetricDistance),
         1,
         1000,
         {},
         "fog",
         "m",
         "How far the lit fog volume reaches with fog_quality low, medium or high; the closed form past it."},
    };
    return params;
}

const PostProcessParamInfo* findPostProcessParam(std::string_view key) {
    for (const auto& p : postProcessParams())
        if (key == p.key) return &p;
    return nullptr;
}

namespace {

size_t paramIndex(std::string_view key) {
    const auto& params = postProcessParams();
    for (size_t i = 0; i < params.size(); ++i)
        if (key == params[i].key) return i;
    return params.size();
}

template <class T>
T& member(PostProcessValues& v, size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(&v) + offset);
}
template <class T>
const T& member(const PostProcessValues& v, size_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(&v) + offset);
}

int components(PostProcessValueType type) {
    return type == PostProcessValueType::Vec2 ? 2 : type == PostProcessValueType::Color3 ? 3 : 1;
}

// Words of a line, a quoted string kept whole (without its quotes).
bool splitLine(std::string_view line, std::vector<std::string>& out, std::string& error) {
    out.clear();
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
        // Exponents only where plain digits can't say it in 9 (60, not 6e+01).
        if (std::strtof(text, nullptr) == v &&
            (!std::strchr(text, 'e') || std::fabs(v) >= 1e9f || std::fabs(v) < 1e-4f))
            break;
    }
    return text;
}

AutoExposureDesc autoDesc(const PostProcessValues& v) {
    AutoExposureDesc d;
    d.compensation = v.autoCompensation;
    d.minEv = v.autoMinEv;
    d.maxEv = v.autoMaxEv;
    d.targetGray = v.autoTargetGray;
    d.lowPercentile = v.autoLowPercentile;
    d.highPercentile = v.autoHighPercentile;
    d.brightenTime = v.autoBrightenTime;
    d.darkenTime = v.autoDarkenTime;
    d.metering = ExposureMetering(std::clamp(v.autoMetering, 0, 1));
    d.resetOnCut = v.autoResetOnCut;
    return d;
}

FogDesc fogDesc(const PostProcessValues& v) {
    FogDesc f;
    f.density = v.fogDensity;
    f.heightFalloff = v.fogHeightFalloff;
    f.baseHeight = v.fogBaseHeight;
    f.maxDensity = v.fogMaxDensity;
    f.color = v.fogColor;
    f.albedo = v.fogAlbedo;
    f.anisotropy = v.fogAnisotropy;
    f.start = v.fogStart;
    f.skyDistance = v.fogSkyDistance;
    f.volumetricDistance = v.fogVolumetricDistance;
    return f;
}

bool readFile(const fs::path& path, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "can't open " + path.string();
        return false;
    }
    std::ostringstream s;
    s << in.rdbuf();
    text = s.str();
    return true;
}

fs::file_time_type modifiedTime(const fs::path& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type{} : t;
}

}   // namespace

bool PostProcessFile::isSet(std::string_view key) const {
    const size_t i = paramIndex(key);
    return i < postProcessParams().size() && (set >> i) & 1u;
}

bool isPostProcessFile(const fs::path& path) { return path.extension() == ".swpostprocess"; }

bool parsePostProcess(std::string_view text, PostProcessFile& file, std::string& error) {
    file = {};
    const auto& params = postProcessParams();
    int lineNumber = 0;
    bool header = false, parent = false;
    std::vector<std::string> w;
    auto fail = [&](const std::string& why) {
        error = "line " + std::to_string(lineNumber) + ": " + why;
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
        if (!splitLine(line, w, why)) return fail(why);
        if (w.empty()) {
            if (end == text.size()) break;
            continue;
        }
        if (!header) {
            if (w[0] != "swpostprocess") return fail("the first line must be \"swpostprocess 1\"");
            if (w.size() != 2 || w[1] != "1") return fail("only swpostprocess version 1 is known");
            header = true;
        } else if (w[0] == "parent") {
            if (parent) return fail("parent given twice");
            if (w.size() != 2 || w[1].empty()) return fail("parent needs one quoted .swpostprocess path");
            if (!isPostProcessFile(w[1])) return fail("a parent must be a .swpostprocess file");
            file.parent = w[1];
            parent = true;
        } else {
            const size_t index = paramIndex(w[0]);
            if (index >= params.size()) return fail("unknown key " + w[0]);
            if ((file.set >> index) & 1u) return fail(w[0] + " given twice");
            const PostProcessParamInfo& p = params[index];
            const size_t count = w.size() - 1;
            switch (p.type) {
            case PostProcessValueType::Bool:
                if (count != 1 || (w[1] != "true" && w[1] != "false")) return fail(w[0] + " must be true or false");
                member<bool>(file.values, p.offset) = w[1] == "true";
                break;
            case PostProcessValueType::Enum: {
                if (count != 1) return fail(w[0] + " needs one value");
                auto it = std::find_if(p.options.begin(), p.options.end(), [&](const char* o) { return w[1] == o; });
                if (it == p.options.end()) {
                    std::string list;
                    for (const char* o : p.options) list += (list.empty() ? "" : ", ") + std::string(o);
                    return fail(w[0] + " must be one of " + list);
                }
                member<int>(file.values, p.offset) = int(it - p.options.begin());
                break;
            }
            case PostProcessValueType::Path:
                if (count != 1) return fail(w[0] + " needs one quoted path");
                if (!w[1].empty() && fs::path(w[1]).extension() != ".cube") return fail("a LUT must be a .cube file");
                member<std::string>(file.values, p.offset) = w[1];
                break;
            default: {
                const int n = components(p.type);
                if (int(count) != n)
                    return fail(w[0] + " needs " + std::to_string(n) + (n == 1 ? " number" : " numbers"));
                float* out = &member<float>(file.values, p.offset);
                for (int i = 0; i < n; ++i) {
                    float v;
                    if (!number(w[1 + i], v)) return fail(w[0] + ": " + w[1 + i] + " isn't a number");
                    if (p.min != p.max && (v < p.min || v > p.max))
                        return fail(
                            w[0] + " must be " + formatFloat(p.min) + " to " + formatFloat(p.max) +
                            (p.unit[0] ? std::string(" ") + p.unit : std::string()));
                    out[i] = v;
                }
            }
            }
            file.set |= uint64_t(1) << index;
        }
        if (end == text.size()) break;
    }
    lineNumber = 0;
    if (!header) return fail("empty: the first line must be \"swpostprocess 1\"");
    // Whole-group checks, on what this file gives (defaults fill the rest).
    std::string why;
    if (file.isSet("white_point") && !validWhitePoint(file.values.whitePoint, &why)) {
        error = "white_point: " + why;
        return false;
    }
    if (!validAutoExposure(autoDesc(file.values), &why)) {
        error = why;
        return false;
    }
    if (!validFog(fogDesc(file.values), &why)) {
        error = why;
        return false;
    }
    return true;
}

std::string writePostProcess(const PostProcessFile& file) {
    std::string out = "swpostprocess 1\n";
    if (!file.parent.empty()) out += "parent \"" + file.parent + "\"\n";
    const auto& params = postProcessParams();
    for (size_t i = 0; i < params.size(); ++i) {
        if (!((file.set >> i) & 1u)) continue;
        const PostProcessParamInfo& p = params[i];
        out += p.key;
        switch (p.type) {
        case PostProcessValueType::Bool: out += member<bool>(file.values, p.offset) ? " true" : " false"; break;
        case PostProcessValueType::Enum:
            out += std::string(" ") +
                   p.options[size_t(std::clamp(member<int>(file.values, p.offset), 0, int(p.options.size()) - 1))];
            break;
        case PostProcessValueType::Path: out += " \"" + member<std::string>(file.values, p.offset) + "\""; break;
        default: {
            const float* v = &member<float>(file.values, p.offset);
            for (int c = 0; c < components(p.type); ++c) out += " " + formatFloat(v[c]);
        }
        }
        out += "\n";
    }
    return out;
}

std::string postProcessSchemaMarkdown() {
    std::string out =
        "# Postprocess profile schema\n\n"
        "Generated by `player --postprocess-schema`; edit Source/Runtime/Engine/Private/PostProcess.cpp, not this "
        "file.\n\n"
        "A `.swpostprocess` file starts with `swpostprocess 1`, may name a `parent \"other.swpostprocess\"`, then "
        "gives any of these keys once each. Keys it doesn't give come from its parent, then from the scene and the "
        "defaults below, which change nothing. `#` starts a comment.\n\n"
        "| Key | Group | Values | Default | Unit | What it does |\n|---|---|---|---|---|---|\n";
    const PostProcessValues defaults;
    for (const PostProcessParamInfo& p : postProcessParams()) {
        std::string values, def;
        switch (p.type) {
        case PostProcessValueType::Bool:
            values = "true, false";
            def = member<bool>(defaults, p.offset) ? "true" : "false";
            break;
        case PostProcessValueType::Enum:
            for (const char* o : p.options) values += (values.empty() ? "" : ", ") + std::string(o);
            def = p.options[size_t(member<int>(defaults, p.offset))];
            break;
        case PostProcessValueType::Path:
            values = "\"path.cube\"";
            def = "none";
            break;
        default: {
            const int n = components(p.type);
            values = p.min == p.max ? "any" : formatFloat(p.min) + " to " + formatFloat(p.max);
            if (n > 1) values = std::to_string(n) + " numbers, " + values;
            const float* v = &member<float>(defaults, p.offset);
            for (int c = 0; c < n; ++c) def += (c ? " " : "") + formatFloat(v[c]);
        }
        }
        out += std::string("| `") + p.key + "` | " + p.group + " | " + values + " | " + def + " | " + p.unit + " | " +
               p.description + " |\n";
    }
    out += "\nWhite points are checked as a whole (x > 0, y > 0, x + y < 1, at most 4x in any cone response); "
           "so are auto exposure (min below max, percentiles in order) and fog (no negative density, anisotropy "
           "within 0.9).\n";
    return out;
}

// ---- The library ----

struct PostProcessLibrary::Entry {
    fs::file_time_type modified{};
    bool loaded = false;
    std::string error;   // why the last load failed
    std::shared_ptr<const PostProcessFile> file;   // profiles
    std::shared_ptr<const LoadedLut> lut;   // LUTs
    std::shared_ptr<const Resolved> resolved;   // cached until anything reloads
};

PostProcessLibrary::PostProcessLibrary(Renderer* renderer) : m_renderer(renderer) {}

PostProcessLibrary::~PostProcessLibrary() {
    if (!m_renderer) return;
    for (auto& [name, e] : m_entries)
        if (e->lut && e->lut->texture.id) m_renderer->destroyTexture(e->lut->texture);
}

void PostProcessLibrary::setContentFolder(const fs::path& folder) {
    if (folder == m_folder) return;
    if (m_renderer)
        for (auto& [name, e] : m_entries)
            if (e->lut && e->lut->texture.id) m_renderer->destroyTexture(e->lut->texture);
    m_entries.clear();
    m_folder = folder;
}

PostProcessLibrary::Entry& PostProcessLibrary::entry(std::string_view name) {
    auto it = m_entries.find(name);
    if (it == m_entries.end()) it = m_entries.emplace(std::string(name), std::make_unique<Entry>()).first;
    Entry& e = *it->second;
    if (e.loaded) return e;
    e.loaded = true;
    e.error.clear();
    const fs::path path = m_folder / fs::path(std::string(name));
    e.modified = modifiedTime(path);
    std::string text;
    if (!readFile(path, text, e.error)) {
        e.error = "missing: " + std::string(name);
        e.file.reset();
        return e;
    }
    if (isPostProcessFile(path)) {
        auto file = std::make_shared<PostProcessFile>();
        if (parsePostProcess(text, *file, e.error)) {
            e.file = std::move(file);
        } else {
            e.error = std::string(name) + ": " + e.error;
            // A file that stops parsing keeps its last good version.
            if (e.file) SW_LOG(LogRender, Warning, "{} (keeping the last version that parsed)", e.error);
        }
    } else {
        auto lut = std::make_shared<LoadedLut>();
        if (parseCubeLut(text, lut->lut, e.error)) {
            if (m_renderer) {
                // A renderer that can't hold it (the null renderer, tools) keeps the parsed
                // table without a texture: the grade is skipped, the file still checks.
                try {
                    lut->texture = m_renderer->createTexture(lutTexture(lut->lut));
                } catch (const std::exception& ex) {
                    SW_LOG(LogRender, Warning, "{}: no texture for it: {}", name, ex.what());
                }
                if (e.lut && e.lut->texture.id) m_renderer->destroyTexture(e.lut->texture);
            }
            e.lut = std::move(lut);
        } else {
            e.error = std::string(name) + ": " + e.error;
            if (e.lut) SW_LOG(LogRender, Warning, "{} (keeping the last version that parsed)", e.error);
        }
    }
    return e;
}

std::shared_ptr<const PostProcessLibrary::Resolved>
PostProcessLibrary::resolve(std::string_view name, int depth, std::vector<std::string>& seen, std::string& error) {
    if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
        error = "parents loop: " + std::string(name) + " is its own ancestor";
        return nullptr;
    }
    if (depth >= kMaxPostProcessDepth) {
        error = "more than 8 profiles in a parent chain";
        return nullptr;
    }
    Entry& e = entry(name);
    if (!e.file) {
        error = e.error.empty() ? "can't read " + std::string(name) : e.error;
        return nullptr;
    }
    seen.emplace_back(name);
    auto out = std::make_shared<Resolved>();
    if (!e.file->parent.empty()) {
        auto parent = resolve(e.file->parent, depth + 1, seen, error);
        if (!parent) {
            error = std::string(name) + ": parent " + e.file->parent + ": " + error;
            return nullptr;
        }
        *out = *parent;
    }
    out->chain.insert(out->chain.begin(), std::string(name));
    const auto& params = postProcessParams();
    for (size_t i = 0; i < params.size(); ++i) {
        if (!((e.file->set >> i) & 1u)) continue;
        const PostProcessParamInfo& p = params[i];
        switch (p.type) {
        case PostProcessValueType::Bool:
            member<bool>(out->values, p.offset) = member<bool>(e.file->values, p.offset);
            break;
        case PostProcessValueType::Enum:
            member<int>(out->values, p.offset) = member<int>(e.file->values, p.offset);
            break;
        case PostProcessValueType::Path:
            member<std::string>(out->values, p.offset) = member<std::string>(e.file->values, p.offset);
            break;
        default:
            for (int c = 0; c < components(p.type); ++c)
                (&member<float>(out->values, p.offset))[c] = (&member<float>(e.file->values, p.offset))[c];
        }
    }
    out->set |= e.file->set;
    // Chained limits are checked once more, as a whole.
    std::string why;
    if (!validAutoExposure(autoDesc(out->values), &why) || !validFog(fogDesc(out->values), &why)) {
        error = std::string(name) + " with its parents: " + why;
        return nullptr;
    }
    return out;
}

std::shared_ptr<const PostProcessLibrary::Resolved>
PostProcessLibrary::profile(std::string_view name, std::string* error) {
    Entry& e = entry(name);
    if (e.resolved) return e.resolved;
    std::vector<std::string> seen;
    std::string why;
    e.resolved = resolve(name, 0, seen, why);
    if (!e.resolved && error) *error = why;
    return e.resolved;
}

std::shared_ptr<const PostProcessLibrary::LoadedLut>
PostProcessLibrary::lut(std::string_view name, std::string* error) {
    Entry& e = entry(name);
    if (!e.lut && error) *error = e.error;
    return e.lut;
}

bool PostProcessLibrary::poll(float dt) {
    m_sincePoll += dt;
    if (m_sincePoll < 0.5f) return false;
    m_sincePoll = 0.0f;
    bool changed = false;
    for (auto& [name, e] : m_entries) {
        if (modifiedTime(m_folder / fs::path(name)) == e->modified) continue;
        e->loaded = false;
        changed = true;
        SW_LOG(LogRender, Info, "Reloading {}", name);
    }
    if (!changed) return false;
    for (auto& [name, e] : m_entries) {
        e->resolved.reset();   // a parent may have changed
        if (!e->loaded) entry(name);
    }
    return true;
}

std::vector<std::string> PostProcessLibrary::profileFiles() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (m_folder.empty() || !fs::is_directory(m_folder, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(m_folder, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && isPostProcessFile(it->path()))
            out.push_back(fs::relative(it->path(), m_folder, ec).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- Blending ----

float postProcessVolumeWeight(const PostProcessVolume& volume, const glm::mat4& transform, const glm::vec3& point) {
    if (!volume.enabled || !(volume.weight > 0.0f)) return 0.0f;
    const float weight = std::min(volume.weight, 1.0f);
    if (volume.unbounded) return weight;
    // Distance inside the nearest face, in world metres: each axis's half size along its
    // own world direction (any scale, rotation kept).
    const glm::vec3 offset = point - glm::vec3(transform[3]);
    float inside = 1e30f;
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 axis(transform[i]);
        const float scale = glm::length(axis);
        if (!(scale > 1e-8f)) return 0.0f;
        const float half = std::abs(volume.halfExtents[i]) * scale;
        inside = std::min(inside, half - std::abs(glm::dot(offset, axis / scale)));
    }
    if (inside < 0.0f) return 0.0f;
    if (!(volume.blendDistance > 0.0f)) return weight;
    const float t = std::clamp(inside / volume.blendDistance, 0.0f, 1.0f);
    return weight * t * t * (3.0f - 2.0f * t);
}

PostProcessResult evaluatePostProcess(
    const World& world, const glm::vec3& camera, PostProcessLibrary& library, const Environment& environment) {
    PostProcessResult result;
    result.values.exposure = environment.exposure;
    result.values.bloomIntensity = std::clamp(environment.bloomIntensity, 0.0f, 1.0f);
    const Pool<PostProcessVolume>* pool = world.pool<PostProcessVolume>();
    if (!pool || pool->size() == 0) return result;

    struct Active {
        Entity entity;
        const PostProcessVolume* volume;
        std::shared_ptr<const PostProcessLibrary::Resolved> profile;
        float weight;
    };
    std::vector<Active> active;
    for (size_t i = 0; i < pool->size(); ++i) {
        const Entity e = pool->entities()[i];
        const PostProcessVolume& v = pool->components()[i];
        if (!v.enabled || v.profile.empty()) continue;
        if (!v.unbounded && !world.has<Transform>(e)) {
            result.problems.push_back("a bounded PostProcessVolume needs a Transform");
            continue;
        }
        const float w = postProcessVolumeWeight(v, v.unbounded ? glm::mat4(1.0f) : worldMatrix(world, e), camera);
        if (!(w > 0.0f)) continue;
        std::string error;
        auto profile = library.profile(v.profile, &error);
        if (!profile) {
            result.problems.push_back(error);
            continue;
        }
        active.push_back({e, &v, std::move(profile), w});
        result.volumes.push_back({e, v.profile, v.priority, w});
    }
    // Lowest priority first; within one, a stable order by entity.
    std::sort(active.begin(), active.end(), [](const Active& a, const Active& b) {
        if (a.volume->priority != b.volume->priority) return a.volume->priority < b.volume->priority;
        return a.entity.index < b.entity.index;
    });
    const auto& params = postProcessParams();
    PostProcessValues& out = result.values;
    for (size_t start = 0; start < active.size();) {
        size_t end = start;
        float total = 0.0f;
        while (end < active.size() && active[end].volume->priority == active[start].volume->priority)
            total += active[end++].weight;
        const float coverage = std::min(total, 1.0f);
        const PostProcessValues lower = out;
        for (size_t i = 0; i < params.size(); ++i) {
            const PostProcessParamInfo& p = params[i];
            const uint64_t bit = uint64_t(1) << i;
            if (p.type == PostProcessValueType::Path || p.key == std::string_view("lut_encoding") ||
                p.key == std::string_view("lut_intensity"))
                continue;   // LUT layers, below
            if (p.type == PostProcessValueType::Float || p.type == PostProcessValueType::Vec2 ||
                p.type == PostProcessValueType::Color3) {
                const int n = components(p.type);
                for (int c = 0; c < n; ++c) {
                    const float base = (&member<float>(lower, p.offset))[c];
                    float sum = 0.0f;
                    for (size_t k = start; k < end; ++k)
                        sum += active[k].weight * ((active[k].profile->set & bit)
                                                       ? (&member<float>(active[k].profile->values, p.offset))[c]
                                                       : base);
                    (&member<float>(out, p.offset))[c] = base + (sum / total - base) * coverage;
                }
                continue;
            }
            if (p.key == std::string_view("auto_exposure")) continue;   // a share, below
            // Discrete choices: the heaviest volume that sets it, once its group's setters
            // cover at least half.
            float setters = 0.0f;
            const Active* heaviest = nullptr;
            for (size_t k = start; k < end; ++k)
                if (active[k].profile->set & bit) {
                    setters += active[k].weight;
                    if (!heaviest || active[k].weight > heaviest->weight) heaviest = &active[k];
                }
            if (heaviest && setters / total * coverage >= 0.5f) {
                if (p.type == PostProcessValueType::Bool)
                    member<bool>(out, p.offset) = member<bool>(heaviest->profile->values, p.offset);
                else
                    member<int>(out, p.offset) = member<int>(heaviest->profile->values, p.offset);
            }
        }
        // Auto exposure as a share in stops: metered and manual exposure blend smoothly.
        {
            const size_t index = paramIndex("auto_exposure");
            float sum = 0.0f;
            for (size_t k = start; k < end; ++k) {
                const bool set = (active[k].profile->set >> index) & 1u;
                sum += active[k].weight *
                       (set ? (active[k].profile->values.autoExposure ? 1.0f : 0.0f) : result.autoExposure);
            }
            result.autoExposure += (sum / total - result.autoExposure) * coverage;
        }
        // LUTs: what's below keeps the share the group's LUT setters don't take.
        {
            const size_t index = paramIndex("lut");
            float share = 0.0f;
            for (size_t k = start; k < end; ++k)
                if ((active[k].profile->set >> index) & 1u) share += active[k].weight / total * coverage;
            for (auto& layer : result.luts) layer.weight *= 1.0f - share;
            for (size_t k = start; k < end; ++k) {
                if (!((active[k].profile->set >> index) & 1u) || active[k].profile->values.lut.empty()) continue;
                const PostProcessValues& v = active[k].profile->values;
                const LutEncoding encoding = LutEncoding(std::clamp(v.lutEncoding, 0, 1));
                const float weight = active[k].weight / total * coverage * std::clamp(v.lutIntensity, 0.0f, 1.0f);
                auto same =
                    std::find_if(result.luts.begin(), result.luts.end(), [&](const PostProcessResult::Layer& l) {
                        return l.lut == v.lut && l.encoding == encoding;
                    });
                if (same != result.luts.end())
                    same->weight += weight;
                else
                    result.luts.push_back({v.lut, encoding, weight});
            }
        }
        start = end;
    }
    std::erase_if(result.luts, [](const PostProcessResult::Layer& l) { return !(l.weight > 1e-6f); });
    std::stable_sort(
        result.luts.begin(), result.luts.end(),
        [](const PostProcessResult::Layer& a, const PostProcessResult::Layer& b) { return a.weight > b.weight; });
    out.autoExposure = result.autoExposure > 0.0f;
    return result;
}

PostProcessDesc postProcessDesc(const PostProcessResult& result, PostProcessLibrary& library, float dt) {
    PostProcessDesc desc;
    const PostProcessValues& v = result.values;
    desc.overrideExposure = true;
    desc.exposure = v.exposure;
    desc.autoExposure = std::clamp(result.autoExposure, 0.0f, 1.0f);
    desc.autoParams = autoDesc(v);
    desc.whitePoint = validWhitePoint(v.whitePoint) ? v.whitePoint : kD65White;
    desc.overrideBloom = true;
    desc.bloomIntensity = std::clamp(v.bloomIntensity, 0.0f, 1.0f);
    desc.fog = fogDesc(v);
    if (!validFog(desc.fog)) desc.fog = {};
    desc.deltaTime = dt;
    float total = 0.0f, kept = 0.0f;
    for (const auto& layer : result.luts) {
        total += layer.weight;
        if (desc.lutCount == kMaxActiveLuts) {
            desc.lutsApproximated = true;
            continue;
        }
        auto lut = library.lut(layer.lut);
        if (!lut || !lut->texture.id) continue;
        desc.luts[desc.lutCount++] = {lut->texture, lut->lut.size, layer.encoding, layer.weight};
        kept += layer.weight;
    }
    // Past two, the two largest keep the whole graded share between them.
    if (desc.lutsApproximated && kept > 0.0f)
        for (uint32_t i = 0; i < desc.lutCount; ++i) desc.luts[i].weight *= std::min(total, 1.0f) / kept;
    return desc;
}

void updatePostProcess(const World& world, Renderer& renderer, PostProcessLibrary& library, float dt) {
    library.poll(dt);
    Environment environment;
    if (const Pool<Environment>* environments = world.pool<Environment>(); environments && environments->size())
        environment = environments->components()[0];
    const glm::vec3 camera = glm::vec3(glm::inverse(renderer.cameraView())[3]);
    const PostProcessResult result = evaluatePostProcess(world, camera, library, environment);
    static std::unordered_set<std::string> reported;
    for (const std::string& problem : result.problems)
        if (reported.insert(problem).second) SW_LOG(LogRender, Warning, "PostProcessVolume: {}", problem);
    for (const auto& layer : result.luts) {
        std::string error;
        if (!library.lut(layer.lut, &error) && reported.insert(error).second)
            SW_LOG(LogRender, Warning, "PostProcessVolume LUT {}", error);
    }
    renderer.setPostProcess(postProcessDesc(result, library, dt));
}

}   // namespace shockwave
