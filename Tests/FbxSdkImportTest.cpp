// F1b: the Autodesk FBX SDK and Assimp read the test FBX files the same way: same vertex
// counts, bounds within 1 mm, same joint names and clip lengths. Built only with
// -DSHOCKWAVE_FBXSDK=ON; a manual gate on a machine with the SDK, not part of CI.
//   fbx_sdk_import_test <repo> [more .fbx files to compare]
#include <Assets/Model.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

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

std::set<std::string> jointNames(const ModelData& model) {
    std::set<std::string> names;
    for (const auto& mesh : model.meshes)
        for (const auto& bone : mesh.bones) names.insert(model.nodes[bone.node].name);
    return names;
}

void compare(const fs::path& file) {
    const std::string name = file.filename().string();
    ModelData sdk, assimp;
    std::string error;
    if (!loadModel(file, sdk, error, nullptr, FbxImporter::Sdk)) {
        check(false, name + ": the FBX SDK can't read it: " + error);
        return;
    }
    if (!loadModel(file, assimp, error, nullptr, FbxImporter::Assimp)) {
        check(false, name + ": Assimp can't read it: " + error);
        return;
    }
    check(sdk.importer == "fbxsdk" && assimp.importer == "assimp", name + ": importer names");
    size_t sdkVertices = 0, assimpVertices = 0;
    for (const auto& mesh : sdk.meshes) sdkVertices += mesh.data.vertices.size();
    for (const auto& mesh : assimp.meshes) assimpVertices += mesh.data.vertices.size();
    check(
        sdkVertices == assimpVertices, name + ": vertices " + std::to_string(sdkVertices) + " (SDK) vs " +
                                           std::to_string(assimpVertices) + " (Assimp)");
    check(sdk.triangles == assimp.triangles, name + ": triangle counts");
    // Both in centimetres: 1 mm is 0.1.
    const float gap =
        std::max(glm::length(sdk.bounds.min - assimp.bounds.min), glm::length(sdk.bounds.max - assimp.bounds.max));
    check(gap <= 0.1f, name + ": bounds differ by " + std::to_string(gap * 10.0f) + " mm");
    check(jointNames(sdk) == jointNames(assimp), name + ": joint names");
    check(sdk.animations.size() == assimp.animations.size(), name + ": clip counts");
    for (size_t a = 0; a < std::min(sdk.animations.size(), assimp.animations.size()); ++a)
        check(
            std::abs(sdk.animations[a].duration - assimp.animations[a].duration) < 1.0f / 60.0f,
            name + ": clip " + sdk.animations[a].name + " lasts " + std::to_string(sdk.animations[a].duration) +
                " s (SDK) vs " + std::to_string(assimp.animations[a].duration) + " s (Assimp)");
    std::printf(
        "%s: %zu vertices, %llu triangles, %zu joints, %zu clips; bounds within %.2f mm\n", name.c_str(), sdkVertices,
        static_cast<unsigned long long>(sdk.triangles), jointNames(sdk).size(), sdk.animations.size(),
        static_cast<double>(gap * 10.0f));
}
}   // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    check(fbxSdkAvailable(), "this build has the FBX SDK importer");
    const fs::path fixtures = fs::path(argv[1]) / "Tests/fixtures/models/fbx";
    compare(fixtures / "crate.fbx");
    compare(fixtures / "arm.fbx");
    for (int i = 2; i < argc; ++i) compare(argv[i]);
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
