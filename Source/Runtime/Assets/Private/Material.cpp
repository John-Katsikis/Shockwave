#include <Assets/Material.h>

#include <Assets/Image.h>
#include <Assets/TextureCompression.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

// ---- Schema ----

namespace {
#define SW_PARAM(member) offsetof(MaterialParams, member)
const std::vector<MaterialParamInfo> kParams = {
    {"family", MaterialValueType::Enum, SW_PARAM(family), 0, 0, {"standard"}, "surface",
     "Shading family. standard is glTF's metallic-roughness; coat, skin and hair come later"},
    {"base_color", MaterialValueType::Color4, SW_PARAM(baseColor), 0, 1, {}, "surface",
     "Linear RGB and alpha, multiplying the base_color texture"},
    {"metallic", MaterialValueType::Float, SW_PARAM(metallic), 0, 1, {}, "surface",
     "0 plastic, wood, stone .. 1 bare metal; multiplies the metallic_roughness texture's blue"},
    {"roughness", MaterialValueType::Float, SW_PARAM(roughness), 0, 1, {}, "surface",
     "Perceptual roughness (not smoothness): 0 mirror .. 1 matte; multiplies the texture's green. 0 is kept as "
     "written; the shader never goes below 0.03"},
    {"normal_scale", MaterialValueType::Float, SW_PARAM(normalScale), 0, 4, {}, "surface",
     "How strongly the normal map tilts the surface"},
    {"occlusion_strength", MaterialValueType::Float, SW_PARAM(occlusionStrength), 0, 1, {}, "surface",
     "0 ignores the occlusion map .. 1 full"},
    {"emissive", MaterialValueType::Color3, SW_PARAM(emissive), 0, kMaxEmissive, {}, "surface",
     "Light the surface gives off, linear RGB (above 1 for bright glows); multiplies the emissive texture"},
    {"alpha_mode", MaterialValueType::Enum, SW_PARAM(alphaMode), 0, 0, {"opaque", "mask", "blend"}, "alpha",
     "opaque ignores alpha; mask cuts out below alpha_cutoff (in every pass, shadows included); blend is see-through"},
    {"alpha_cutoff", MaterialValueType::Float, SW_PARAM(alphaCutoff), 0, 1, {}, "alpha", "mask's threshold"},
    {"double_sided", MaterialValueType::Bool, SW_PARAM(doubleSided), 0, 0, {}, "alpha",
     "Draw back faces too (leaves, cloth, glass panes)"},
    {"detail_strength", MaterialValueType::Float, SW_PARAM(detailStrength), 0, 1, {}, "detail",
     "How much of the detail layer shows, times detail_mask and the fade"},
    {"detail_normal_scale", MaterialValueType::Float, SW_PARAM(detailNormalScale), 0, 4, {}, "detail",
     "How strongly the detail normal map tilts the surface"},
    {"detail_roughness", MaterialValueType::Float, SW_PARAM(detailRoughness), 0, 1, {}, "detail",
     "The roughness the detail layer moves the surface towards..."},
    {"detail_roughness_strength", MaterialValueType::Float, SW_PARAM(detailRoughnessStrength), 0, 1, {}, "detail",
     "...and how far (0 leaves roughness alone)"},
    {"detail_mask_channel", MaterialValueType::Enum, SW_PARAM(detailMaskChannel), 0, 0, {"r", "g", "b", "a"},
     "detail", "Which channel of the detail_mask texture limits the detail"},
    {"detail_fade", MaterialValueType::Vec2, SW_PARAM(detailFade), 0, 100000, {}, "detail",
     "Metres from the camera where the detail starts and finishes fading out, smoothly; 0 0 never fades"},
    {"layer_mask", MaterialValueType::Enum, SW_PARAM(layerMask), 0, 0,
     {"texture_r", "texture_g", "texture_b", "texture_a", "vertex_r", "vertex_g", "vertex_b"}, "layer",
     "Where the second layer shows: a channel of the layer_mask texture, or a colour channel of the vertices (whose "
     "colours then don't tint the surface)"},
    {"layer_base_color", MaterialValueType::Color4, SW_PARAM(layerBaseColor), 0, 1, {}, "layer",
     "The second layer's linear RGB (alpha unused: cut-outs follow the first layer)"},
    {"layer_metallic", MaterialValueType::Float, SW_PARAM(layerMetallic), 0, 1, {}, "layer", "As metallic"},
    {"layer_roughness", MaterialValueType::Float, SW_PARAM(layerRoughness), 0, 1, {}, "layer", "As roughness"},
    {"layer_normal_scale", MaterialValueType::Float, SW_PARAM(layerNormalScale), 0, 4, {}, "layer",
     "As normal_scale"},
};
#undef SW_PARAM
static_assert(sizeof(int) == 4);

const std::array<MaterialSlotInfo, kAllMaterialSlots> kSlots = {{
    {MaterialSlot::BaseColor, true, false, "surface", "Colour and alpha"},
    {MaterialSlot::Normal, false, true, "surface", "Tangent-space normals, green up"},
    {MaterialSlot::MetallicRoughness, false, false, "surface", "Green roughness, blue metallic (glTF's packing)"},
    {MaterialSlot::Emissive, true, false, "surface", "Emitted colour"},
    {MaterialSlot::Occlusion, false, false, "surface", "Red darkens ambient light only"},
    {MaterialSlot::DetailColor, true, false, "detail",
     "Multiplies the base colour around neutral white: 1 1 1 changes nothing"},
    {MaterialSlot::DetailNormal, false, true, "detail", "Fine normals composed on top"},
    {MaterialSlot::DetailMask, false, false, "detail", "Where the detail shows, from detail_mask_channel"},
    {MaterialSlot::LayerMask, false, false, "layer", "Where the second layer shows, from the layer_mask value"},
    {MaterialSlot::LayerBaseColor, true, false, "layer", "The second layer's colour"},
    {MaterialSlot::LayerNormal, false, true, "layer", "The second layer's normals"},
    {MaterialSlot::LayerMetallicRoughness, false, false, "layer", "The second layer's roughness and metallic"},
}};

template <class T>
T& member(MaterialParams& p, const MaterialParamInfo& info) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(&p) + info.offset);
}
template <class T>
const T& member(const MaterialParams& p, const MaterialParamInfo& info) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const char*>(&p) + info.offset);
}
int componentCount(MaterialValueType type) {
    switch (type) {
    case MaterialValueType::Vec2: return 2;
    case MaterialValueType::Color3: return 3;
    case MaterialValueType::Color4: return 4;
    default: return 1;
    }
}
}   // namespace

const std::vector<MaterialParamInfo>& materialParams() { return kParams; }
const MaterialParamInfo* findMaterialParam(std::string_view key) {
    for (const auto& info : kParams)
        if (key == info.key) return &info;
    return nullptr;
}
const MaterialSlotInfo& materialSlotInfo(MaterialSlot slot) {
    return kSlots[std::min<size_t>(size_t(slot), kAllMaterialSlots - 1)];
}
bool parseMaterialSlot(std::string_view name, MaterialSlot& slot) {
    for (size_t i = 0; i < kAllMaterialSlots; ++i)
        if (name == materialSlotName(MaterialSlot(i))) {
            slot = MaterialSlot(i);
            return true;
        }
    return false;
}

bool MaterialFile::isSet(std::string_view key) const {
    for (size_t i = 0; i < kParams.size(); ++i)
        if (key == kParams[i].key) return (set >> i) & 1u;
    return false;
}
void MaterialFile::setValue(std::string_view key, bool on) {
    for (size_t i = 0; i < kParams.size(); ++i)
        if (key == kParams[i].key) set = on ? set | (1ull << i) : set & ~(1ull << i);
}

bool isMaterialFile(const fs::path& path) { return path.extension() == ".swmaterial"; }

// ---- Parsing ----

namespace {
struct Token {
    std::string text;
    bool quoted = false;
};
// Words and "quoted strings" (\" and \\ escaped); # starts a comment outside quotes.
bool tokenize(std::string_view line, std::vector<Token>& tokens, std::string& error) {
    tokens.clear();
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '#') break;
        Token token;
        if (c == '"') {
            token.quoted = true;
            bool closed = false;
            for (++i; i < line.size(); ++i) {
                if (line[i] == '\\') {
                    if (i + 1 >= line.size() || (line[i + 1] != '"' && line[i + 1] != '\\')) {
                        error = "only \\\" and \\\\ can be escaped in a string";
                        return false;
                    }
                    token.text += line[++i];
                } else if (line[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                } else if (line[i] == '\0') {
                    error = "a string can't hold a NUL character";
                    return false;
                } else {
                    token.text += line[i];
                }
            }
            if (!closed) {
                error = "a string isn't closed with \"";
                return false;
            }
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '#' &&
                   line[i] != '"')
                token.text += line[i++];
        }
        tokens.push_back(std::move(token));
    }
    return true;
}
bool parseNumber(const Token& token, float& value) {
    if (token.quoted || token.text.empty()) return false;
    const char* begin = token.text.data();
    if (*begin == '+') ++begin;   // from_chars doesn't take a leading +
    const auto [end, ec] = std::from_chars(begin, token.text.data() + token.text.size(), value);
    return ec == std::errc{} && end == token.text.data() + token.text.size() && std::isfinite(value);
}
// Asset names inside Content: forward slashes, relative, never climbing out.
bool validAssetName(std::string_view name, std::string& error) {
    if (name.empty()) {
        error = "an empty path";
        return false;
    }
    if (name.find('\\') != std::string_view::npos || name.front() == '/' ||
        (name.size() > 1 && name[1] == ':')) {
        error = "'" + std::string(name) + "' must be a path inside Content with forward slashes";
        return false;
    }
    for (const auto& part : fs::path(std::string(name)))
        if (part == "..") {
            error = "'" + std::string(name) + "' climbs out of Content (..)";
            return false;
        }
    return true;
}
const std::vector<const char*> kWraps = {"repeat", "clamp", "mirror"};
const std::vector<const char*> kFilters = {"linear", "nearest"};
const std::vector<const char*> kMipFilters = {"none", "nearest", "linear"};
int optionIndex(const std::vector<const char*>& options, const Token& token) {
    if (token.quoted) return -1;
    for (size_t i = 0; i < options.size(); ++i)
        if (token.text == options[i]) return int(i);
    return -1;
}
std::string optionList(const std::vector<const char*>& options) {
    std::string text;
    for (size_t i = 0; i < options.size(); ++i)
        text += std::string(i == 0 ? "" : i + 1 == options.size() ? " or " : ", ") + options[i];
    return text;
}
}   // namespace

bool parseMaterial(std::string_view text, MaterialFile& out, std::string& error) {
    error.clear();
    MaterialFile file;
    bool versionSeen = false, parentSeen = false;
    std::set<std::string> records;   // "key" or "key slot", for duplicates
    std::vector<Token> tokens;
    size_t number = 0;
    for (size_t start = 0; start <= text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        ++number;
        auto fail = [&](const std::string& message) {
            error = "line " + std::to_string(number) + ": " + message;
            return false;
        };
        std::string problem;
        if (!tokenize(line, tokens, problem)) return fail(problem);
        if (tokens.empty()) {
            if (end == text.size()) break;
            continue;
        }
        const Token& key = tokens[0];
        if (key.quoted) return fail("a record starts with its key, not a string");
        auto expect = [&](size_t count, const char* syntax) {
            if (tokens.size() == count) return true;
            fail(std::string(tokens.size() < count ? "too few values" : "unexpected '" + tokens[count].text + "'") +
                 "; expected " + syntax);
            return false;
        };
        if (!versionSeen) {
            float version = 0;
            if (key.text != "swmaterial") return fail("expected swmaterial 1 first");
            if (tokens.size() != 2 || !parseNumber(tokens[1], version) || version != 1.0f)
                return fail("unsupported version; this engine reads swmaterial 1");
            versionSeen = true;
            continue;
        }
        if (key.text == "swmaterial") return fail("swmaterial is given twice");
        if (key.text == "inherit")
            return fail("inherit isn't stored: to inherit a value, delete its record (the editor's Inherit does that)");
        if (key.text == "parent") {
            if (parentSeen) return fail("duplicate record 'parent'");
            parentSeen = true;
            if (tokens.size() < 2 || tokens.size() > 3 || !tokens[1].quoted ||
                (tokens.size() == 3 && !tokens[2].quoted))
                return fail("expected parent \"path.swmaterial\", or parent \"model path\" \"material name\"");
            if (!validAssetName(tokens[1].text, problem)) return fail(problem);
            file.parent = tokens[1].text;
            if (tokens.size() == 3) {
                if (isMaterialFile(file.parent)) return fail("a .swmaterial parent takes no material name");
                if (tokens[2].text.empty()) return fail("the source material's name is empty");
                file.parentMaterial = tokens[2].text;
            } else if (!isMaterialFile(file.parent)) {
                return fail("a model parent needs the material's name too: parent \"" + file.parent +
                            "\" \"Material name\"");
            }
            continue;
        }
        if (const MaterialParamInfo* info = findMaterialParam(key.text)) {
            if (!records.insert(key.text).second) return fail("duplicate record '" + key.text + "'");
            const size_t index = size_t(info - kParams.data());
            file.set |= 1ull << index;
            switch (info->type) {
            case MaterialValueType::Bool: {
                if (!expect(2, "true or false")) return false;
                if (tokens[1].quoted || (tokens[1].text != "true" && tokens[1].text != "false"))
                    return fail(key.text + " is true or false");
                member<bool>(file.values, *info) = tokens[1].text == "true";
                break;
            }
            case MaterialValueType::Enum: {
                if (!expect(2, optionList(info->options).c_str())) return false;
                const int value = optionIndex(info->options, tokens[1]);
                if (value < 0) {
                    if (key.text == "family" && !tokens[1].quoted)
                        return fail("family " + tokens[1].text + " isn't supported yet; only standard is");
                    return fail(key.text + " is " + optionList(info->options));
                }
                member<int>(file.values, *info) = value;
                break;
            }
            default: {
                const int count = componentCount(info->type);
                const std::string syntax = count == 1 ? "a number" : std::to_string(count) + " numbers";
                if (!expect(size_t(count) + 1, syntax.c_str())) return false;
                float* values = &member<float>(file.values, *info);
                for (int c = 0; c < count; ++c) {
                    float v = 0;
                    if (!parseNumber(tokens[size_t(c) + 1], v))
                        return fail("'" + tokens[size_t(c) + 1].text + "' isn't a finite number");
                    if (info->min < info->max && (v < info->min || v > info->max)) {
                        std::ostringstream range;
                        range << key.text << " must be within " << info->min << ".." << info->max;
                        return fail(range.str());
                    }
                    values[c] = v;
                }
                if (key.text == "detail_fade" && values[1] != 0.0f && values[1] < values[0])
                    return fail("detail_fade's end must be at least its start (or 0 for no fade)");
            }
            }
            continue;
        }
        // Slot records: texture, clear_texture, uv_set, uv_transform, sampler.
        const bool slotRecord = key.text == "texture" || key.text == "clear_texture" || key.text == "uv_set" ||
                                key.text == "uv_transform" || key.text == "sampler";
        if (!slotRecord) return fail("unknown key '" + key.text + "'");
        MaterialSlot slot{};
        if (tokens.size() < 2 || tokens[1].quoted || !parseMaterialSlot(tokens[1].text, slot))
            return fail(key.text + " needs a slot: base_color, normal, metallic_roughness, emissive, occlusion, "
                                   "detail_color, detail_normal, detail_mask, layer_mask, layer_base_color, "
                                   "layer_normal or layer_metallic_roughness");
        const std::string kind = key.text == "clear_texture" ? "texture" : key.text;
        if (!records.insert(kind + " " + tokens[1].text).second)
            return fail(key.text == "texture" || key.text == "clear_texture"
                            ? "slot " + tokens[1].text + " has more than one texture or clear_texture record"
                            : "duplicate record '" + key.text + " " + tokens[1].text + "'");
        MaterialSlotOverride& o = file.slots[size_t(slot)];
        if (key.text == "texture") {
            if (!expect(3, "texture <slot> \"path\"")) return false;
            if (!tokens[2].quoted) return fail("the texture's path is a quoted string");
            if (tokens[2].text.empty())
                return fail("an empty texture path; use clear_texture " + tokens[1].text + " to remove one");
            if (!validAssetName(tokens[2].text, problem)) return fail(problem);
            o.hasTexture = true;
            o.texture = tokens[2].text;
        } else if (key.text == "clear_texture") {
            if (!expect(2, "clear_texture <slot>")) return false;
            o.hasTexture = true;
            o.texture.clear();
        } else if (key.text == "uv_set") {
            float set = 0;
            if (!expect(3, "uv_set <slot> 0 or 1")) return false;
            if (!parseNumber(tokens[2], set) || (set != 0.0f && set != 1.0f)) return fail("uv_set is 0 or 1");
            o.hasUvSet = true;
            o.uvSet = uint32_t(set);
        } else if (key.text == "uv_transform") {
            if (!expect(7, "uv_transform <slot> offsetU offsetV scaleU scaleV rotationRadians")) return false;
            float v[5];
            for (int c = 0; c < 5; ++c)
                if (!parseNumber(tokens[size_t(c) + 2], v[c]))
                    return fail("'" + tokens[size_t(c) + 2].text + "' isn't a finite number");
            o.hasTransform = true;
            o.transform.offset = {v[0], v[1]};
            o.transform.scale = {v[2], v[3]};
            o.transform.rotation = v[4];
        } else {
            if (!expect(8, "sampler <slot> wrapU wrapV minFilter magFilter mipFilter anisotropy")) return false;
            const int wrapU = optionIndex(kWraps, tokens[2]), wrapV = optionIndex(kWraps, tokens[3]);
            const int minFilter = optionIndex(kFilters, tokens[4]), magFilter = optionIndex(kFilters, tokens[5]);
            const int mip = optionIndex(kMipFilters, tokens[6]);
            float anisotropy = 0;
            if (wrapU < 0 || wrapV < 0) return fail("wrap is " + optionList(kWraps));
            if (minFilter < 0 || magFilter < 0) return fail("min and mag filters are linear or nearest");
            if (mip < 0) return fail("the mip filter is " + optionList(kMipFilters));
            if (!parseNumber(tokens[7], anisotropy) || anisotropy < 1 || anisotropy > 16 ||
                anisotropy != std::floor(anisotropy))
                return fail("anisotropy is a whole number 1..16");
            o.hasSampler = true;
            o.sampler.wrapU = TextureWrap(wrapU);
            o.sampler.wrapV = TextureWrap(wrapV);
            o.sampler.minFilter = TextureFilter(minFilter);
            o.sampler.magFilter = TextureFilter(magFilter);
            o.sampler.mipmaps = mip != 0;
            o.sampler.mipFilter = mip == 1 ? TextureFilter::Nearest : TextureFilter::Linear;
            o.sampler.maxAnisotropy = uint8_t(anisotropy);
        }
        if (end == text.size()) break;
    }
    if (!versionSeen) {
        error = "line 1: expected swmaterial 1 first";
        return false;
    }
    out = std::move(file);
    return true;
}

namespace {
std::string number(float value) {
    char text[32];
    const auto [end, ec] = std::to_chars(text, text + sizeof(text), value);
    return ec == std::errc{} ? std::string(text, end) : "0";
}
std::string quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}
}   // namespace

std::string writeMaterial(const MaterialFile& file) {
    std::string out = "swmaterial 1\n";
    if (!file.parent.empty())
        out += "parent " + quote(file.parent) + (file.parentMaterial.empty() ? "" : " " + quote(file.parentMaterial)) +
               "\n";
    for (size_t i = 0; i < kParams.size(); ++i) {
        if (!((file.set >> i) & 1u)) continue;
        const MaterialParamInfo& info = kParams[i];
        out += info.key;
        switch (info.type) {
        case MaterialValueType::Bool: out += member<bool>(file.values, info) ? " true" : " false"; break;
        case MaterialValueType::Enum: {
            const int value = member<int>(file.values, info);
            out += " ";
            out += info.options[size_t(std::clamp(value, 0, int(info.options.size()) - 1))];
            break;
        }
        default: {
            const float* values = &member<float>(file.values, info);
            for (int c = 0; c < componentCount(info.type); ++c) out += " " + number(values[c]);
        }
        }
        out += "\n";
    }
    for (size_t s = 0; s < kAllMaterialSlots; ++s) {
        const MaterialSlotOverride& o = file.slots[s];
        const std::string slot = materialSlotName(MaterialSlot(s));
        if (o.hasTexture)
            out += o.texture.empty() ? "clear_texture " + slot + "\n" : "texture " + slot + " " + quote(o.texture) + "\n";
        if (o.hasUvSet) out += "uv_set " + slot + " " + std::to_string(o.uvSet) + "\n";
        if (o.hasTransform) {
            const UvTransform& t = o.transform;
            out += "uv_transform " + slot + " " + number(t.offset.x) + " " + number(t.offset.y) + " " +
                   number(t.scale.x) + " " + number(t.scale.y) + " " + number(t.rotation) + "\n";
        }
        if (o.hasSampler) {
            const SamplerDesc& d = o.sampler;
            out += "sampler " + slot + " " + kWraps[size_t(d.wrapU)] + " " + kWraps[size_t(d.wrapV)] + " " +
                   kFilters[size_t(d.minFilter)] + " " + kFilters[size_t(d.magFilter)] + " " +
                   (d.mipmaps ? kMipFilters[d.mipFilter == TextureFilter::Nearest ? 1 : 2] : "none") + " " +
                   std::to_string(d.maxAnisotropy) + "\n";
        }
    }
    return out;
}

bool loadMaterialFile(const fs::path& path, MaterialFile& file, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "can't open " + path.generic_string();
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (!parseMaterial(text.str(), file, error)) {
        error = path.filename().string() + ", " + error;
        return false;
    }
    return true;
}

bool saveMaterialFile(const fs::path& path, const MaterialFile& file, std::string& error) {
    const fs::path temporary = path.string() + ".saving";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << writeMaterial(file);
        if (!out) {
            error = "can't write " + temporary.generic_string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        error = "can't replace " + path.generic_string();
        return false;
    }
    return true;
}

std::string materialSchemaMarkdown() {
    std::ostringstream out;
    out << "# Material file reference (.swmaterial)\n\n"
           "Generated by `player --material-schema` from the engine's material descriptors\n"
           "(Source/Runtime/Assets/Private/Material.cpp). Don't edit by hand.\n\n"
           "A `.swmaterial` file is UTF-8 text, one record per line. `#` starts a comment outside\n"
           "quotes; strings are double-quoted with `\\\"` and `\\\\` escaped. Numbers are plain decimals\n"
           "(a `.` for the point, whatever the system's locale). The first record is `swmaterial 1`.\n"
           "Anything unknown, out of range, given twice or left over on a line is an error, with\n"
           "its line number. Values a file doesn't set come from its parent, or from the defaults\n"
           "below. Saving from the editor writes this canonical order and drops comments.\n\n"
           "## Parent\n\n"
           "| Record | Meaning |\n|---|---|\n"
           "| `parent \"materials/base.swmaterial\"` | Start from another material file |\n"
           "| `parent \"models/crate.glb\" \"Rusty metal\"` | Start from a material imported with a model, "
           "found by name (so reordering the model's materials keeps it) |\n\n"
           "Chains are at most "
        << kMaxMaterialDepth
        << " files long and can't loop. A missing parent draws the material magenta, with\n"
           "the reason in the editor and `player --check`.\n\n"
           "## Values\n\n"
           "| Key | Values | Default | Range | Meaning |\n|---|---|---|---|---|\n";
    const MaterialParams defaults;
    for (const auto& info : kParams) {
        std::string values, initial, range = "-";
        switch (info.type) {
        case MaterialValueType::Bool:
            values = "`true` or `false`";
            initial = member<bool>(defaults, info) ? "true" : "false";
            break;
        case MaterialValueType::Enum:
            for (size_t i = 0; i < info.options.size(); ++i)
                values += std::string(i ? ", " : "") + "`" + info.options[i] + "`";
            initial = info.options[size_t(member<int>(defaults, info))];
            break;
        default: {
            const int count = componentCount(info.type);
            values = count == 1 ? "number" : count == 2 ? "2 numbers" : count == 3 ? "r g b" : "r g b a";
            const float* v = &member<float>(defaults, info);
            for (int c = 0; c < count; ++c) initial += (c ? " " : "") + number(v[c]);
            if (info.min < info.max) range = number(info.min) + ".." + number(info.max);
        }
        }
        out << "| `" << info.key << "` | " << values << " | " << initial << " | " << range << " | "
            << info.description << " |\n";
    }
    out << "\n## Texture slots\n\n"
           "| Record | Meaning |\n|---|---|\n"
           "| `texture <slot> \"textures/x.png\"` | The slot's texture: an image or .ktx2 in Content |\n"
           "| `clear_texture <slot>` | Removes a texture the parent has |\n"
           "| `uv_set <slot> 0` | Which UV set: 0 (TEXCOORD_0) or 1 (TEXCOORD_1) |\n"
           "| `uv_transform <slot> offsetU offsetV scaleU scaleV rotationRadians` | "
           "`uv' = offset + R(rotation) (scale uv)`, as glTF's KHR_texture_transform |\n"
           "| `sampler <slot> wrapU wrapV min mag mip anisotropy` | wrap repeat, clamp or mirror; min and mag "
           "linear or nearest; mip none, nearest or linear; anisotropy 1..16 (the graphics setting caps it) |\n\n"
           "| Slot | Decoded as | Holds |\n|---|---|---|\n";
    for (const auto& slot : kSlots)
        out << "| `" << materialSlotName(slot.slot) << "` | " << (slot.srgb ? "colour (sRGB)" : "data (linear)")
            << " | " << slot.description << " |\n";
    out << "\nA texture is decoded the way its slot says, whatever another material does with the same\n"
           "image: one used as colour by one material and as data by another is kept twice.\n\n"
           "## Detail and second layer\n\n"
           "The detail layer is on when `detail_color` or `detail_normal` has a texture and\n"
           "`detail_strength` is above 0; it tiles with its own `uv_transform` (scale 8 8 repeats it\n"
           "eight times). The second layer is on when `layer_mask` reads the vertex colours, or the\n"
           "`layer_mask` slot has a texture. At `material_quality low` the detail layer is left out;\n"
           "masks, second layers and alpha never are.\n";
    return out.str();
}

// ---- Resolving ----

const char* materialFeatureName(uint32_t feature) {
    switch (feature) {
    case kMaterialAlphaMask: return "alpha_mask";
    case kMaterialAlphaBlend: return "alpha_blend";
    case kMaterialNormalMap: return "normal_map";
    case kMaterialDetail: return "detail";
    case kMaterialDetailNormal: return "detail_normal";
    case kMaterialLayer: return "layer";
    case kMaterialDoubleSided: return "double_sided";
    }
    return "unknown";
}

uint32_t materialFeatures(const MaterialDesc& desc) {
    const MaterialParams& p = desc.params;
    auto has = [&](MaterialSlot slot) { return !desc.slots[size_t(slot)].texture.empty(); };
    uint32_t features = 0;
    if (p.alphaMode == int(MaterialAlpha::Mask)) features |= kMaterialAlphaMask;
    if (p.alphaMode == int(MaterialAlpha::Blend)) features |= kMaterialAlphaBlend;
    if (has(MaterialSlot::Normal)) features |= kMaterialNormalMap;
    if (p.detailStrength > 0.0f && (has(MaterialSlot::DetailColor) || has(MaterialSlot::DetailNormal))) {
        features |= kMaterialDetail;
        if (has(MaterialSlot::DetailNormal)) features |= kMaterialDetailNormal;
    }
    if (p.layerMask >= int(LayerMaskSource::VertexR) || has(MaterialSlot::LayerMask)) features |= kMaterialLayer;
    if (p.doubleSided) features |= kMaterialDoubleSided;
    return features;
}

std::vector<std::string> materialLowQualityOmissions(const MaterialDesc& desc) {
    std::vector<std::string> omitted;
    if (!(desc.features & kMaterialDetail)) return omitted;
    for (const auto slot : {MaterialSlot::DetailColor, MaterialSlot::DetailNormal, MaterialSlot::DetailMask})
        if (!desc.slots[size_t(slot)].texture.empty()) omitted.emplace_back(materialSlotName(slot));
    if (desc.params.detailRoughnessStrength > 0.0f) omitted.emplace_back("detail_roughness");
    return omitted;
}

MaterialDesc importedMaterial(const ModelData& model, std::string_view modelName, uint32_t index) {
    MaterialDesc desc;
    if (index >= model.materials.size()) return desc;
    const ModelMaterial& m = model.materials[index];
    MaterialParams& p = desc.params;
    p.baseColor = m.baseColor;
    p.metallic = m.metallic;
    p.roughness = m.roughness;
    p.normalScale = m.normalScale;
    p.occlusionStrength = m.occlusionStrength;
    p.emissive = m.emissive;
    p.alphaMode = int(m.alpha == ModelMaterial::Alpha::Mask    ? MaterialAlpha::Mask
                      : m.alpha == ModelMaterial::Alpha::Blend ? MaterialAlpha::Blend
                                                               : MaterialAlpha::Opaque);
    p.alphaCutoff = m.alphaCutoff;
    p.doubleSided = m.doubleSided;
    const auto textures = materialTextures(m);
    for (size_t slot = 0; slot < kMaterialSlots; ++slot) {
        desc.slots[slot].binding = m.bindings[slot];
        if (textures[slot] >= 0) desc.slots[slot].texture = {std::string(modelName), textures[slot]};
    }
    desc.chain.push_back(std::string(modelName) + "#" + m.name);
    desc.dependencies.emplace_back(modelName);
    desc.features = materialFeatures(desc);
    return desc;
}

int findSourceMaterial(const ModelData& model, std::string_view name, std::string& error) {
    int found = -1;
    for (size_t i = 0; i < model.materials.size(); ++i) {
        if (model.materials[i].name != name) continue;
        if (found >= 0) {
            error = "the model has more than one material called '" + std::string(name) +
                    "'; give them different names in the modelling tool";
            return -1;
        }
        found = int(i);
    }
    if (found < 0) {
        error = "the model has no material called '" + std::string(name) + "'";
        if (!model.materials.empty()) {
            error += " (it has";
            for (size_t i = 0; i < model.materials.size(); ++i)
                error += (i ? ", '" : " '") + model.materials[i].name + "'";
            error += ")";
        }
    }
    return found;
}

MaterialLookup folderMaterialLookup(const fs::path& root) {
    struct Models {
        std::mutex mutex;
        std::map<std::string, std::unique_ptr<ModelData>, std::less<>> loaded;
    };
    auto models = std::make_shared<Models>();
    MaterialLookup lookup;
    lookup.file = [root](std::string_view name, MaterialFile& file, std::string& error) {
        return loadMaterialFile(root / std::string(name), file, error);
    };
    lookup.model = [root, models](std::string_view name, std::string& error) -> const ModelData* {
        std::lock_guard lock(models->mutex);
        if (const auto it = models->loaded.find(name); it != models->loaded.end()) return it->second.get();
        auto model = std::make_unique<ModelData>();
        if (!isModelFile(std::string(name)) || !loadModel(root / std::string(name), *model, error)) {
            if (error.empty()) error = std::string(name) + " isn't a model";
            return nullptr;
        }
        return (models->loaded[std::string(name)] = std::move(model)).get();
    };
    lookup.texture = [root](std::string_view name) {
        std::error_code ec;
        const fs::path path = root / std::string(name);
        return (isImageFile(path) || isKtx2File(path)) && fs::is_regular_file(path, ec);
    };
    return lookup;
}

MaterialProblem resolveMaterial(
    std::string_view name, const MaterialLookup& lookup, MaterialDesc& out, std::string& error) {
    error.clear();
    // The chain of files, the material first.
    std::vector<std::pair<std::string, MaterialFile>> files;
    std::set<std::string, std::less<>> seen;
    std::string next(name);
    while (!next.empty() && isMaterialFile(next)) {
        if (!seen.insert(next).second) {
            error = "its parents loop: " + next + " is its own ancestor";
            return MaterialProblem::File;
        }
        if (files.size() == kMaxMaterialDepth) {
            error = "more than " + std::to_string(kMaxMaterialDepth) + " parents deep";
            return MaterialProblem::File;
        }
        MaterialFile file;
        std::string problem;
        if (!lookup.file || !lookup.file(next, file, problem)) {
            // The material itself not parsing is the file's problem; a parent that's gone is
            // a missing reference.
            error = files.empty() ? problem : "parent " + next + ": " + problem;
            const bool missing = !files.empty() && problem.rfind("can't open", 0) == 0;
            return missing ? MaterialProblem::Reference : MaterialProblem::File;
        }
        files.emplace_back(next, std::move(file));
        next = files.back().second.parent;
    }
    MaterialDesc desc;
    if (!next.empty()) {
        // An imported material at the root of the chain.
        const std::string& material = files.back().second.parentMaterial;
        std::string problem;
        const ModelData* model = lookup.model ? lookup.model(next, problem) : nullptr;
        if (!model) {
            error = "parent model " + next + ": " + (problem.empty() ? "not found" : problem);
            return MaterialProblem::Reference;
        }
        const int index = findSourceMaterial(*model, material, problem);
        if (index < 0) {
            error = "parent " + next + ": " + problem;
            return MaterialProblem::Reference;
        }
        desc = importedMaterial(*model, next, uint32_t(index));
    }
    // Apply each file's overrides, the oldest ancestor first.
    std::vector<std::string> chain;
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        const MaterialFile& file = it->second;
        for (size_t i = 0; i < kParams.size(); ++i) {
            if (!((file.set >> i) & 1u)) continue;
            const MaterialParamInfo& info = kParams[i];
            if (info.offset == offsetof(MaterialParams, family) && it != files.rbegin() &&
                member<int>(file.values, info) != desc.params.family) {
                error = it->first + " sets family " + info.options[size_t(member<int>(file.values, info))] +
                        ", which contradicts its parent's";
                return MaterialProblem::File;
            }
            const size_t size = info.type == MaterialValueType::Bool ? sizeof(bool)
                                : info.type == MaterialValueType::Enum ? sizeof(int)
                                                                       : sizeof(float) * componentCount(info.type);
            std::memcpy(
                reinterpret_cast<char*>(&desc.params) + info.offset,
                reinterpret_cast<const char*>(&file.values) + info.offset, size);
        }
        for (size_t s = 0; s < kAllMaterialSlots; ++s) {
            const MaterialSlotOverride& o = file.slots[s];
            ResolvedSlot& slot = desc.slots[s];
            if (o.hasTexture) slot.texture = o.texture.empty() ? MaterialTexture{} : MaterialTexture{o.texture, -1};
            if (o.hasUvSet) slot.binding.uvSet = o.uvSet;
            if (o.hasTransform) slot.binding.transform = o.transform;
            if (o.hasSampler) slot.binding.sampler = o.sampler;
            if (o.hasTexture && !o.texture.empty()) {
                if (lookup.texture && !lookup.texture(o.texture)) {
                    error = it->first + ": texture " + o.texture + " isn't in Content (or isn't an image)";
                    return MaterialProblem::Reference;
                }
                if (std::find(desc.dependencies.begin(), desc.dependencies.end(), o.texture) ==
                    desc.dependencies.end())
                    desc.dependencies.push_back(o.texture);
            }
        }
    }
    for (const auto& [file, data] : files) {
        chain.push_back(file);
        desc.dependencies.push_back(file);
    }
    chain.insert(chain.end(), desc.chain.begin(), desc.chain.end());
    desc.chain = std::move(chain);
    desc.features = materialFeatures(desc);
    // Values that do nothing, so an author isn't left wondering.
    const MaterialParams& p = desc.params;
    auto has = [&](MaterialSlot slot) { return !desc.slots[size_t(slot)].texture.empty(); };
    const bool layerValues = std::any_of(files.begin(), files.end(), [](const auto& f) {
        return f.second.isSet("layer_base_color") || f.second.isSet("layer_metallic") ||
               f.second.isSet("layer_roughness") || !f.second.slots[size_t(MaterialSlot::LayerBaseColor)].empty();
    });
    if (layerValues && !(desc.features & kMaterialLayer))
        desc.warnings.push_back("second-layer values are set but there's no layer mask (a layer_mask texture, or "
                                "layer_mask vertex_*), so they do nothing");
    const bool detailValues = std::any_of(files.begin(), files.end(), [](const auto& f) {
        return f.second.isSet("detail_strength") || f.second.isSet("detail_normal_scale") ||
               f.second.isSet("detail_roughness") || f.second.isSet("detail_roughness_strength") ||
               f.second.isSet("detail_fade");
    });
    if (detailValues && !has(MaterialSlot::DetailColor) && !has(MaterialSlot::DetailNormal))
        desc.warnings.push_back("detail values are set but there's no detail_color or detail_normal texture, so "
                                "they do nothing");
    if (has(MaterialSlot::DetailMask) && !has(MaterialSlot::DetailColor) && !has(MaterialSlot::DetailNormal))
        desc.warnings.push_back("detail_mask has a texture but there's no detail_color or detail_normal to mask");
    if (p.layerMask >= int(LayerMaskSource::VertexR) && has(MaterialSlot::LayerMask))
        desc.warnings.push_back("layer_mask reads the vertex colours, so the layer_mask texture is unused");
    out = std::move(desc);
    return MaterialProblem::None;
}

// ---- Drawing ----

void buildDrawMaterial(
    MaterialAsset& asset, const std::array<TextureHandle, kAllMaterialSlots>& textures,
    const std::array<SamplerHandle, kAllMaterialSlots>& samplers) {
    const MaterialParams& p = asset.desc.params;
    const uint32_t features = asset.desc.features;
    auto binding = [&](MaterialSlot slot) {
        const ModelTextureBinding& b = asset.desc.slots[size_t(slot)].binding;
        return TextureBinding{b.uvSet, b.transform, samplers[size_t(slot)]};
    };
    auto texture = [&](MaterialSlot slot) { return textures[size_t(slot)]; };
    asset.textures = textures;
    asset.samplers = samplers;
    asset.baseColorTexture = texture(MaterialSlot::BaseColor);
    asset.baseColor = p.baseColor;
    DrawMaterial& d = asset.draw;
    d = {};
    d.alphaCutoff = p.alphaMode == int(MaterialAlpha::Mask) ? p.alphaCutoff : -1.0f;
    d.doubleSided = p.doubleSided;
    d.blend = p.alphaMode == int(MaterialAlpha::Blend);
    d.roughness = p.roughness;
    d.metallic = p.metallic;
    d.emissive = p.emissive;
    d.normalTexture = texture(MaterialSlot::Normal);
    d.metallicRoughnessTexture = texture(MaterialSlot::MetallicRoughness);
    d.emissiveTexture = texture(MaterialSlot::Emissive);
    d.occlusionTexture = texture(MaterialSlot::Occlusion);
    d.normalScale = p.normalScale;
    d.occlusionStrength = p.occlusionStrength;
    for (size_t slot = 0; slot < kMaterialSlots; ++slot) d.bindings[slot] = binding(MaterialSlot(slot));

    MaterialLayers& l = asset.layers;
    l = {};
    l.detail = features & kMaterialDetail;
    l.detailColorTexture = texture(MaterialSlot::DetailColor);
    l.detailNormalTexture = texture(MaterialSlot::DetailNormal);
    l.detailMaskTexture = texture(MaterialSlot::DetailMask);
    l.detailStrength = p.detailStrength;
    l.detailNormalScale = p.detailNormalScale;
    l.detailRoughness = p.detailRoughness;
    l.detailRoughnessStrength = p.detailRoughnessStrength;
    l.detailMaskChannel = uint32_t(p.detailMaskChannel);
    l.detailFade = p.detailFade;
    l.layer = features & kMaterialLayer;
    l.layerMaskTexture = texture(MaterialSlot::LayerMask);
    l.layerMask = uint32_t(p.layerMask);
    l.layerBaseColorTexture = texture(MaterialSlot::LayerBaseColor);
    l.layerNormalTexture = texture(MaterialSlot::LayerNormal);
    l.layerMetallicRoughnessTexture = texture(MaterialSlot::LayerMetallicRoughness);
    l.layerBaseColor = p.layerBaseColor;
    l.layerMetallic = p.layerMetallic;
    l.layerRoughness = p.layerRoughness;
    l.layerNormalScale = p.layerNormalScale;
    for (size_t slot = kMaterialSlots; slot < kAllMaterialSlots; ++slot)
        l.bindings[slot - kMaterialSlots] = binding(MaterialSlot(slot));
    d.layers = l.detail || l.layer ? &asset.layers : nullptr;
}

DrawMaterial modelDrawMaterial(const ModelAsset& model, uint32_t material) {
    if (material < model.drawMaterials.size()) return model.drawMaterials[material];
    if (material >= model.data.materials.size()) return {};
    MaterialAsset drawn;
    drawn.desc = importedMaterial(model.data, {}, material);
    std::array<TextureHandle, kAllMaterialSlots> textures{};
    std::array<SamplerHandle, kAllMaterialSlots> samplers{};
    const auto maps = materialTextures(model.data.materials[material]);
    for (size_t slot = 0; slot < kMaterialSlots; ++slot) {
        if (maps[slot] >= 0 && size_t(maps[slot]) < model.textures.size()) textures[slot] = model.textures[size_t(maps[slot])];
        if (material < model.samplers.size()) samplers[slot] = model.samplers[material][slot];
    }
    buildDrawMaterial(drawn, textures, samplers);
    return drawn.draw;
}

MaterialAsset fallbackMaterial(std::string error) {
    MaterialAsset asset;
    asset.desc.params.baseColor = {1.0f, 0.0f, 1.0f, 1.0f};
    asset.desc.params.roughness = 0.6f;
    asset.desc.features = materialFeatures(asset.desc);
    asset.fallback = true;
    asset.error = std::move(error);
    buildDrawMaterial(asset, {}, {});
    return asset;
}

// ---- Shading math ----

glm::vec3 composeNormals(glm::vec3 base, glm::vec3 detail) {
    const glm::vec3 t = base + glm::vec3(0.0f, 0.0f, 1.0f);
    if (t.z <= 1e-4f) return glm::vec3(detail.x, -detail.y, -detail.z);   // base points straight back
    const glm::vec3 u = detail * glm::vec3(-1.0f, -1.0f, 1.0f);
    return glm::normalize(t * (glm::dot(t, u) / t.z) - u);
}

float toksvigAlpha(float alpha, float length) {
    if (!(length < 1.0f)) return alpha;
    if (length <= 0.0f) return 1.0f;
    // GGX alpha as the equivalent Blinn-Phong exponent s = 2 / alpha^2 - 2, narrowed by
    // Toksvig's factor ft = |n| / (|n| + s (1 - |n|)), and back.
    const float a2 = std::max(alpha * alpha, 1e-7f);
    const float s = 2.0f / a2 - 2.0f;
    const float ft = length / (length + s * (1.0f - length));
    return std::sqrt(2.0f / (ft * s + 2.0f));
}

}   // namespace shockwave
