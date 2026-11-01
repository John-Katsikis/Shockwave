// Material assets (A02, docs/MaterialSchema.md): the .swmaterial parser and writer,
// inheritance, the shading math the shaders share, coverage-keeping mipmaps, scene fields,
// and hot reload through AssetFolder on the null renderer.

#include <Assets/AssetFolder.h>
#include <Assets/Material.h>
#include <Assets/TextureCompression.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <Engine/SceneFile.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}
bool near(float a, float b, float tolerance = 1e-5f) { return std::abs(a - b) <= tolerance; }
bool near(glm::vec3 a, glm::vec3 b, float tolerance = 1e-5f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

bool parses(const std::string& text, MaterialFile& file, std::string& error) {
    file = {};
    return parseMaterial(text, file, error);
}

void parserChecks() {
    MaterialFile file;
    std::string error;
    const std::string text = "swmaterial 1   # comment\n"
                             "parent \"models/rock.glb\" \"Rock \\\"wet\\\"\"\n"
                             "texture detail_normal \"textures/ridges.png\"\n"
                             "roughness 0.25\n"
                             "base_color 0.5 0.25 1 1\n"
                             "uv_transform detail_normal 0 0 8 8 0\n"
                             "sampler detail_normal repeat mirror linear linear linear 8\n"
                             "alpha_mode mask\n";
    check(parses(text, file, error), "a valid file parses: " + error);
    check(file.parent == "models/rock.glb" && file.parentMaterial == "Rock \"wet\"", "quoted parent and material");
    check(file.isSet("roughness") && near(file.values.roughness, 0.25f), "roughness override");
    check(!file.isSet("metallic"), "unset values stay unset");
    const auto& detail = file.slots[size_t(MaterialSlot::DetailNormal)];
    check(detail.hasTexture && detail.texture == "textures/ridges.png", "detail texture");
    check(detail.hasTransform && near(detail.transform.scale.x, 8.0f), "detail transform");
    check(
        detail.hasSampler && detail.sampler.wrapV == TextureWrap::Mirror && detail.sampler.maxAnisotropy == 8,
        "detail sampler");

    // Canonical: schema order, comments gone, and it reads back as the same file.
    const std::string written = writeMaterial(file);
    check(written.find("comment") == std::string::npos, "comments are dropped");
    check(written.find("base_color") < written.find("roughness"), "values in schema order");
    MaterialFile again;
    check(parses(written, again, error), "written file parses: " + error);
    check(writeMaterial(again) == written, "writing is stable");
    check(again.set == file.set && again.values == file.values && again.slots == file.slots, "round trip");

    // Locale-independent numbers, with exact round trips.
    MaterialFile precise;
    precise.setValue("roughness", true);
    precise.values.roughness = 0.1f;
    MaterialFile back;
    check(parses(writeMaterial(precise), back, error) && back.values.roughness == 0.1f, "floats round-trip exactly");

    // Loud mistakes, each with its line.
    const std::pair<const char*, const char*> bad[] = {
        {"swmaterial 2\n", "version"},
        {"roughness 0.5\n", "swmaterial"},
        {"swmaterial 1\nroughnes 0.5\n", "line 2"},
        {"swmaterial 1\nroughness 0.5\nroughness 0.6\n", "line 3"},
        {"swmaterial 1\nroughness 1.5\n", "line 2"},
        {"swmaterial 1\nroughness nan\n", "line 2"},
        {"swmaterial 1\nroughness 0.5 extra\n", "line 2"},
        {"swmaterial 1\nroughness 0,5\n", "line 2"},
        {"swmaterial 1\nalpha_mode cutout\n", "line 2"},
        {"swmaterial 1\nfamily cloth\n", "line 2"},
        {"swmaterial 1\ntexture sheen \"t.png\"\n", "line 2"},
        {"swmaterial 1\ntexture normal \"../outside.png\"\n", "line 2"},
        {"swmaterial 1\ntexture normal \"C:\\\\t.png\"\n", "line 2"},
        {"swmaterial 1\nparent \"a.swmaterial\" \"extra\"\n", "line 2"},
        {"swmaterial 1\nparent \"rock.glb\"\n", "line 2"},
        {"swmaterial 1\nroughness inherit\n", "inherit"},
        {"swmaterial 1\nuv_set normal 2\n", "line 2"},
        {"swmaterial 1\nemissive 0 0 2000\n", "line 2"},
        {"swmaterial 1\nbase_color \"unterminated\n", "line 2"},
    };
    for (const auto& [source, says] : bad) {
        const bool ok = parses(source, file, error);
        check(
            !ok && error.find(says) != std::string::npos,
            std::string("rejects ") + source + " mentioning '" + says + "' (got: " + error + ")");
    }
    check(
        !materialSchemaMarkdown().empty() && materialSchemaMarkdown().find("detail_fade") != std::string::npos,
        "the schema lists every key");
}

// An in-memory Content folder for resolve.
struct Files {
    std::map<std::string, std::string, std::less<>> materials;
    std::map<std::string, ModelData, std::less<>> models;
    MaterialLookup lookup() const {
        MaterialLookup l;
        l.file = [this](std::string_view name, MaterialFile& file, std::string& error) {
            const auto it = materials.find(name);
            if (it == materials.end()) {
                error = "can't open " + std::string(name);
                return false;
            }
            file = {};
            if (!parseMaterial(it->second, file, error)) {
                error = std::string(name) + ", " + error;
                return false;
            }
            return true;
        };
        l.model = [this](std::string_view name, std::string& error) -> const ModelData* {
            const auto it = models.find(name);
            if (it == models.end()) error = "no model " + std::string(name);
            return it == models.end() ? nullptr : &it->second;
        };
        l.texture = [](std::string_view) { return true; };
        return l;
    }
};

ModelData rockModel(bool reordered) {
    ModelData model;
    ModelMaterial moss;
    moss.name = "Moss";
    moss.baseColor = {0.1f, 0.4f, 0.1f, 1.0f};
    moss.roughness = 0.9f;
    ModelMaterial rock;
    rock.name = "Rock";
    rock.baseColor = {0.5f, 0.5f, 0.5f, 1.0f};
    rock.metallic = 0.0f;
    rock.roughness = 0.6f;
    rock.baseColorTexture = 0;
    rock.alpha = ModelMaterial::Alpha::Mask;
    rock.alphaCutoff = 0.4f;
    model.materials = reordered ? std::vector{rock, moss} : std::vector{moss, rock};
    model.textures.resize(1);
    model.textures[0].source = "rock.png";
    return model;
}

void resolveChecks() {
    Files files;
    files.materials["base.swmaterial"] = "swmaterial 1\nroughness 0.3\nmetallic 1\n";
    files.materials["child.swmaterial"] = "swmaterial 1\nparent \"base.swmaterial\"\nroughness 0.8\n";
    MaterialDesc desc;
    std::string error;
    check(resolveMaterial("child.swmaterial", files.lookup(), desc, error) == MaterialProblem::None, error);
    check(near(desc.params.roughness, 0.8f) && near(desc.params.metallic, 1.0f), "child overrides, parent fills in");
    check(
        desc.chain.size() == 2 && desc.chain[0] == "child.swmaterial" && desc.chain[1] == "base.swmaterial",
        "chain lists the material first");
    MaterialDesc again;
    resolveMaterial("child.swmaterial", files.lookup(), again, error);
    check(again == desc, "resolving is deterministic");

    // An imported parent is found by name, so reordering the model's materials keeps it.
    files.models["rock.glb"] = rockModel(false);
    files.materials["rock.swmaterial"] = "swmaterial 1\nparent \"rock.glb\" \"Rock\"\nmetallic 0.5\n";
    MaterialDesc first, reordered;
    check(resolveMaterial("rock.swmaterial", files.lookup(), first, error) == MaterialProblem::None, error);
    files.models["rock.glb"] = rockModel(true);
    check(resolveMaterial("rock.swmaterial", files.lookup(), reordered, error) == MaterialProblem::None, error);
    check(first == reordered, "reordered source materials resolve the same");
    check(near(first.params.roughness, 0.6f) && near(first.params.metallic, 0.5f), "imported values inherited");
    check(first.params.alphaMode == int(MaterialAlpha::Mask) && near(first.params.alphaCutoff, 0.4f), "imported alpha");
    check(
        first.slots[size_t(MaterialSlot::BaseColor)].texture.asset == "rock.glb" &&
            first.slots[size_t(MaterialSlot::BaseColor)].texture.modelTexture == 0,
        "imported texture stays the model's");
    check((first.features & kMaterialAlphaMask) != 0, "mask feature");

    // Missing names are reference problems; loops, depth and parse errors are file problems.
    files.materials["orphan.swmaterial"] = "swmaterial 1\nparent \"gone.swmaterial\"\n";
    check(
        resolveMaterial("orphan.swmaterial", files.lookup(), desc, error) == MaterialProblem::Reference,
        "missing parent is a reference problem: " + error);
    files.materials["wrong.swmaterial"] = "swmaterial 1\nparent \"rock.glb\" \"Granite\"\n";
    check(
        resolveMaterial("wrong.swmaterial", files.lookup(), desc, error) == MaterialProblem::Reference &&
            error.find("Granite") != std::string::npos,
        "missing source material named: " + error);
    files.materials["a.swmaterial"] = "swmaterial 1\nparent \"b.swmaterial\"\n";
    files.materials["b.swmaterial"] = "swmaterial 1\nparent \"a.swmaterial\"\n";
    check(resolveMaterial("a.swmaterial", files.lookup(), desc, error) == MaterialProblem::File, "loops are refused");
    files.materials["broken.swmaterial"] = "swmaterial 1\nroughness 9\n";
    check(
        resolveMaterial("broken.swmaterial", files.lookup(), desc, error) == MaterialProblem::File,
        "a parse error is a file problem");
    for (size_t i = 0; i < kMaxMaterialDepth + 1; ++i)
        files.materials["d" + std::to_string(i) + ".swmaterial"] =
            "swmaterial 1\n" + (i ? "parent \"d" + std::to_string(i - 1) + ".swmaterial\"\n" : std::string());
    check(
        resolveMaterial("d15.swmaterial", files.lookup(), desc, error) == MaterialProblem::None,
        "16 files deep is allowed: " + error);
    check(
        resolveMaterial("d16.swmaterial", files.lookup(), desc, error) == MaterialProblem::File,
        "17 files deep is refused");

    // Detail and layers switch on with their textures; low quality names what it drops.
    files.materials["detail.swmaterial"] =
        "swmaterial 1\ntexture detail_normal \"n.png\"\ntexture layer_mask \"m.png\"\n";
    check(resolveMaterial("detail.swmaterial", files.lookup(), desc, error) == MaterialProblem::None, error);
    check(
        (desc.features & kMaterialDetail) && (desc.features & kMaterialDetailNormal) &&
            (desc.features & kMaterialLayer),
        "detail and layer features");
    const auto omitted = materialLowQualityOmissions(desc);
    check(omitted.size() == 1 && omitted[0] == "detail_normal", "low quality omits only the detail normal");
    // A value that does nothing says so.
    files.materials["idle.swmaterial"] = "swmaterial 1\ndetail_strength 0.5\n";
    check(
        resolveMaterial("idle.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            !desc.warnings.empty(),
        "a detail value without a detail texture warns");

    // Shading families (A07-A09): each switches on its feature, says what it draws as with
    // each setting, and warns about another family's values.
    ShadingSettings all;
    all.transmission = TransmissionQuality::High;
    all.skin = SkinQuality::High;
    all.hair = HairQuality::High;
    ShadingSettings off;
    off.clearcoat = false;
    off.transmission = TransmissionQuality::Off;
    off.skin = SkinQuality::Off;
    off.hair = HairQuality::Low;
    files.materials["plain.swmaterial"] = "swmaterial 1\nroughness 0.4\n";
    check(
        resolveMaterial("plain.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            materialEffectiveShading(desc, all) == "standard" && materialShadingCost(desc, all).empty(),
        "a plain material draws as standard with nothing extra: " + materialEffectiveShading(desc, all));
    files.materials["coat.swmaterial"] = "swmaterial 1\nclearcoat 1\n";
    check(
        resolveMaterial("coat.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            (desc.features & kMaterialClearcoat) && desc.warnings.empty(),
        "clearcoat feature: " + error);
    check(materialEffectiveShading(desc, off) == "standard (clearcoat off)", "clearcoat off draws standard");
    files.materials["nocoat.swmaterial"] = "swmaterial 1\nclearcoat 0\n";
    check(
        resolveMaterial("nocoat.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            !(desc.features & kMaterialClearcoat),
        "clearcoat 0 is plain standard");
    files.materials["glass.swmaterial"] =
        "swmaterial 1\nfamily transmission\ntransmission_mode solid\nior 1.5\nthickness 0.02\n";
    check(
        resolveMaterial("glass.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            (desc.features & kMaterialTransmission) && !(desc.features & kMaterialAlphaBlend),
        "transmission feature, not blended: " + error);
    check(materialEffectiveShading(desc, all).find("refracted") != std::string::npos, "solid glass refracts on high");
    check(
        materialEffectiveShading(desc, off).find("simplified glass") != std::string::npos,
        "transmission off is simplified glass");
    check(
        !materialShadingCost(desc, all).empty() && materialShadingCost(desc, off).empty(),
        "glass costs a pass only when it runs");
    files.materials["skin.swmaterial"] = "swmaterial 1\nfamily skin\nskin_radius 0.004 0.002 0.001\n";
    check(
        resolveMaterial("skin.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            (desc.features & kMaterialSkin),
        "skin feature: " + error);
    check(
        materialEffectiveShading(desc, off) == "skin without diffusion (opaque)" &&
            materialShadingCost(desc, off).empty(),
        "skin_quality off: no pass");
    files.materials["hair.swmaterial"] = "swmaterial 1\nfamily hair_card\nalpha_mode mask\n";
    check(
        resolveMaterial("hair.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            (desc.features & kMaterialHairCard) &&
            materialEffectiveShading(desc, all) == "hair card, anisotropic highlight" &&
            materialEffectiveShading(desc, off) == "hair card, round highlight",
        "hair card feature and quality: " + error);
    files.materials["mixed.swmaterial"] = "swmaterial 1\nfamily skin\nior 1.4\nclearcoat 1\n";
    check(
        resolveMaterial("mixed.swmaterial", files.lookup(), desc, error) == MaterialProblem::None &&
            desc.warnings.size() >= 2 && !(desc.features & kMaterialClearcoat),
        "another family's values warn and do nothing");
    check(
        materialFamilyUses(int(MaterialFamily::Standard), "clearcoat") &&
            !materialFamilyUses(int(MaterialFamily::Skin), "clearcoat") &&
            materialFamilyUses(int(MaterialFamily::Skin), "surface"),
        "family groups");
}

void shadingChecks() {
    const glm::vec3 flat{0.0f, 0.0f, 1.0f};
    const glm::vec3 tilted = glm::normalize(glm::vec3(0.3f, -0.2f, 0.9f));
    check(near(composeNormals(tilted, flat), tilted), "neutral detail returns the base");
    check(near(composeNormals(flat, tilted), tilted), "neutral base returns the detail");
    uint32_t seed = 12345;
    auto random = [&] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24) * 2.0f - 1.0f;
    };
    for (int i = 0; i < 200; ++i) {
        const glm::vec3 b = glm::normalize(glm::vec3(random(), random(), std::abs(random()) + 0.05f));
        const glm::vec3 d = glm::normalize(glm::vec3(random(), random(), std::abs(random()) + 0.05f));
        const glm::vec3 n = composeNormals(b, d);
        check(near(glm::length(n), 1.0f, 1e-4f) && std::isfinite(n.x), "composed normals are unit length");
    }
    check(near(glm::length(composeNormals(glm::vec3(0, 0, -1), tilted)), 1.0f, 1e-4f), "a backwards base stays finite");

    check(near(toksvigAlpha(0.25f, 1.0f), 0.25f), "an unfiltered normal keeps its roughness");
    float last = 0.25f;
    for (float length : {0.99f, 0.95f, 0.8f, 0.5f}) {
        const float a = toksvigAlpha(0.25f, length);
        check(a > last && a <= 1.0f, "shorter mean normals widen the highlight");
        last = a;
    }
}

void coverageChecks() {
    // Foliage-like alpha: mostly low, a scattering of opaque texels. Box filtering pulls
    // every texel towards the mean, under the cutoff, so plain mipmaps thin out.
    TextureData source;
    source.width = source.height = 64;
    source.srgb = true;
    source.bytes.assign(64 * 64 * 4, 0);
    uint32_t state = 7;
    for (size_t i = 0; i < 64 * 64; ++i) {
        state = state * 1664525u + 1013904223u;
        const float r = float(state >> 8) / float(1u << 24);
        source.bytes[i * 4 + 1] = 200;
        source.bytes[i * 4 + 3] = uint8_t(std::lround(r * r * 255.0f));
    }
    source.levels = {{64, 64, 0, source.bytes.size()}};
    const float cutoff = 0.5f;
    const float original = alphaCoverage(source.bytes.data(), 64, 64, cutoff);
    const TextureData kept = textureMipChain(source, cutoff);
    const TextureData plain = textureMipChain(source);
    check(kept.levels.size() == 7 && plain.levels.size() == 7, "a full chain");
    bool keptClose = true, plainLost = false;
    for (size_t level = 1; kept.levels[level].width >= 8; ++level) {   // down to 8 x 8
        const auto& k = kept.levels[level];
        const auto& p = plain.levels[level];
        const float c = alphaCoverage(kept.bytes.data() + k.offset, k.width, k.height, cutoff);
        const float d = alphaCoverage(plain.bytes.data() + p.offset, p.width, p.height, cutoff);
        if (std::abs(c - original) > 0.06f) {
            std::fprintf(stderr, "level %zu: coverage %.3f kept, %.3f plain, %.3f original\n", level, c, d, original);
            keptClose = false;
        }
        if (std::abs(d - original) > 0.15f) plainLost = true;
    }
    check(keptClose, "coverage-keeping mipmaps stay near the original coverage");
    check(plainLost, "plain mipmaps lose the coverage (the test pattern is hard enough)");
    // Colour is untouched by the alpha scaling.
    check(kept.bytes[kept.levels[1].offset + 1] == plain.bytes[plain.levels[1].offset + 1], "colour unchanged");

    const std::vector<uint8_t> bytes = {1, 2, 3};
    check(
        textureCacheKey(bytes, TextureCompression::High, true, 0.5f) !=
            textureCacheKey(bytes, TextureCompression::High, true),
        "the cache key includes the coverage cutoff");
    check(
        textureCacheKey(bytes, TextureCompression::High, true, 0.5f) !=
            textureCacheKey(bytes, TextureCompression::High, true, 0.4f),
        "and its value");
}

void sceneChecks() {
    Assets assets;
    auto material = std::make_shared<MaterialAsset>();
    const MaterialHandle stone = assets.addMaterial("materials/stone.swmaterial", material);
    const MaterialHandle moss = assets.addMaterial("materials/moss.swmaterial", material);
    check(assets.findMaterial("materials/stone.swmaterial") == stone, "materials are found by name");
    check(assets.addMaterial("materials/stone.swmaterial", material) == stone, "a reload keeps its handle");

    World world;
    const Entity cube = world.create();
    MeshRenderer mesh;
    mesh.material = stone;
    world.add<MeshRenderer>(cube, mesh);
    const Entity rock = world.create();
    ModelRenderer model;
    model.materials = {{"Rock \"wet\"", moss}, {"Moss", stone}};
    world.add<ModelRenderer>(rock, model);
    const Entity plain = world.create();
    world.add<MeshRenderer>(plain, MeshRenderer{});

    std::ostringstream out;
    writeScene(out, world, assets);
    const std::string text = out.str();
    check(
        text.find("    material ") != std::string::npos && text.find("materials/stone.swmaterial") != std::string::npos,
        "material field written");
    size_t count = 0;
    for (size_t at = text.find("    material "); at != std::string::npos; at = text.find("    material ", at + 1))
        ++count;
    check(count == 1, "an unset material isn't written");
    World loaded;
    std::string error;
    std::vector<std::string> warnings;
    std::istringstream in(text);
    check(readScene(loaded, assets, in, "test", error, {}, &warnings), "scene reads back: " + error);
    check(warnings.empty(), "no warnings");
    const auto* m = loaded.get<MeshRenderer>(loaded.entityAt(0));
    const auto* r = loaded.get<ModelRenderer>(loaded.entityAt(1));
    check(m && m->material == stone, "MeshRenderer material round trip");
    check(
        r && r->materials.size() == 2 && findMaterialOverride(r->materials, "Rock \"wet\"") &&
            findMaterialOverride(r->materials, "Rock \"wet\"")->material == moss,
        "per-part overrides round trip, quotes and all");
    check(!loaded.get<MeshRenderer>(loaded.entityAt(2))->material.valid(), "unset stays unset");

    std::istringstream unknown(
        "shockwave_scene 2\nentity \"A\"\n  MeshRenderer\n    material \"nope.swmaterial\"\nend\n");
    warnings.clear();
    check(
        readScene(loaded, assets, unknown, "test", error, {}, &warnings) && !warnings.empty(),
        "an unknown material warns");
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
// The file's modification time must move for the folder to notice.
void touchLater(const fs::path& path) {
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
}

void hotReloadChecks(Engine& engine) {
    const fs::path root = fs::temp_directory_path() / "shockwave-material-test";
    fs::remove_all(root);
    write(root / "base.swmaterial", "swmaterial 1\nroughness 0.3\n");
    write(root / "child.swmaterial", "swmaterial 1\nparent \"base.swmaterial\"\nmetallic 1\n");
    AssetFolder folder(engine.renderer(), engine.assets(), engine.jobs());
    folder.open(root);
    auto current = [&](const char* name) { return engine.assets().material(engine.assets().findMaterial(name)); };
    auto child = current("child.swmaterial");
    check(child && !child->fallback && near(child->desc.params.roughness, 0.3f), "child resolves through its parent");

    // Editing the parent reloads the child.
    write(root / "base.swmaterial", "swmaterial 1\nroughness 0.7\n");
    touchLater(root / "base.swmaterial");
    folder.refresh(true);
    child = current("child.swmaterial");
    check(child && near(child->desc.params.roughness, 0.7f), "a parent edit reaches the child");

    // A half-written file keeps the last good version, with the error on the file.
    write(root / "child.swmaterial", "swmaterial 1\nparent \"base.swmaterial\"\nmetallic 1\nroughn");
    touchLater(root / "child.swmaterial");
    folder.refresh(true);
    child = current("child.swmaterial");
    const AssetFolder::File* file = folder.find("child.swmaterial");
    check(child && !child->fallback && near(child->desc.params.metallic, 1.0f), "last good version kept");
    check(file && !file->error.empty(), "the parse error is reported");

    // A missing parent draws the fallback, and says why.
    write(root / "child.swmaterial", "swmaterial 1\nparent \"gone.swmaterial\"\n");
    touchLater(root / "child.swmaterial");
    folder.refresh(true);
    child = current("child.swmaterial");
    check(
        child && child->fallback && child->error.find("gone.swmaterial") != std::string::npos,
        "a missing parent falls back to magenta");
    check(child && near(child->baseColor.g, 0.0f) && child->baseColor.r > 0.5f, "magenta");

    // Fixed again: back to normal, same handle.
    const MaterialHandle handle = engine.assets().findMaterial("child.swmaterial");
    write(root / "child.swmaterial", "swmaterial 1\nparent \"base.swmaterial\"\n");
    touchLater(root / "child.swmaterial");
    folder.refresh(true);
    child = current("child.swmaterial");
    check(
        child && !child->fallback && engine.assets().findMaterial("child.swmaterial") == handle,
        "fixing it recovers, keeping the handle");
    fs::remove_all(root);
}

class App final : public Application {
public:
    void onStart() override {
        hotReloadChecks(engine());
        engine().quit();
    }
};

}   // namespace

int main() {
    parserChecks();
    resolveChecks();
    shadingChecks();
    coverageChecks();
    sceneChecks();
    EngineConfig config;
    config.window = {"material test", 320, 240, false, true};
    config.renderer = "null";
    config.maxFrames = 0;
    config.showFpsInTitle = false;
    Engine engine(config);
    App app;
    engine.run(app);
    if (failures) return 1;
    std::printf("materials: all checks passed\n");
    return 0;
}
