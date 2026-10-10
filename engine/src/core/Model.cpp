#include <shockwave/Model.h>
#include <assimp/Importer.hpp>
#include <assimp/DefaultIOSystem.h>
#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace shockwave {
namespace {
namespace fs = std::filesystem;
std::string extension(const fs::path& p) {
    std::string s = p.extension().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
void dependency(std::vector<fs::path>& list, const fs::path& path) {
    const auto p = fs::absolute(path).lexically_normal();
    if (std::find(list.begin(), list.end(), p) == list.end()) list.push_back(p);
}
class TrackingIO final : public Assimp::IOSystem {
public:
    explicit TrackingIO(std::vector<fs::path>& dependencies) : m_dependencies(dependencies) {}
    bool Exists(const char* p) const override { return m_io.Exists(p); }
    char getOsSeparator() const override { return m_io.getOsSeparator(); }
    Assimp::IOStream* Open(const char* p, const char* mode = "rb") override {
        dependency(m_dependencies, fs::path(p));
        return m_io.Open(p, mode);
    }
    void Close(Assimp::IOStream* stream) override { m_io.Close(stream); }
private:
    Assimp::DefaultIOSystem m_io;
    std::vector<fs::path>& m_dependencies;
};
glm::mat4 matrix(const aiMatrix4x4& m) {
    return glm::mat4(m.a1,m.b1,m.c1,m.d1, m.a2,m.b2,m.c2,m.d2,
                     m.a3,m.b3,m.c3,m.d3, m.a4,m.b4,m.c4,m.d4);
}
glm::vec3 vector(const aiVector3D& v) { return {v.x,v.y,v.z}; }
void expand(Bounds& b, const glm::vec3& p) { b.min = glm::min(b.min,p); b.max = glm::max(b.max,p); }
Bounds emptyBounds() { return {glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())}; }

// Temporary files are unique and removed after import, including on failure.
struct Temporary {
    fs::path root;
    ~Temporary() { std::error_code ec; if (!root.empty()) fs::remove_all(root,ec); }
};
bool convertBlend(const fs::path& source, Temporary& temp, fs::path& result, std::string& diagnostic) {
    static std::atomic<uint64_t> serial{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int retry = 0; retry < 8; ++retry) {
        temp.root = fs::temp_directory_path() / ("shockwave-blend-" + std::to_string(stamp) + "-" + std::to_string(serial++));
        if (fs::create_directory(temp.root)) break;
        temp.root.clear();
    }
    if (temp.root.empty()) throw std::runtime_error("Could not create Blender conversion directory");
    const auto script = temp.root / "export.py";
    result = temp.root / "model.glb";
    std::ofstream out(script);
    out << R"PY(import bpy, sys, pathlib
output = sys.argv[sys.argv.index('--') + 1]
dependencies = set()
for image in bpy.data.images:
    if image.filepath and not image.packed_file:
        dependencies.add(bpy.path.abspath(image.filepath, library=image.library))
for library in bpy.data.libraries:
    dependencies.add(bpy.path.abspath(library.filepath))
pathlib.Path(output).with_suffix('.deps').write_text('\n'.join(sorted(dependencies)), encoding='utf-8')
bpy.ops.export_scene.gltf(filepath=output, export_format='GLB', use_selection=False,
                          export_yup=True, export_apply=True, export_animations=False)
)PY";
    out.close();
    if (!out) throw std::runtime_error("Could not write Blender export script");
    const char* configured = SDL_getenv("SHOCKWAVE_BLENDER");
    const std::string exe = configured && *configured ? configured : "blender";
    const std::string src = fs::absolute(source).string(), py = script.string(), dst = result.string();
    const char* args[] = {exe.c_str(), "--background", "--disable-autoexec", src.c_str(),
                          "--python-exit-code", "1", "--python", py.c_str(), "--", dst.c_str(), nullptr};
    SDL_Process* process = SDL_CreateProcess(args, false);
    if (!process) {
        diagnostic = "Install Blender on PATH or set SHOCKWAVE_BLENDER to its executable for modern .blend files (" + std::string(SDL_GetError()) + ")";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    int exitCode = -1;
    bool complete = false;
    while (!(complete = SDL_WaitProcess(process, false, &exitCode)) && std::chrono::steady_clock::now() < deadline)
        SDL_Delay(20);
    if (!complete) { SDL_KillProcess(process, true); SDL_WaitProcess(process, true, &exitCode); }
    SDL_DestroyProcess(process);
    if (!complete || exitCode != 0 || !fs::is_regular_file(result)) {
        diagnostic = complete ? "Blender could not export this file to GLB; see Blender's console output" : "Blender conversion timed out after 120 seconds";
        return false;
    }
    return true;
}
}

std::string modelExtensions() {
    static const std::string list = [] { Assimp::Importer importer; aiString extensions; importer.GetExtensionList(extensions); return std::string(extensions.C_Str()); }();
    return list;
}
bool isModelFile(const std::filesystem::path& path) {
    const std::string ext = extension(path);
    if (ext.empty()) return false;
    const std::string list = ";" + modelExtensions() + ";";
    return list.find(";*" + ext + ";") != std::string::npos;
}

bool loadModel(const std::filesystem::path& path, ModelData& destination, std::string& error,
               std::vector<std::filesystem::path>* attemptedDependencies) {
    error.clear();
    ModelData model;
    dependency(model.dependencies, path);
    auto fail = [&](const std::string& message) {
        error = message;
        if (attemptedDependencies) *attemptedDependencies = model.dependencies;
        return false;
    };
    try {
        Temporary temp;
        fs::path importedPath = fs::absolute(path);
        std::string blendDiagnostic;
        bool convertedBlend=false;
        const bool blend = extension(path) == ".blend";
        if (blend) {
            fs::path converted;
            if (convertBlend(path,temp,converted,blendDiagnostic)) {
                importedPath = converted;
                convertedBlend=true;
                std::ifstream dependencyFile(converted.parent_path()/"model.deps");
                std::string line;
                while (std::getline(dependencyFile,line)) if (!line.empty()) dependency(model.dependencies,fs::path(line));
                model.warnings.emplace_back("Blender converted this file to static GLB; animation playback is not implemented yet");
            } else model.warnings.emplace_back(blendDiagnostic + "; trying Assimp's deprecated native .blend reader");
        }
        Assimp::Importer importer;
        importer.SetIOHandler(new TrackingIO(model.dependencies)); // importer takes ownership
        const aiScene* scene = importer.ReadFile(importedPath.string(), aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals | aiProcess_ImproveCacheLocality |
            aiProcess_SortByPType | aiProcess_ValidateDataStructure | aiProcess_FlipUVs);
        if (!scene || !scene->mRootNode || !scene->HasMeshes())
            return fail(std::string(importer.GetErrorString()) + (blend ? ". " + blendDiagnostic + "; export glTF/GLB from Blender as an alternative" : ""));
        if (scene->HasAnimations()) model.warnings.emplace_back("Animations were found; this milestone renders the static pose only");
        std::unordered_map<std::string,int> textureCache;
        auto texture = [&](const aiMaterial& material, aiTextureType type) -> int {
            aiString source;
            unsigned uv = 0;
            if (material.GetTexture(type,0,&source,nullptr,&uv) != AI_SUCCESS) return -1;
            const std::string key = source.C_Str();
            if (auto it = textureCache.find(key); it != textureCache.end()) return it->second;
            if (uv != 0) model.warnings.emplace_back("Texture " + key + " uses a UV channel other than UV0; UV0 is used by the current renderer");
            ModelTexture tex;
            tex.source = key;
            std::string reason;
            if (const aiTexture* embedded = scene->GetEmbeddedTexture(source.C_Str())) {
                if (embedded->mHeight == 0) {
                    if (!decodeImage({reinterpret_cast<const uint8_t*>(embedded->pcData),embedded->mWidth},tex.image,reason))
                        throw std::runtime_error("embedded texture " + key + ": " + reason);
                } else {
                    tex.image.width = embedded->mWidth; tex.image.height = embedded->mHeight;
                    tex.image.rgba.resize(size_t(embedded->mWidth)*embedded->mHeight*4);
                    for (size_t i=0;i<size_t(embedded->mWidth)*embedded->mHeight;++i) {
                        const aiTexel& p = embedded->pcData[i];
                        std::copy_n(std::array<uint8_t,4>{p.r,p.g,p.b,p.a}.data(),4,tex.image.rgba.data()+i*4);
                    }
                }
            } else {
                std::string portable = key;
                std::replace(portable.begin(),portable.end(),'\\','/');
                const fs::path file = (importedPath.parent_path() / fs::path(portable)).lexically_normal();
                dependency(model.dependencies,file);
                tex.source = file.generic_string();
                if (!loadImage(file,tex.image,reason)) throw std::runtime_error("texture " + file.string() + ": " + reason);
            }
            const int index = static_cast<int>(model.textures.size());
            model.textures.push_back(std::move(tex));
            textureCache.emplace(key,index);
            return index;
        };
        std::vector<bool> usedMaterial(scene->mNumMaterials,false);
        for (unsigned i=0;i<scene->mNumMeshes;++i) {
            const auto& mesh=*scene->mMeshes[i];
            if ((mesh.mPrimitiveTypes & aiPrimitiveType_TRIANGLE) && mesh.mMaterialIndex<usedMaterial.size())
                usedMaterial[mesh.mMaterialIndex]=true;
        }
        model.materials.resize(scene->mNumMaterials);
        for (unsigned i=0;i<scene->mNumMaterials;++i) {
            if (!usedMaterial[i]) continue;
            const aiMaterial& src = *scene->mMaterials[i];
            ModelMaterial m;
            aiString name; src.Get(AI_MATKEY_NAME,name); m.name=name.C_Str();
            aiColor4D color(1,1,1,1);
            const bool pbr = src.Get(AI_MATKEY_BASE_COLOR,color) == AI_SUCCESS;
            if (!pbr) src.Get(AI_MATKEY_COLOR_DIFFUSE,color);
            m.baseColor = {color.r,color.g,color.b,color.a};
            float opacity=1; if (!pbr && src.Get(AI_MATKEY_OPACITY,opacity)==AI_SUCCESS) m.baseColor.a *= opacity;
            aiColor3D emission(0,0,0); src.Get(AI_MATKEY_COLOR_EMISSIVE,emission);
            m.emissive = {emission.r,emission.g,emission.b};
            src.Get(AI_MATKEY_METALLIC_FACTOR,m.metallic); src.Get(AI_MATKEY_ROUGHNESS_FACTOR,m.roughness);
            int doubleSided=0; src.Get(AI_MATKEY_TWOSIDED,doubleSided); m.doubleSided=doubleSided!=0;
            aiString alpha; src.Get(AI_MATKEY_GLTF_ALPHAMODE,alpha);
            if (std::string(alpha.C_Str())=="MASK") m.alpha=ModelMaterial::Alpha::Mask;
            else if (std::string(alpha.C_Str())=="BLEND" || (alpha.length==0 && m.baseColor.a < 0.999f)) m.alpha=ModelMaterial::Alpha::Blend;
            src.Get(AI_MATKEY_GLTF_ALPHACUTOFF,m.alphaCutoff);
            m.baseColorTexture=texture(src,aiTextureType_BASE_COLOR);
            if (m.baseColorTexture<0) m.baseColorTexture=texture(src,aiTextureType_DIFFUSE);
            m.normalTexture=texture(src,aiTextureType_NORMALS);
            m.metallicRoughnessTexture=texture(src,aiTextureType_GLTF_METALLIC_ROUGHNESS);
            m.emissiveTexture=texture(src,aiTextureType_EMISSIVE);
            m.occlusionTexture=texture(src,aiTextureType_AMBIENT_OCCLUSION);
            if (m.normalTexture>=0 || m.metallicRoughnessTexture>=0 || m.emissiveTexture>=0 || m.occlusionTexture>=0 || m.metallic>0 || glm::length(m.emissive)>0)
                model.warnings.emplace_back("Material '"+m.name+"': PBR factors/maps are retained; full PBR lighting is a later milestone");
            model.materials[i]=std::move(m);
        }
        if (model.materials.empty()) model.materials.emplace_back();
        // Assimp mesh indices are retained, including empty slots for non-triangle primitives.
        model.meshes.resize(scene->mNumMeshes);
        for (unsigned i=0;i<scene->mNumMeshes;++i) {
            const aiMesh& src = *scene->mMeshes[i];
            ModelMesh& mesh=model.meshes[i];
            mesh.name=src.mName.C_Str(); mesh.material=src.mMaterialIndex;
            if (mesh.material>=model.materials.size()) throw std::runtime_error("Invalid material index");
            mesh.bounds=emptyBounds();
            if (src.HasBones()) model.warnings.emplace_back("Mesh '"+mesh.name+"' has skinning; rendered in its static pose");
            if (!(src.mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
            mesh.data.vertices.reserve(src.mNumVertices);
            for (unsigned v=0;v<src.mNumVertices;++v) {
                Vertex vertex;
                vertex.position=vector(src.mVertices[v]);
                if (!std::isfinite(vertex.position.x)||!std::isfinite(vertex.position.y)||!std::isfinite(vertex.position.z))
                    throw std::runtime_error("Mesh has non-finite vertex positions");
                if (src.HasNormals()) vertex.normal=vector(src.mNormals[v]);
                if (src.HasTextureCoords(0)) vertex.uv={src.mTextureCoords[0][v].x,src.mTextureCoords[0][v].y};
                if (src.HasVertexColors(0)) { const auto& c=src.mColors[0][v]; vertex.color={c.r,c.g,c.b,c.a}; }
                expand(mesh.bounds,vertex.position); mesh.data.vertices.push_back(vertex);
            }
            for (unsigned f=0;f<src.mNumFaces;++f) {
                const aiFace& face=src.mFaces[f];
                if (face.mNumIndices!=3) continue;
                for (unsigned k=0;k<3;++k) {
                    if (face.mIndices[k]>=mesh.data.vertices.size()) throw std::runtime_error("Invalid triangle index");
                    mesh.data.indices.push_back(face.mIndices[k]);
                }
                ++model.triangles;
            }
        }
        // Assimp sometimes appends an unused fallback material. Keep only actual
        // triangle materials so the browser/inspector represent source parts.
        std::vector<ModelMaterial> materials;
        std::vector<int> remap(model.materials.size(),-1);
        for (auto& mesh:model.meshes) {
            if (mesh.data.indices.empty()) continue;
            auto& mapped=remap[mesh.material];
            if (mapped<0) { mapped=static_cast<int>(materials.size()); materials.push_back(std::move(model.materials[mesh.material])); }
            mesh.material=static_cast<uint32_t>(mapped);
        }
        model.materials=std::move(materials);
        model.bounds=emptyBounds();
        bool hasGeometry=false;
        std::function<void(const aiNode&,int,const glm::mat4&,int)> visit;
        visit = [&](const aiNode& src,int parent,const glm::mat4& parentMatrix,int depth) {
            if (depth>256) throw std::runtime_error("Model hierarchy exceeds 256 levels");
            ModelNode node;
            node.name=src.mName.C_Str(); node.parent=parent; node.local=matrix(src.mTransformation);
            if (depth==0 && blend && !convertedBlend) {
                // Native Blender data is Z-up. GLB exports already use Y-up.
                const glm::mat4 zToY(1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1);
                node.local=zToY*node.local;
            }
            node.world=parentMatrix*node.local;
            for (unsigned i=0;i<src.mNumMeshes;++i) {
                const uint32_t index=src.mMeshes[i];
                if (index>=model.meshes.size()) throw std::runtime_error("Invalid node mesh reference");
                const auto& mesh=model.meshes[index];
                if (mesh.data.indices.empty()) continue;
                node.meshes.push_back(index);
                for (int corner=0;corner<8;++corner) {
                    const glm::vec3 p((corner&1)?mesh.bounds.max.x:mesh.bounds.min.x,
                        (corner&2)?mesh.bounds.max.y:mesh.bounds.min.y,(corner&4)?mesh.bounds.max.z:mesh.bounds.min.z);
                    const auto world = glm::vec3(node.world*glm::vec4(p,1));
                    if (!std::isfinite(world.x)||!std::isfinite(world.y)||!std::isfinite(world.z)) throw std::runtime_error("Non-finite model transform");
                    expand(model.bounds,world);
                }
                hasGeometry=true;
            }
            const int index=static_cast<int>(model.nodes.size());
            const auto world=node.world;
            model.nodes.push_back(std::move(node));
            for (unsigned i=0;i<src.mNumChildren;++i) visit(*src.mChildren[i],index,world,depth+1);
        };
        visit(*scene->mRootNode,-1,glm::mat4(1),0);
        if (!hasGeometry) return fail("Model contains no drawable triangles");
        if (!temp.root.empty()) std::erase_if(model.dependencies,[&](const fs::path& p) { return p.string().starts_with(temp.root.string()); });
        if (attemptedDependencies) *attemptedDependencies=model.dependencies;
        destination=std::move(model);
        return true;
    } catch (const std::exception& e) { return fail(e.what()); }
}

namespace {
bool rayBounds(const Bounds& bounds,const glm::vec3& o,const glm::vec3& d,float limit) {
    float lo=0,hi=limit;
    for (int axis=0;axis<3;++axis) {
        if (std::abs(d[axis])<1e-10f) { if (o[axis]<bounds.min[axis] || o[axis]>bounds.max[axis]) return false; continue; }
        float a=(bounds.min[axis]-o[axis])/d[axis],b=(bounds.max[axis]-o[axis])/d[axis];
        if (a>b) std::swap(a,b);
        lo=std::max(lo,a);hi=std::min(hi,b);
        if (hi<lo) return false;
    }
    return true;
}
}

bool intersectModel(const ModelData& model, const glm::vec3& origin, const glm::vec3& direction, float& distance) {
    float best=std::numeric_limits<float>::max();
    if (!rayBounds(model.bounds,origin,direction,best)) return false;
    for (const auto& node:model.nodes) {
        if (std::abs(glm::determinant(node.world))<1e-10f) continue;
        const auto inverse=glm::inverse(node.world);
        const auto o=glm::vec3(inverse*glm::vec4(origin,1)), d=glm::vec3(inverse*glm::vec4(direction,0));
        for (uint32_t index:node.meshes) {
            if (!rayBounds(model.meshes[index].bounds,o,d,best)) continue;
            const auto& mesh=model.meshes[index].data;
            for (size_t i=0;i+2<mesh.indices.size();i+=3) {
                const auto a=mesh.vertices[mesh.indices[i]].position, b=mesh.vertices[mesh.indices[i+1]].position, c=mesh.vertices[mesh.indices[i+2]].position;
                const auto e1=b-a,e2=c-a,p=glm::cross(d,e2);
                const float det=glm::dot(e1,p);
                if (std::abs(det)<1e-8f) continue;
                const auto s=o-a; const float u=glm::dot(s,p)/det;
                if (u<0 || u>1) continue;
                const auto q=glm::cross(s,e1); const float v=glm::dot(d,q)/det;
                if (v<0 || u+v>1) continue;
                const float t=glm::dot(e2,q)/det;
                if (t>=0 && t<best) best=t;
            }
        }
    }
    if (best==std::numeric_limits<float>::max()) return false;
    distance=best; return true;
}
} // namespace shockwave
