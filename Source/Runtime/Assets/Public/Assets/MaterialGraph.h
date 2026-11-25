#pragma once

// Material graphs (W4, docs/MaterialGraph.md): a material built by connecting nodes. A
// .swmatgraph file compiles to one GLSL function that fills in the standard family's
// surface (base colour, opacity, normal, roughness, metallic, emissive, occlusion); the
// renderer splices it into the scene's fragment shader at the "@material_graph" call in
// mesh.glsl, and ShaderCompiler (Assets/ShaderCompiler.h) turns that into SPIR-V.
//
//   swmatgraph 1
//   node rock texture "textures/rock.png"          # id, type, then the type's own values
//   node tint param tint 1 0.85 0.7               # a named value a .swmaterial can change
//   node lit multiply a=rock.rgb b=tint            # inputs: pin=node[.output] or pin=number
//   output base_color lit
//   output roughness rock.g
//   # layout rock 40 120                           # editor positions, kept at the end
//
// Lighting, shadows, fog and the other families are untouched.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

inline constexpr size_t kMaxGraphNodes = 256;
inline constexpr size_t kMaxGraphTextures = 16;
inline constexpr size_t kMaxGraphParams = 16;

// What travels on a wire: 1 to 4 components. A float connects to any vector by repeating.
enum class GraphValue : uint8_t { Any = 0, Float = 1, Vec2 = 2, Vec3 = 3, Vec4 = 4 };
const char* graphValueName(GraphValue type);   // "float", "vec2", ...

struct MaterialGraphLink {
    std::string pin;   // the input it fills
    bool literal = false;   // a number written in place of a node
    float value = 0.0f;
    std::string node;   // the node it comes from
    std::string output;   // that node's output; empty: its first
};
struct MaterialGraphNode {
    std::string id;
    std::string type;
    std::string text;   // texture: the asset; param: its name
    std::vector<float> values;   // constant and param: 1 to 4 numbers; uv: the set
    bool linear = false;   // texture: decoded as data, not colour (a "linear" word after the path)
    std::vector<MaterialGraphLink> inputs;
    int line = 0;
    bool placed = false;   // has a "# layout" position
    glm::vec2 position{0.0f};
};
struct MaterialGraphOutput {
    std::string surface;   // base_color, opacity, normal, roughness, metallic, emissive, occlusion
    std::string node, output;
    int line = 0;
};
struct MaterialGraph {
    std::vector<MaterialGraphNode> nodes;
    std::vector<MaterialGraphOutput> outputs;
    const MaterialGraphNode* find(std::string_view id) const;
};

// Text in, graph out; error names the line. Only the format is checked here: what the
// nodes mean is checked by compileMaterialGraph.
bool parseMaterialGraph(std::string_view text, MaterialGraph& graph, std::string& error);
// Canonical text: nodes in file order, outputs, then the layout comments.
std::string writeMaterialGraph(const MaterialGraph& graph);

// The node types (version 1) with their pins, for the compiler, the editor and the schema.
struct GraphPinInfo {
    const char* name;
    GraphValue type;   // Any: the node's own type, the widest of its Any inputs
    bool optional = false;   // has a default when not connected
};
struct GraphNodeInfo {
    const char* type;
    const char* values;   // what follows the type on its line, "" for nothing
    std::vector<GraphPinInfo> inputs;
    std::vector<GraphPinInfo> outputs;   // the first is the one a bare link means
    const char* description;
};
const std::vector<GraphNodeInfo>& materialGraphNodeTypes();
const GraphNodeInfo* findMaterialGraphNodeType(std::string_view type);
// The surface values an output line can fill, in MaterialGraphSurface's order.
struct GraphSurfaceInfo {
    const char* name;
    GraphValue type;
    const char* description;
};
const std::vector<GraphSurfaceInfo>& materialGraphSurfaces();

struct MaterialGraphError {
    std::string node;   // empty: the graph as a whole
    std::string message;
};
// A graph's parameter: its slot in the draw's data, its components and its default.
struct MaterialGraphParam {
    std::string name;
    int components = 1;
    glm::vec4 value{0.0f};
};
struct MaterialGraphTexture {
    std::string asset;
    bool linear = false;
};
struct MaterialGraphCode {
    std::string glsl;   // the function materialGraphSurface, and the defines it needs
    std::vector<MaterialGraphParam> params;   // in slot order
    std::vector<MaterialGraphTexture> textures;   // in slot order
    bool normal = false;   // writes the normal (the shader then builds a tangent frame)
};
// Checks the graph (unknown types, missing or doubled links, cycles, type mismatches,
// limits) and generates its GLSL, one statement per node in dependency order. False with
// every problem found, each naming its node.
bool compileMaterialGraph(const MaterialGraph& graph, MaterialGraphCode& code, std::vector<MaterialGraphError>& errors);
std::string materialGraphErrorText(const std::vector<MaterialGraphError>& errors);
// The shader cache's key: the generated GLSL and the engine's scene shader (engineHash, the
// hash of the compiled mesh.frag the graph is spliced into).
uint64_t materialGraphHash(std::string_view glsl, uint64_t engineHash);
// The whole fragment shader ShaderCompiler compiles: mesh.frag with MATERIAL_GRAPH defined
// and the graph's function after it.
std::string materialGraphShaderSource(const MaterialGraphCode& code);
// player --material-graph-schema: every node type, its values and pins.
std::string materialGraphSchemaMarkdown();

}   // namespace shockwave
