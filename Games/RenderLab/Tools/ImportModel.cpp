// Static library assets -> standard, inspectable glTF + binary vertex buffer.
// Uses Shockwave's own importer; never changes originals. Texture rebinding is explicit
// because the FBXs contain author-machine paths and legacy Phong materials.
#include <Assets/Model.h>

#include <bit>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>

namespace fs = std::filesystem;
int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: renderlab_import source.fbx output.gltf report.json\n";
        return 1;
    }
    static_assert(std::endian::native == std::endian::little, "glTF buffers require little endian");
    shockwave::ModelData source;
    std::string error;
    if (!shockwave::loadModel(argv[1], source, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (shockwave::hasSkinnedMeshes(source) || !source.animations.empty()) {
        std::cerr << "This static stress-test converter refuses animated/skinned content\n";
        return 1;
    }
    for (const auto& warning : source.warnings) std::cerr << "source: " << warning << '\n';
    const fs::path output(argv[2]);
    if (fs::absolute(output).parent_path().lexically_normal() ==
        fs::absolute(argv[1]).parent_path().lexically_normal()) {
        std::cerr << "Use a separate output folder to preserve source files\n";
        return 1;
    }
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    if (ec) {
        std::cerr << ec.message() << '\n';
        return 1;
    }
    std::ofstream bin(output.parent_path() / "geometry.bin", std::ios::binary);
    std::ostringstream views, accessors, meshes, nodes, roots;
    for (auto* stream : {&views, &accessors, &meshes, &nodes, &roots}) {
        stream->imbue(std::locale::classic());
        *stream << std::setprecision(9);
    }
    size_t offset = 0;
    for (size_t i = 0; i < source.meshes.size(); ++i) {
        const auto& mesh = source.meshes[i];
        const auto& vertices = mesh.data.vertices;
        const auto& indices = mesh.data.indices;
        if (vertices.empty() || indices.empty()) {
            std::cerr << "empty triangle mesh\n";
            return 1;
        }
        const size_t bytes = vertices.size() * sizeof(shockwave::Vertex);
        bin.write(reinterpret_cast<const char*>(vertices.data()), static_cast<std::streamsize>(bytes));
        if (i) views << ',', accessors << ',', meshes << ',';
        views << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << bytes
              << ",\"byteStride\":64,\"target\":34962},";
        offset += bytes;
        const size_t indexBytes = indices.size() * sizeof(uint32_t);
        bin.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(indexBytes));
        views << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << indexBytes << ",\"target\":34963}";
        offset += indexBytes;
        constexpr size_t offsets[] = {
            offsetof(shockwave::Vertex, position), offsetof(shockwave::Vertex, normal), offsetof(shockwave::Vertex, uv),
            offsetof(shockwave::Vertex, color), offsetof(shockwave::Vertex, tangent)};
        constexpr const char* types[] = {"VEC3", "VEC3", "VEC2", "VEC4", "VEC4"};
        for (size_t a = 0; a < 5; ++a) {
            if (a) accessors << ',';
            accessors << "{\"bufferView\":" << i * 2 << ",\"byteOffset\":" << offsets[a]
                      << ",\"componentType\":5126,\"count\":" << vertices.size() << ",\"type\":\"" << types[a] << "\"";
            if (a == 0)
                accessors << ",\"min\":[" << mesh.bounds.min.x << ',' << mesh.bounds.min.y << ',' << mesh.bounds.min.z
                          << "],\"max\":[" << mesh.bounds.max.x << ',' << mesh.bounds.max.y << ',' << mesh.bounds.max.z
                          << ']';
            accessors << '}';
        }
        accessors << ",{\"bufferView\":" << i * 2 + 1 << ",\"componentType\":5125,\"count\":" << indices.size()
                  << ",\"type\":\"SCALAR\"}";
        meshes << "{\"primitives\":[{\"attributes\":{\"POSITION\":" << i * 6 << ",\"NORMAL\":" << i * 6 + 1
               << ",\"TEXCOORD_0\":" << i * 6 + 2 << ",\"COLOR_0\":" << i * 6 + 3 << ",\"TANGENT\":" << i * 6 + 4
               << "},\"indices\":" << i * 6 + 5 << ",\"material\":0}]}";
    }
    size_t count = 0;
    for (const auto& node : source.nodes)
        for (uint32_t mesh : node.meshes) {
            if (count) nodes << ',', roots << ',';
            roots << count++;
            nodes << "{\"mesh\":" << mesh << ",\"matrix\":[";
            for (int i = 0; i < 16; ++i) {
                if (i) nodes << ',';
                nodes << node.world[i / 4][i % 4];
            }
            nodes << "]}";
        }
    const bool textured = fs::exists(output.parent_path() / "basecolor.png");
    std::ofstream gltf(output);
    gltf << "{\n\"asset\":{\"version\":\"2.0\",\"generator\":\"Shockwave RenderLab static importer\"},\n"
         << "\"buffers\":[{\"uri\":\"geometry.bin\",\"byteLength\":" << offset << "}],\n"
         << "\"bufferViews\":[" << views.str() << "],\n\"accessors\":[" << accessors.str() << "],\n"
         << "\"meshes\":[" << meshes.str() << "],\n\"nodes\":[" << nodes.str() << "],\n"
         << "\"scenes\":[{\"nodes\":[" << roots.str() << "]}],\"scene\":0,\n";
    if (textured)
        gltf << R"JSON("images":[{"uri":"basecolor.png"},{"uri":"normal.png"},{"uri":"metallic-roughness.png"}],
"textures":[{"source":0},{"source":1},{"source":2}],
"materials":[{"name":"Library UV-matched PBR","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":2},"metallicFactor":1,"roughnessFactor":1},"normalTexture":{"index":1}}]
)JSON";
    else
        gltf << R"JSON("materials":[{"name":"Neutral metal (kit has no maps)", "pbrMetallicRoughness"
            : {"baseColorFactor" : [ 0.45, 0.5, 0.55, 1 ], "metallicFactor" : 0.8, "roughnessFactor" : 0.35}
}]
)JSON";
    gltf << "}\n";
    gltf.close();
    bin.close();
    if (!gltf || !bin) {
        std::cerr << "failed writing converted model\n";
        return 1;
    }
    shockwave::ModelData model;
    if (!shockwave::loadModel(output, model, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    for (const auto& warning : model.warnings) std::cerr << warning << '\n';
    if (!model.warnings.empty()) return 1;
    uint64_t vertices = 0;
    for (const auto& mesh : model.meshes) vertices += mesh.data.vertices.size();
    std::ofstream report(argv[3]);
    report.imbue(std::locale::classic());
    report << std::setprecision(9);
    report << "{\"triangles\":" << model.triangles << ",\"vertices\":" << vertices
           << ",\"meshes\":" << model.meshes.size() << ",\"materials\":" << model.materials.size()
           << ",\"textures\":" << model.textures.size() << ",\"min\":[" << model.bounds.min.x << ','
           << model.bounds.min.y << ',' << model.bounds.min.z << "],\"max\":[" << model.bounds.max.x << ','
           << model.bounds.max.y << ',' << model.bounds.max.z << "]}\n";
    report.close();
    return report ? 0 : 1;
}
