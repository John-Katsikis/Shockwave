// The optional Autodesk FBX SDK importer (F1b, docs/Assets.md). Only compiled with
// -DSHOCKWAVE_FBXSDK=ON (cmake/FbxSdk.cmake): the SDK is proprietary, every developer
// downloads it from Autodesk, and it's never committed, fetched or shipped.
//
// It fills ModelData exactly as Model.cpp's Assimp path does, so a model looks the same
// whichever importer a teammate's build has, except where the SDK reads the file better.
// The shared parts (texture lookup, tangents, skinned bounds) are in ModelImport.h.
#include "FbxSdkImport.h"

#include "ModelImport.h"

#include <Assets/Image.h>

#include <fbxsdk.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace shockwave {
namespace {
namespace fs = std::filesystem;
using modelimport::dependency;
using modelimport::emptyBounds;
using modelimport::expand;

std::string toUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path fromUtf8(const char* p) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(p))); }

// FbxAMatrix keeps the translation in its fourth row, which is glm's fourth column.
glm::mat4 toGlm(const FbxAMatrix& m) {
    glm::mat4 out;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = static_cast<float>(m.Get(i, j));
    return out;
}
glm::vec3 toGlm(const FbxVector4& v) { return {float(v[0]), float(v[1]), float(v[2])}; }

// Owns the SDK objects of one import.
struct Sdk {
    FbxManager* manager = FbxManager::Create();
    ~Sdk() {
        if (manager) manager->Destroy();
    }
};

// One value of a layer element (normals, UVs, colours, tangents) for a polygon corner.
template <class T>
bool elementValue(const FbxLayerElementTemplate<T>* element, int controlPoint, int corner, int polygon, T& value) {
    if (!element) return false;
    int index = 0;
    switch (element->GetMappingMode()) {
    case FbxLayerElement::eByControlPoint: index = controlPoint; break;
    case FbxLayerElement::eByPolygonVertex: index = corner; break;
    case FbxLayerElement::eByPolygon: index = polygon; break;
    case FbxLayerElement::eAllSame: index = 0; break;
    default: return false;
    }
    if (element->GetReferenceMode() != FbxLayerElement::eDirect) {
        if (index < 0 || index >= element->GetIndexArray().GetCount()) return false;
        index = element->GetIndexArray().GetAt(index);
    }
    if (index < 0 || index >= element->GetDirectArray().GetCount()) return false;
    value = element->GetDirectArray().GetAt(index);
    return true;
}

// The first file texture behind a material property, directly or in a layered texture.
FbxFileTexture* fileTexture(const FbxProperty& property) {
    if (!property.IsValid()) return nullptr;
    if (property.GetSrcObjectCount<FbxFileTexture>() > 0) return property.GetSrcObject<FbxFileTexture>(0);
    for (int l = 0; l < property.GetSrcObjectCount<FbxLayeredTexture>(); ++l) {
        const FbxLayeredTexture* layered = property.GetSrcObject<FbxLayeredTexture>(l);
        if (layered && layered->GetSrcObjectCount<FbxFileTexture>() > 0)
            return layered->GetSrcObject<FbxFileTexture>(0);
    }
    return nullptr;
}

class Importer {
public:
    Importer(ModelData& model, const fs::path& file, const fs::path& extractFolder)
        : m_model(model), m_file(file), m_extract(extractFolder) {
        m_modelFolder = fs::absolute(file).lexically_normal().parent_path();
    }

    void run(FbxScene& scene) {
        // Nodes first, parent first, so meshes and skins can name them by index.
        visitNodes(scene.GetRootNode(), -1, 0);
        for (size_t n = 0; n < m_nodes.size(); ++n) readMeshes(*m_nodes[n], static_cast<uint32_t>(n));
        if (m_model.materials.empty()) m_model.materials.emplace_back();
        bounds();
        readTakes(scene);
    }

private:
    void visitNodes(FbxNode* node, int parent, int depth) {
        if (!node) return;
        if (depth > 256) throw std::runtime_error("Model hierarchy exceeds 256 levels");
        ModelNode out;
        out.name = node->GetName();
        out.parent = parent;
        // Pivots, pre- and post-rotations included; the take isn't (FBXSDK_TIME_INFINITE).
        out.local = toGlm(node->EvaluateLocalTransform(FBXSDK_TIME_INFINITE));
        out.world = parent >= 0 ? m_model.nodes[size_t(parent)].world * out.local : out.local;
        const int index = static_cast<int>(m_model.nodes.size());
        m_index.emplace(node, static_cast<uint32_t>(index));
        m_nodes.push_back(node);
        m_model.nodes.push_back(std::move(out));
        for (int c = 0; c < node->GetChildCount(); ++c) visitNodes(node->GetChild(c), index, depth + 1);
    }

    int material(FbxSurfaceMaterial* source) {
        if (!source) return -1;
        if (auto it = m_materials.find(source); it != m_materials.end()) return it->second;
        ModelMaterial m;
        m.name = source->GetName();
        // As Assimp reads FBX: colours times their factors, opacity from the legacy
        // Opacity value or the transparent colour, roughness from the shininess exponent.
        auto color = [&](const char* name, const char* factor, glm::vec3& out) {
            const FbxProperty p = source->FindProperty(name);
            if (!p.IsValid()) return false;
            const FbxDouble3 c = p.Get<FbxDouble3>();
            out = {float(c[0]), float(c[1]), float(c[2])};
            if (const FbxProperty f = source->FindProperty(factor); f.IsValid()) out *= float(f.Get<FbxDouble>());
            return true;
        };
        glm::vec3 diffuse(1.0f);
        color(FbxSurfaceMaterial::sDiffuse, FbxSurfaceMaterial::sDiffuseFactor, diffuse);
        m.baseColor = glm::vec4(diffuse, 1.0f);
        color(FbxSurfaceMaterial::sEmissive, FbxSurfaceMaterial::sEmissiveFactor, m.emissive);
        float opacity = 1.0f;
        glm::vec3 transparent(0.0f);
        if (const FbxProperty p = source->FindProperty("Opacity"); p.IsValid())
            opacity = float(p.Get<FbxDouble>());
        else if (color(FbxSurfaceMaterial::sTransparentColor, FbxSurfaceMaterial::sTransparencyFactor, transparent))
            opacity = 1.0f - (transparent.r + transparent.g + transparent.b) / 3.0f;
        m.baseColor.a *= opacity;
        if (m.baseColor.a < 0.999f) m.alpha = ModelMaterial::Alpha::Blend;
        if (const FbxProperty p = source->FindProperty(FbxSurfaceMaterial::sShininess); p.IsValid())
            m.roughness = 1.0f - std::sqrt(float(p.Get<FbxDouble>())) / 10.0f;
        // The slots Assimp's FBX reader fills and the renderer reads.
        auto slot = [&](std::initializer_list<const char*> names, bool srgb) {
            for (const char* name : names)
                if (FbxFileTexture* t = fileTexture(source->FindProperty(name))) return texture(t, srgb);
            return -1;
        };
        m.baseColorTexture = slot({"Maya|baseColor", "Maya|TEX_color_map"}, true);
        if (m.baseColorTexture < 0)
            m.baseColorTexture = slot({FbxSurfaceMaterial::sDiffuse, "Maya|DiffuseTexture"}, true);
        m.normalTexture = slot({FbxSurfaceMaterial::sNormalMap, "Maya|NormalTexture"}, false);
        m.emissiveTexture = slot({FbxSurfaceMaterial::sEmissive, FbxSurfaceMaterial::sEmissiveFactor}, true);
        const int index = static_cast<int>(m_model.materials.size());
        m_model.materials.push_back(std::move(m));
        m_materials.emplace(source, index);
        return index;
    }

    int texture(FbxFileTexture* source, bool srgb) {
        const auto cacheKey = std::make_pair(source, srgb);
        if (auto it = m_textures.find(cacheKey); it != m_textures.end()) return it->second;
        // The path as saved, as Assimp takes it: the relative one, else the absolute one.
        const char* relative = source->GetRelativeFileName();
        const std::string saved = relative && *relative ? relative : source->GetFileName();
        ModelTexture tex;
        tex.srgb = srgb;
        std::optional<fs::path> file;
        // Media embedded in the file were unpacked into the temporary folder.
        const fs::path unpacked = fromUtf8(source->GetFileName()).lexically_normal();
        std::error_code ec;
        if (!m_extract.empty() && toUtf8(unpacked).starts_with(toUtf8(m_extract.lexically_normal())) &&
            fs::is_regular_file(unpacked, ec)) {
            file = unpacked;
            tex.source = saved;
        } else {
            file = modelimport::locateTexture(m_model, saved, m_modelFolder, fs::absolute(m_file).parent_path());
            if (!file) {
                m_textures.emplace(cacheKey, -1);
                return -1;
            }
            const std::u8string generic = file->generic_u8string();
            tex.source.assign(generic.begin(), generic.end());
        }
        std::string reason;
        if (!loadImage(*file, tex.image, reason)) throw std::runtime_error("texture " + file->string() + ": " + reason);
        const int index = static_cast<int>(m_model.textures.size());
        m_model.textures.push_back(std::move(tex));
        m_textures.emplace(cacheKey, index);
        return index;
    }

    void readMeshes(FbxNode& node, uint32_t nodeIndex) {
        for (int a = 0; a < node.GetNodeAttributeCount(); ++a) {
            FbxNodeAttribute* attribute = node.GetNodeAttributeByIndex(a);
            if (!attribute || attribute->GetAttributeType() != FbxNodeAttribute::eMesh) continue;
            FbxMesh& source = *static_cast<FbxMesh*>(attribute);
            ModelMesh mesh;
            mesh.name = node.GetName();
            mesh.bounds = emptyBounds();
            // The material: after SplitMeshesPerMaterial, one for the whole mesh.
            int materialIndex = 0;
            if (const FbxGeometryElementMaterial* element = source.GetElementMaterial(0);
                element && element->GetIndexArray().GetCount() > 0)
                materialIndex = element->GetIndexArray().GetAt(0);
            int index = materialIndex >= 0 && materialIndex < node.GetMaterialCount()
                            ? material(node.GetMaterial(materialIndex))
                            : -1;
            if (index < 0) {
                if (m_defaultMaterial < 0) {
                    m_defaultMaterial = static_cast<int>(m_model.materials.size());
                    m_model.materials.emplace_back();
                }
                index = m_defaultMaterial;
            }
            mesh.material = static_cast<uint32_t>(index);
            if (!source.GetElementNormal(0)) source.GenerateNormals(true, false, false);
            if (readGeometry(node, source, mesh)) {
                m_model.nodes[nodeIndex].meshes.push_back(static_cast<uint32_t>(m_model.meshes.size()));
                m_model.meshes.push_back(std::move(mesh));
            }
        }
    }

    // Corners with the same control point, normal, UVs and colour become one vertex, as
    // Assimp's JoinIdenticalVertices makes them.
    bool readGeometry(FbxNode& node, FbxMesh& source, ModelMesh& mesh) {
        const FbxAMatrix geometryTransform(
            node.GetGeometricTranslation(FbxNode::eSourcePivot), node.GetGeometricRotation(FbxNode::eSourcePivot),
            node.GetGeometricScaling(FbxNode::eSourcePivot));
        const glm::mat4 geometry = toGlm(geometryTransform);
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(geometry)));
        const FbxVector4* points = source.GetControlPoints();
        const int pointCount = source.GetControlPointsCount();
        const FbxGeometryElementNormal* normals = source.GetElementNormal(0);
        const FbxGeometryElementUV* uv0 = source.GetElementUV(0);
        const FbxGeometryElementUV* uv1 = source.GetElementUV(1);
        const FbxGeometryElementVertexColor* colors = source.GetElementVertexColor(0);
        const FbxGeometryElementTangent* tangents = source.GetElementTangent(0);
        const FbxGeometryElementBinormal* binormals = source.GetElementBinormal(0);
        const bool fileTangents = tangents && binormals;
        struct Key {
            int point;
            float data[14];
            bool operator<(const Key& o) const {
                if (point != o.point) return point < o.point;
                return std::memcmp(data, o.data, sizeof(data)) < 0;
            }
        };
        std::map<Key, uint32_t> joined;
        std::vector<int> vertexPoint;   // control point of each vertex, for the skin
        std::vector<glm::vec3> tangentList, bitangentList;
        int corner = 0;
        for (int p = 0; p < source.GetPolygonCount(); ++p) {
            const int size = source.GetPolygonSize(p);
            uint32_t triangle[3] = {0, 0, 0};
            for (int k = 0; k < size; ++k, ++corner) {
                const int point = source.GetPolygonVertex(p, k);
                if (point < 0 || point >= pointCount) throw std::runtime_error("Invalid triangle index");
                Vertex vertex;
                vertex.position = glm::vec3(geometry * glm::vec4(toGlm(points[point]), 1.0f));
                if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) ||
                    !std::isfinite(vertex.position.z))
                    throw std::runtime_error("Mesh has non-finite vertex positions");
                FbxVector4 n;
                if (elementValue(normals, point, corner, p, n)) vertex.normal = normalMatrix * toGlm(n);
                FbxVector2 uv;
                // UVs flipped to a top-left origin, as Assimp's aiProcess_FlipUVs does.
                if (elementValue(uv0, point, corner, p, uv)) vertex.uv = {float(uv[0]), 1.0f - float(uv[1])};
                glm::vec2 second(0.0f);
                if (elementValue(uv1, point, corner, p, uv)) second = {float(uv[0]), 1.0f - float(uv[1])};
                FbxColor c;
                if (elementValue(colors, point, corner, p, c))
                    vertex.color = {float(c.mRed), float(c.mGreen), float(c.mBlue), float(c.mAlpha)};
                Key key{point, {}};
                const float values[14] = {
                    vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x,    vertex.uv.y, second.x, second.y,
                    vertex.color.r,  vertex.color.g,  vertex.color.b,  vertex.color.a, 0,           0,        0};
                std::copy(std::begin(values), std::end(values), key.data);
                auto [it, added] = joined.emplace(key, static_cast<uint32_t>(mesh.data.vertices.size()));
                if (added) {
                    expand(mesh.bounds, vertex.position);
                    mesh.data.vertices.push_back(vertex);
                    if (uv1) mesh.data.uv1.push_back(second);
                    vertexPoint.push_back(point);
                    if (fileTangents) {
                        FbxVector4 t, b;
                        elementValue(tangents, point, corner, p, t);
                        elementValue(binormals, point, corner, p, b);
                        tangentList.push_back(glm::mat3(geometry) * toGlm(t));
                        bitangentList.push_back(glm::mat3(geometry) * toGlm(b));
                    }
                }
                if (size == 3) triangle[k] = it->second;
            }
            if (size != 3) continue;   // Triangulate left a polygon it couldn't split
            mesh.data.indices.insert(mesh.data.indices.end(), std::begin(triangle), std::end(triangle));
            ++m_model.triangles;
        }
        if (mesh.data.indices.empty()) return false;
        modelimport::computeTangents(
            mesh, fileTangents ? tangentList.data() : nullptr, fileTangents ? bitangentList.data() : nullptr);
        const ModelMaterial& material = m_model.materials[mesh.material];
        const bool textured =
            material.baseColorTexture >= 0 || material.normalTexture >= 0 || material.emissiveTexture >= 0;
        if (!uv0 && textured)
            m_model.warnings.emplace_back(
                "Mesh '" + mesh.name + "' has no texture coordinates, so material '" + material.name +
                "' can't place its texture maps on it");
        if (source.GetDeformerCount(FbxDeformer::eBlendShape) > 0)
            m_model.warnings.emplace_back(
                "Mesh '" + mesh.name + "' has morph targets (blend shapes); they aren't played yet");
        readSkin(source, mesh, vertexPoint);
        return true;
    }

    // Each cluster that weights a vertex is a bone; a vertex keeps its four strongest
    // joints, weights adding up to 1, as the Assimp path does (readSkin in Model.cpp).
    void readSkin(FbxMesh& source, ModelMesh& mesh, const std::vector<int>& vertexPoint) {
        if (source.GetDeformerCount(FbxDeformer::eSkin) == 0) return;
        const FbxSkin* skin = static_cast<FbxSkin*>(source.GetDeformer(0, FbxDeformer::eSkin));
        if (!skin) return;
        const int pointCount = source.GetControlPointsCount();
        struct Weights {
            glm::u16vec4 joints{0};
            glm::vec4 weights{0.0f};
            int count = 0;
        };
        std::vector<Weights> points(size_t(std::max(pointCount, 0)));
        for (int c = 0; c < skin->GetClusterCount(); ++c) {
            FbxCluster* cluster = skin->GetCluster(c);
            if (!cluster || !cluster->GetLink() || cluster->GetControlPointIndicesCount() == 0) continue;
            const auto found = m_index.find(cluster->GetLink());
            if (found == m_index.end())
                throw std::runtime_error(
                    "Mesh '" + mesh.name + "' is skinned to a joint '" + cluster->GetLink()->GetName() +
                    "' that isn't in the model");
            if (mesh.bones.size() >= 65535)
                throw std::runtime_error("Mesh '" + mesh.name + "' has more than 65535 bones");
            FbxAMatrix meshBind, linkBind;
            cluster->GetTransformMatrix(meshBind);
            cluster->GetTransformLinkMatrix(linkBind);
            // Vertices already carry the geometric transform (readGeometry).
            const uint16_t bone = static_cast<uint16_t>(mesh.bones.size());
            mesh.bones.push_back({found->second, toGlm(linkBind.Inverse() * meshBind)});
            const int* indices = cluster->GetControlPointIndices();
            const double* weights = cluster->GetControlPointWeights();
            for (int i = 0; i < cluster->GetControlPointIndicesCount(); ++i) {
                const int point = indices[i];
                const float weight = static_cast<float>(weights[i]);
                if (point < 0 || point >= pointCount || !(weight > 0.0f)) continue;
                Weights& w = points[size_t(point)];
                int slot = w.count;
                if (w.count == 4) {   // keep the four strongest
                    slot = 0;
                    for (int k = 1; k < 4; ++k)
                        if (w.weights[k] < w.weights[slot]) slot = k;
                    if (w.weights[slot] >= weight) continue;
                } else
                    ++w.count;
                w.joints[slot] = bone;
                w.weights[slot] = weight;
            }
        }
        if (mesh.bones.empty()) return;
        mesh.skin.resize(mesh.data.vertices.size());
        for (size_t v = 0; v < mesh.skin.size(); ++v) {
            const Weights& w = points[size_t(vertexPoint[v])];
            const float total = w.weights.x + w.weights.y + w.weights.z + w.weights.w;
            mesh.skin[v].joints = w.joints;
            mesh.skin[v].weights = total > 1e-6f ? w.weights / total : glm::vec4(1, 0, 0, 0);
        }
    }

    void bounds() {
        m_model.bounds = emptyBounds();
        bool hasGeometry = false;
        for (const ModelNode& node : m_model.nodes)
            for (uint32_t index : node.meshes) {
                const ModelMesh& mesh = m_model.meshes[index];
                for (int corner = 0; corner < 8; ++corner) {
                    const glm::vec3 p(
                        (corner & 1) ? mesh.bounds.max.x : mesh.bounds.min.x,
                        (corner & 2) ? mesh.bounds.max.y : mesh.bounds.min.y,
                        (corner & 4) ? mesh.bounds.max.z : mesh.bounds.min.z);
                    const auto world = glm::vec3(node.world * glm::vec4(p, 1));
                    if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
                        throw std::runtime_error("Non-finite model transform");
                    expand(m_model.bounds, world);
                }
                hasGeometry = true;
            }
        if (!hasGeometry) throw std::runtime_error("Model contains no drawable triangles");
        modelimport::skinnedBounds(m_model);
    }

    // Every take (animation stack), sampled at the file's frame rate for each node one of
    // its layers moves.
    void readTakes(FbxScene& scene) {
        FbxTime::EMode mode = scene.GetGlobalSettings().GetTimeMode();
        double fps =
            mode == FbxTime::eCustom ? scene.GetGlobalSettings().GetCustomFrameRate() : FbxTime::GetFrameRate(mode);
        if (!(fps > 0.0)) fps = 30.0;
        for (int s = 0; s < scene.GetSrcObjectCount<FbxAnimStack>(); ++s) {
            FbxAnimStack* stack = scene.GetSrcObject<FbxAnimStack>(s);
            if (!stack) continue;
            scene.SetCurrentAnimationStack(stack);
            FbxTimeSpan span = stack->GetLocalTimeSpan();
            if (const FbxTakeInfo* take = scene.GetTakeInfo(stack->GetName());
                take && span.GetDuration().GetSecondDouble() <= 0.0)
                span = take->mLocalTimeSpan;
            ModelAnimation clip;
            clip.name = stack->GetName();
            if (clip.name.empty()) clip.name = "Animation " + std::to_string(s + 1);
            const double duration = std::max(span.GetDuration().GetSecondDouble(), 0.0);
            clip.duration = static_cast<float>(duration);
            const int frames = static_cast<int>(std::lround(duration * fps)) + 1;
            for (size_t n = 0; n < m_nodes.size(); ++n) {
                FbxNode* node = m_nodes[n];
                bool animated = false;
                for (int l = 0; l < stack->GetMemberCount<FbxAnimLayer>() && !animated; ++l) {
                    FbxAnimLayer* layer = stack->GetMember<FbxAnimLayer>(l);
                    animated = node->LclTranslation.GetCurveNode(layer) || node->LclRotation.GetCurveNode(layer) ||
                               node->LclScaling.GetCurveNode(layer);
                }
                if (!animated) continue;
                ModelChannel channel;
                channel.node = static_cast<uint32_t>(n);
                for (int f = 0; f < frames; ++f) {
                    const double seconds = std::min(f / fps, duration);
                    FbxTime time;
                    time.SetSecondDouble(span.GetStart().GetSecondDouble() + seconds);
                    const FbxAMatrix local = node->EvaluateLocalTransform(time);
                    const FbxQuaternion q = local.GetQ();
                    const float t = static_cast<float>(seconds);
                    channel.positions.push_back({t, toGlm(local.GetT())});
                    channel.rotations.push_back({t, glm::quat(float(q[3]), float(q[0]), float(q[1]), float(q[2]))});
                    channel.scales.push_back({t, toGlm(local.GetS())});
                }
                clip.channels.push_back(std::move(channel));
            }
            m_model.animations.push_back(std::move(clip));
        }
    }

    ModelData& m_model;
    fs::path m_file, m_extract, m_modelFolder;
    std::vector<FbxNode*> m_nodes;   // by ModelNode index
    std::unordered_map<FbxNode*, uint32_t> m_index;
    std::unordered_map<FbxSurfaceMaterial*, int> m_materials;
    std::map<std::pair<FbxFileTexture*, bool>, int> m_textures;
    int m_defaultMaterial = -1;
};
}   // namespace

bool importFbxWithSdk(const fs::path& file, ModelData& out, std::string& error, const fs::path& extractFolder) {
    // One import at a time: the SDK's managers aren't documented as safe side by side.
    static std::mutex sdkMutex;
    const std::lock_guard lock(sdkMutex);
    error.clear();
    try {
        Sdk sdk;
        if (!sdk.manager) throw std::runtime_error("the FBX SDK didn't start");
        FbxIOSettings* settings = FbxIOSettings::Create(sdk.manager, IOSROOT);
        sdk.manager->SetIOSettings(settings);
        settings->SetBoolProp(IMP_FBX_MATERIAL, true);
        settings->SetBoolProp(IMP_FBX_TEXTURE, true);
        settings->SetBoolProp(IMP_FBX_ANIMATION, true);
        settings->SetBoolProp(IMP_FBX_EXTRACT_EMBEDDED_DATA, !extractFolder.empty());
        // ::FbxImporter is the SDK's; shockwave::FbxImporter is the fbx_importer setting.
        ::FbxImporter* importer = ::FbxImporter::Create(sdk.manager, "");
        if (!extractFolder.empty()) importer->SetEmbeddingExtractionFolder(toUtf8(extractFolder).c_str());
        dependency(out.dependencies, file);
        if (!importer->Initialize(toUtf8(fs::absolute(file)).c_str(), -1, settings))
            throw std::runtime_error(importer->GetStatus().GetErrorString());
        FbxScene* scene = FbxScene::Create(sdk.manager, "");
        const bool imported = importer->Import(scene);
        const std::string status = importer->GetStatus().GetErrorString();
        importer->Destroy();
        if (!imported) throw std::runtime_error(status);
        // Assimp's units and axes, so either importer places a model the same way:
        // centimetres (Assimp scales by the file's UnitScaleFactor) and Y up.
        if (scene->GetGlobalSettings().GetSystemUnit() != FbxSystemUnit::cm) FbxSystemUnit::cm.ConvertScene(scene);
        if (scene->GetGlobalSettings().GetAxisSystem() != FbxAxisSystem::OpenGL)
            FbxAxisSystem::OpenGL.ConvertScene(scene);
        FbxGeometryConverter converter(sdk.manager);
        if (!converter.Triangulate(scene, true))
            out.warnings.emplace_back("the FBX SDK couldn't triangulate every mesh");
        converter.SplitMeshesPerMaterial(scene, true);
        Importer(out, file, extractFolder).run(*scene);
        out.importer = "fbxsdk";
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (error.empty()) error = "the FBX SDK couldn't read the file";
        return false;
    }
}

}   // namespace shockwave
