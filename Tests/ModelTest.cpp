#include <Assets/Assets.h>
#include <Assets/Model.h>
#include <Engine/Components.h>
#include <Engine/SceneFile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
using namespace shockwave;
namespace fs = std::filesystem;
namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
bool near(float a, float b) { return std::abs(a - b) < .0001f; }
void imported(const fs::path& root, const char* filename) {
    ModelData m;
    std::string error;
    const bool ok = loadModel(root / filename, m, error);
    check(ok, error.c_str());
    if (!ok) return;
    check(m.meshes.size() == 3, "all material parts preserved");
    check(m.materials.size() >= 3, "material count");
    check(m.textures.size() == 1 && m.textures[0].image.width == 2, "external/embedded PNG decoded");
    check(m.triangles == 3, "triangle count");
    check(
        near(m.bounds.min.x, 2) && near(m.bounds.max.x, 5) && near(m.bounds.max.z, 1), "nested transforms and bounds");
    bool mask = false, blend = false;
    for (const auto& mat : m.materials) {
        mask |= mat.alpha == ModelMaterial::Alpha::Mask && mat.doubleSided && near(mat.alphaCutoff, .4f);
        blend |= mat.alpha == ModelMaterial::Alpha::Blend && near(mat.baseColor.a, .4f);
    }
    check(mask && blend, "mask/blend properties");
    float t = 0;
    check(intersectModel(m, {2.2f, .2f, 2}, {0, 0, -1}, t) && near(t, 1), "nearest triangle in transformed child");
    check(!intersectModel(m, {2.9f, .9f, 2}, {0, 0, -1}, t), "triangle picking rejects empty part of bounds");
    check(m.nodes.size() >= 4 && m.nodes[1].parent == 0, "parent-first hierarchy");
    int instances = 0;
    for (const auto& node : m.nodes)
        for (auto mesh : node.meshes) instances += mesh == 0;
    check(instances == 2 && near(m.bounds.max.y, 3), "repeated mesh instances preserved");
    check(near(m.meshes[0].data.vertices[0].uv.y, 0), "glTF texture orientation");
}
// pbr-maps.glb (generate_pbr.py): every glTF map, with tangents made on import for the
// cube and taken from the file for the wall, whose texture is mirrored.
void materialMaps(const fs::path& root) {
    ModelData m;
    std::string error;
    check(loadModel(root / "pbr-maps.glb", m, error), error.c_str());
    if (!error.empty()) return;
    const ModelMaterial* tiles = nullptr;
    const ModelMaterial* gold = nullptr;
    for (const auto& material : m.materials) {
        if (material.name == "Tiles") tiles = &material;
        if (material.name == "Polished gold") gold = &material;
    }
    check(tiles && gold, "material names");
    if (!tiles || !gold) return;
    check(
        tiles->baseColorTexture >= 0 && tiles->normalTexture >= 0 && tiles->metallicRoughnessTexture >= 0 &&
            tiles->emissiveTexture >= 0 && tiles->occlusionTexture >= 0,
        "every material map imported, occlusion included");
    check(
        tiles->occlusionTexture == tiles->metallicRoughnessTexture,
        "a shared occlusion-metallic-roughness image is read once");
    if (tiles->baseColorTexture >= 0 && tiles->normalTexture >= 0 && tiles->emissiveTexture >= 0 &&
        tiles->metallicRoughnessTexture >= 0)
        check(
            m.textures[tiles->baseColorTexture].srgb && m.textures[tiles->emissiveTexture].srgb &&
                !m.textures[tiles->normalTexture].srgb && !m.textures[tiles->metallicRoughnessTexture].srgb,
            "colour maps are sRGB, data maps linear");
    check(
        near(tiles->normalScale, 1) && near(tiles->occlusionStrength, 1) && near(tiles->emissive.x, 1), "map factors");
    check(near(gold->metallic, 1) && near(gold->roughness, .25f) && gold->normalTexture < 0, "factor-only material");
    for (const auto& warning : m.warnings) check(warning.find("later milestone") == std::string::npos, warning.c_str());

    bool orthonormal = true, cubeFront = false, wall = false;
    for (const auto& mesh : m.meshes)
        for (const auto& v : mesh.data.vertices) {
            const glm::vec3 t(v.tangent);
            orthonormal &= near(glm::length(t), 1) && std::abs(glm::dot(t, v.normal)) < 1e-3f &&
                           std::abs(std::abs(v.tangent.w) - 1) < 1e-6f;
            // The cube's +Z face runs u along +X; the wall's texture runs along -X.
            if (mesh.name == "Tile cube" && near(v.normal.z, 1))
                cubeFront = near(v.tangent.x, 1) && near(v.tangent.w, 1);
            if (mesh.name == "Mirrored wall") wall = near(v.tangent.x, -1) && near(v.tangent.w, -1);
        }
    check(orthonormal, "tangents are unit length, across the surface, with a handedness of 1 or -1");
    check(cubeFront, "tangents made from texture coordinates");
    check(wall, "the file's tangents and their handedness are kept");
    // Bitangents point up the texture: towards smaller v.
    for (const auto& mesh : m.meshes) {
        if (mesh.name != "Tile cube") continue;
        const auto& v = mesh.data.vertices;
        bool up = true;
        for (size_t i = 0; i + 2 < mesh.data.indices.size(); i += 3) {
            const Vertex& a = v[mesh.data.indices[i]];
            const glm::vec3 b = glm::cross(a.normal, glm::vec3(a.tangent)) * a.tangent.w;
            for (int k = 1; k < 3; ++k) {
                const Vertex& c = v[mesh.data.indices[i + k]];
                const float along = glm::dot(c.position - a.position, b), dv = c.uv.y - a.uv.y;
                if (std::abs(dv) > 1e-4f) up &= (along > 0) == (dv < 0);
            }
        }
        check(up, "bitangents point up the texture");
    }
}
void scenes(const fs::path& file) {
    Assets assets;
    auto first = std::make_shared<ModelAsset>();
    const auto handle = assets.addModel("models/parts with spaces.glb", first);
    World world;
    const auto e = world.create();
    world.add<Name>(e, {"Imported"});
    world.add<Transform>(e);
    world.add<ModelRenderer>(e, {handle, {.1f, .2f, .3f, 1}, false});
    std::string error;
    check(saveScene(world, assets, file.string(), error), error.c_str());
    World loaded;
    std::vector<std::string> warnings;
    check(loadScene(loaded, assets, file.string(), error, {}, &warnings), error.c_str());
    check(warnings.empty() && loaded.count<ModelRenderer>() == 1, "model reference round trip");
    loaded.each<const ModelRenderer>([&](const auto& r) {
        check(r.model.id == handle.id && !r.visible && near(r.tint.z, .3f), "model inspector fields serialized");
    });
    auto replacement = std::make_shared<ModelAsset>();
    check(
        assets.addModel("models/parts with spaces.glb", replacement).id == handle.id,
        "reload keeps stable model handle");
    check(assets.model(handle) == replacement && first != replacement, "atomic asset replacement");
    Assets empty;
    warnings.clear();
    check(loadScene(loaded, empty, file.string(), error, {}, &warnings), error.c_str());
    check(warnings.size() == 1, "missing model produces scene warning");
}
// F1: a texture that isn't where the model's file says is looked for nearby by name.
void missingTextures(const fs::path& root, const fs::path& temporary) {
    int n = 0;
    // A folder with Models/quad.obj naming `map`; returns the model's path.
    auto setup = [&](const std::string& map) {
        const fs::path dir = temporary / ("nearby-" + std::to_string(n++));
        fs::create_directories(dir / "Models");
        fs::copy_file(root / "quad.obj", dir / "Models/quad.obj");
        std::ofstream(dir / "Models/quad.mtl") << "newmtl Paint\nKd 0.8 0.2 0.1\nmap_Kd " << map << "\n";
        return dir;
    };
    auto load = [](const fs::path& model, ModelData& m) {
        std::string error;
        const bool ok = loadModel(model, m, error);
        if (!ok) std::fprintf(stderr, "  %s\n", error.c_str());
        return ok;
    };
    auto usedNearby = [](const ModelData& m, const char* name) {
        bool warned = false;
        for (const auto& w : m.warnings) warned |= w.find("isn't where the file says") != std::string::npos;
        return m.missingTextures.empty() && m.textures.size() == 1 && warned &&
               fs::path(m.textures[0].source).filename() == name;
    };
    {   // in a sibling Textures/ folder
        const fs::path dir = setup("../art/wood.png");
        fs::create_directories(dir / "Textures");
        fs::copy_file(root / "checker.png", dir / "Textures/wood.png");
        ModelData m;
        check(load(dir / "Models/quad.obj", m) && usedNearby(m, "wood.png"), "texture found in a sibling folder");
        bool tracked = false;
        for (const auto& p : m.dependencies) tracked |= p.filename() == "wood.png";
        check(tracked, "the texture found nearby is a dependency");
    }
    {   // with different letter case, one folder down (beside the model, a case-insensitive
        // file system would just open it)
        const fs::path dir = setup("Wood.PNG");
        fs::create_directories(dir / "Models/maps");
        fs::copy_file(root / "checker.png", dir / "Models/maps/wood.png");
        ModelData m;
        check(load(dir / "Models/quad.obj", m) && usedNearby(m, "wood.png"), "texture found with other letter case");
    }
    {   // as a .png where the file says .tga
        const fs::path dir = setup("textures/wood.tga");
        fs::create_directories(dir / "Models/maps");
        fs::copy_file(root / "checker.png", dir / "Models/maps/wood.png");
        ModelData m;
        check(load(dir / "Models/quad.obj", m) && usedNearby(m, "wood.png"), "texture found with another extension");
    }
    {   // an artist's absolute Windows path, found by name
        const fs::path dir = setup("C:\\Users\\artist\\wood.png");
        fs::copy_file(root / "checker.png", dir / "wood.png");
        ModelData m;
        check(load(dir / "Models/quad.obj", m) && usedNearby(m, "wood.png"), "absolute Windows path found by name");
    }
    {   // never deeper than one folder
        const fs::path dir = setup("wood.png");
        fs::create_directories(dir / "Models/a/b");
        fs::copy_file(root / "checker.png", dir / "Models/a/b/wood.png");
        ModelData m;
        check(
            load(dir / "Models/quad.obj", m) && m.missingTextures.size() == 1 && m.textures.empty(),
            "two folders down isn't searched");
    }
    {   // a file that's there but won't decode still fails the import
        const fs::path dir = setup("wood.png");
        std::ofstream(dir / "Models/wood.png") << "not an image";
        ModelData m;
        std::string error;
        check(!loadModel(dir / "Models/quad.obj", m, error) && !error.empty(), "a corrupt texture still fails");
    }
}
}   // namespace
// The FBX test models (generate_fbx.py), through whichever importer this build has: the
// Autodesk FBX SDK with -DSHOCKWAVE_FBXSDK=ON, Assimp otherwise (docs/Assets.md, F1b).
void fbxModels(const fs::path& root) {
    const std::string expected = fbxSdkAvailable() ? "fbxsdk" : "assimp";
    ModelData crate;
    std::string error;
    check(loadModel(root / "fbx/crate.fbx", crate, error), error.c_str());
    if (!error.empty()) return;
    check(crate.importer == expected, "the FBX importer this build has reads .fbx");
    size_t vertices = 0;
    for (const auto& mesh : crate.meshes) vertices += mesh.data.vertices.size();
    check(vertices == 24 && crate.triangles == 12, "FBX cube: 24 corners, 12 triangles");
    check(
        crate.materials.size() == 1 && crate.materials[0].name == "Wood" && near(crate.materials[0].baseColor.r, .8f) &&
            near(crate.materials[0].baseColor.b, .4f),
        "FBX material and colour");
    check(
        crate.materials.size() == 1 && crate.materials[0].baseColorTexture >= 0 && crate.textures.size() == 1 &&
            crate.textures[0].image.width == 2 && crate.missingTextures.empty(),
        "FBX texture with an artist's absolute path found by name");
    // Pallet (y 10) > Crate (y 50, turned 30 degrees about Y): a 100 cm cube.
    const float reach = 50.0f * (std::cos(glm::radians(30.0f)) + std::sin(glm::radians(30.0f)));
    check(
        std::abs(crate.bounds.max.x - reach) < .01f && std::abs(crate.bounds.min.y - 10) < .01f &&
            std::abs(crate.bounds.max.y - 110) < .01f,
        "FBX node transforms and bounds");
    bool pallet = false;
    for (const auto& node : crate.nodes) pallet |= node.name == "Pallet";
    check(pallet, "FBX node names");

    ModelData arm;
    check(loadModel(root / "fbx/arm.fbx", arm, error), error.c_str());
    if (!error.empty()) return;
    const ModelMesh* skinned = nullptr;
    for (const auto& mesh : arm.meshes)
        if (mesh.skinned()) skinned = &mesh;
    check(skinned && skinned->bones.size() == 2, "FBX skin with two joints");
    if (skinned) {
        std::vector<std::string> joints;
        for (const auto& bone : skinned->bones) joints.push_back(arm.nodes[bone.node].name);
        std::sort(joints.begin(), joints.end());
        check(joints == std::vector<std::string>{"Elbow", "Shoulder"}, "FBX joint names");
        bool split = false;   // the middle ring follows both joints equally
        for (const auto& skin : skinned->skin) split |= near(skin.weights.x, .5f) && near(skin.weights.y, .5f);
        check(split, "FBX skin weights");
    }
    check(
        std::abs(arm.bounds.min.y) < .01f && std::abs(arm.bounds.max.y - 200) < .01f &&
            std::abs(arm.bounds.max.x - 5) < .01f,
        "FBX rest pose bounds");
    check(arm.animations.size() == 1 && std::abs(arm.animations[0].duration - 1.0f) < .01f, "FBX take, one second");
    if (arm.animations.size() == 1) {
        check(arm.animations[0].name.find("Wave") != std::string::npos, "FBX take name");
        bool elbow = false;
        for (const auto& channel : arm.animations[0].channels) elbow |= arm.nodes[channel.node].name == "Elbow";
        check(elbow, "FBX take moves the elbow");
    }

    // fbx_importer assimp: always Assimp. sdk: the SDK, and an error in a build without it.
    ModelData forced;
    check(
        loadModel(root / "fbx/crate.fbx", forced, error, nullptr, FbxImporter::Assimp) && forced.importer == "assimp",
        "fbx_importer assimp reads with Assimp");
    const bool sdk = loadModel(root / "fbx/crate.fbx", forced, error, nullptr, FbxImporter::Sdk);
    check(
        fbxSdkAvailable() ? sdk && forced.importer == "fbxsdk"
                          : !sdk && error.find("-DSHOCKWAVE_FBXSDK=ON") != std::string::npos,
        "fbx_importer sdk needs the SDK");
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const fs::path root = fs::path(argv[1]) / "Tests/fixtures/models";
    check(
        isModelFile("THING.GLTF") && isModelFile("a.glb") && isModelFile("b.fbx") && isModelFile("c.blend") &&
            isModelFile("d.obj"),
        "format detection, case insensitive");
    check(!isModelFile("a.bin") && !isModelFile("a.png") && !isModelFile("a.mtl"), "dependencies are not model assets");
    imported(root, "parts.gltf");
    imported(root, "parts.glb");
    materialMaps(root);
    fbxModels(root);
    ModelData quad;
    std::string error;
    check(loadModel(root / "quad.obj", quad, error), error.c_str());
    check(quad.triangles == 2 && quad.textures.size() == 1, "OBJ quad triangulation and MTL texture");
    bool mtl = false;
    for (const auto& p : quad.dependencies) mtl |= p.filename() == "quad.mtl";
    check(mtl, "MTL dependency tracking");
    const auto temporary =
        fs::temp_directory_path() /
        ("shockwave-model-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temporary);
    fs::copy_file(root / "parts.gltf", temporary / "broken.gltf");
    ModelData unchanged = quad;
    std::vector<fs::path> attempted;
    check(
        !loadModel(temporary / "broken.gltf", unchanged, error, &attempted) && !error.empty(),
        "missing external buffer fails clearly");
    check(unchanged.triangles == quad.triangles, "failed import preserves destination");
    bool bin = false;
    for (const auto& p : attempted) bin |= p.filename() == "parts.bin";
    check(bin, "failed dependency retained for retry");
    fs::copy_file(root / "parts.bin", temporary / "parts.bin");
    // A texture that isn't anywhere: the model imports without it and says so.
    ModelData partial;
    check(loadModel(temporary / "broken.gltf", partial, error, &attempted), error.c_str());
    check(
        partial.missingTextures.size() == 1 && partial.missingTextures[0].ends_with("checker.png") &&
            partial.textures.empty(),
        "missing texture imports without it");
    bool missingWarning = false;
    for (const auto& w : partial.warnings) missingWarning |= w.find("is missing") != std::string::npos;
    check(missingWarning, "missing texture is a warning");
    bool png = false;
    for (const auto& p : attempted) png |= p.filename() == "checker.png";
    check(png, "missing texture tracked");
    fs::copy_file(root / "checker.png", temporary / "checker.png");
    check(loadModel(temporary / "broken.gltf", unchanged, error), "repaired dependencies import successfully");
    check(unchanged.missingTextures.empty() && unchanged.textures.size() == 1, "repaired texture is used");
    missingTextures(root, temporary);
    scenes(temporary / "model.swscene");
    // Additional upstream format probes are opt-in and never needed by CTest.
    for (int i = 2; i < argc; ++i) {
        ModelData probe;
        const bool ok = loadModel(argv[i], probe, error);
        check(ok, error.c_str());
        if (ok)
            std::printf(
                "Imported %s: %zu meshes, %llu triangles\n", argv[i], probe.meshes.size(),
                static_cast<unsigned long long>(probe.triangles));
    }
    fs::remove_all(temporary);
    return failures ? 1 : 0;
}
