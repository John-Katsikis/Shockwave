// Material graphs (W4, docs/MaterialGraph.md): the .swmatgraph parser and writer, the
// checks compileMaterialGraph makes (each naming its node), the GLSL it generates, the
// shader cache's names, .swmaterial's graph and graph_param records, and an AssetFolder
// loading a graph material on the null renderer (compiling it when glslangValidator is
// there, which every build that compiles the engine's shaders has).

#include <Assets/AssetFolder.h>
#include <Assets/Image.h>
#include <Assets/Material.h>
#include <Assets/MaterialGraph.h>
#include <Assets/ShaderCompiler.h>
#include <Engine/Engine.h>

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* kLavaValid = "swmatgraph 1\n"
                         "node rock texture \"textures/rock.png\"\n"
                         "node mask texture \"textures/mask.png\" linear uv=flow\n"
                         "node flow panner speed=speed\n"
                         "node speed constant 0.1 0\n"
                         "node glow param glow 4 1.2 0.2\n"
                         "node hot multiply a=glow b=mask.r\n"
                         "node rough constant 0.8\n"
                         "output base_color rock.rgb\n"
                         "output emissive hot\n"
                         "output roughness rough\n"
                         "\n"
                         "# layout rock 10 20\n"
                         "# layout hot 300 40\n";

bool compiles(const std::string& text, MaterialGraphCode& code, std::string& errors) {
    MaterialGraph graph;
    std::string error;
    if (!parseMaterialGraph(text, graph, error)) {
        errors = "parse: " + error;
        return false;
    }
    std::vector<MaterialGraphError> found;
    const bool ok = compileMaterialGraph(graph, code, found);
    errors = materialGraphErrorText(found);
    return ok;
}

void parserChecks() {
    MaterialGraph graph;
    std::string error;
    check(parseMaterialGraph(kLavaValid, graph, error), "parses a graph: " + error);
    check(graph.nodes.size() == 7 && graph.outputs.size() == 3, "seven nodes and three outputs");
    const MaterialGraphNode* mask = graph.find("mask");
    check(mask && mask->type == "texture" && mask->text == "textures/mask.png" && mask->linear, "texture path, linear");
    check(
        mask && mask->inputs.size() == 1 && mask->inputs[0].pin == "uv" && mask->inputs[0].node == "flow",
        "the uv input links to flow");
    const MaterialGraphNode* glow = graph.find("glow");
    check(glow && glow->text == "glow" && glow->values.size() == 3, "a param's name and its default");
    const MaterialGraphNode* hot = graph.find("hot");
    check(hot && hot->inputs[1].node == "mask" && hot->inputs[1].output == "r", "node.output links");
    check(graph.find("rock")->placed && graph.find("rock")->position == glm::vec2(10, 20), "layout comments");
    check(!graph.find("flow")->placed, "nodes without a layout line aren't placed");

    // The canonical text reads back to the same graph and writes the same text again.
    const std::string written = writeMaterialGraph(graph);
    MaterialGraph again;
    check(parseMaterialGraph(written, again, error), "the written text parses: " + error);
    check(writeMaterialGraph(again) == written, "writing is stable");
    check(contains(written, "node glow param glow 4 1.2 0.2\n"), "a param writes its bare name: " + written);
    check(contains(written, "# layout hot 300 40\n"), "layout lines are kept: " + written);

    auto fails = [&](const std::string& text, const std::string& expected) {
        MaterialGraph g;
        std::string e;
        const bool ok = parseMaterialGraph(text, g, e);
        check(!ok && contains(e, expected), "'" + text + "' fails with '" + expected + "', got '" + e + "'");
    };
    fails("node a constant 1\n", "line 1: a material graph starts with \"swmatgraph 1\"");
    fails("swmatgraph 2\n", "swmatgraph 1");
    fails("swmatgraph 1\nnode 1a constant 1\n", "line 2: node id '1a' isn't a name");
    fails("swmatgraph 1\nnode a constant 1\nnode a constant 2\n", "line 3: node 'a' is defined twice");
    fails("swmatgraph 1\nnode a add a=\n", "expected <pin>=<node>");
    fails("swmatgraph 1\nnode a texture \"x.png\n", "a quoted string isn't closed");
    fails("swmatgraph 1\nwire a b\n", "unknown line 'wire'");
    fails("swmatgraph 1\n# layout ghost 1 2\n", "layout for node 'ghost'");
    fails("swmatgraph 1\nnode a constant 1\noutput base_color\n", "expected \"output <surface>");
}

void compileChecks() {
    MaterialGraphCode code;
    std::string errors;
    check(compiles(kLavaValid, code, errors), "the lava graph compiles: " + errors);
    check(
        code.params.size() == 1 && code.params[0].name == "glow" && code.params[0].components == 3 &&
            code.params[0].value == glm::vec4(4, 1.2f, 0.2f, 0),
        "one param with its default");
    check(
        code.textures.size() == 2 && code.textures[0].asset == "textures/rock.png" && !code.textures[0].linear &&
            code.textures[1].linear,
        "two textures in slot order, the mask linear");
    check(!code.normal, "no normal output");
    // One statement per node, in dependency order, each naming its node.
    check(contains(code.glsl, "// flow (panner)") && contains(code.glsl, "// hot (multiply)"), "nodes are named");
    check(
        code.glsl.find("// speed (constant)") < code.glsl.find("// flow (panner)") &&
            code.glsl.find("// flow (panner)") < code.glsl.find("// mask (texture)"),
        "a node comes after the nodes it reads");
    check(contains(code.glsl, "g.params[0].xyz"), "the param reads its slot: " + code.glsl);
    check(contains(code.glsl, "g.textures[1]") && contains(code.glsl, "g.samplers[1]"), "the mask reads slot 1");
    check(contains(code.glsl, "s.emissive = ") && contains(code.glsl, "s.roughness = "), "the outputs are written");
    const std::string source = materialGraphShaderSource(code);
    check(
        contains(source, "#define MATERIAL_GRAPH 1\n") && contains(source, "#include \"mesh.glsl\"\n") &&
            !contains(source, "MATERIAL_GRAPH_NORMAL"),
        "the shader source defines MATERIAL_GRAPH and includes mesh.glsl");

    // A normal output turns on the tangent frame.
    check(
        compiles(
            "swmatgraph 1\nnode n texture \"n.png\" linear\nnode u combine r=0 g=0 b=1\n"
            "node m normal_blend a=n.rgb b=u.rgb\noutput normal m\n",
            code, errors),
        "a normal graph compiles: " + errors);
    check(
        code.normal && contains(materialGraphShaderSource(code), "#define MATERIAL_GRAPH_NORMAL 1"),
        "MATERIAL_GRAPH_NORMAL with a normal output");

    // A float spreads over a vector; the node takes the widest type.
    check(
        compiles(
            "swmatgraph 1\nnode c constant 0.5 0.25 1\nnode m multiply a=c b=2\noutput base_color m\n", code, errors),
        "vec3 times a number: " + errors);
    check(contains(code.glsl, "vec3 n1 = n0 * vec3(2.0)"), "the number becomes a vec3: " + code.glsl);

    // Every problem, each naming its node.
    auto fails = [&](const std::string& body, const std::string& expected) {
        MaterialGraphCode c;
        std::string e;
        const bool ok = compiles("swmatgraph 1\n" + body, c, e);
        check(!ok && contains(e, expected), "'" + body + "' fails with '" + expected + "', got '" + e + "'");
    };
    fails("node a sparkle\n", "node 'a': unknown node type 'sparkle'");
    fails("node a add a=1\n", "node 'a': add's input 'b' isn't connected");
    fails("node a add a=1 b=2 c=3\n", "node 'a': add has no input 'c'");
    fails("node a add a=1 a=2 b=1\n", "node 'a': input 'a' is connected twice");
    fails("node a add a=ghost b=1\n", "node 'a': input 'a' links to 'ghost', which isn't in the graph");
    fails("node t texture \"x.png\"\nnode a add a=t.w b=1\n", "a texture has no output 'w'");
    fails("node a add a=b b=1\nnode b add a=a b=1\noutput base_color a\n", "is part of a cycle");
    fails(
        "node a constant 1 2\nnode b constant 1 2 3\nnode c add a=a b=b\n",
        "node 'c': input 'b' is a vec3 from 'b' but input 'a' is a vec2 from 'a'");
    fails(
        "node a constant 1 2\noutput base_color a\n", "node 'a': output base_color needs a vec3 but 'a' gives a vec2");
    fails(
        "node a constant 1 2\nnode n noise uv=a\nnode c constant 1 2 3\nnode p noise uv=c\n",
        "node 'p': input 'uv' needs a vec2 but 'c' gives a vec3");
    fails("node a constant\n", "node 'a': constant needs 1 to 4 numbers, not 0");
    fails("node a param x 1\nnode b param x 2\n", "node 'b': a second param called 'x'");
    fails("node a param 1\n", "param needs a name");
    fails("node a texture\n", "texture needs a \"path\"");
    fails("node a uv 2\n", "uv takes 0 or 1");
    fails("node a time 3\n", "time takes no numbers");
    fails("node a constant 1 linear\n", "only a texture can be linear");
    fails("node a constant 1\noutput sparkle a\n", "unknown output 'sparkle'");
    fails("node a constant 1\noutput opacity a\noutput opacity a\n", "output opacity is connected twice");
    std::string many;
    for (size_t i = 0; i <= kMaxGraphTextures; ++i)
        many += "node t" + std::to_string(i) + " texture \"t" + std::to_string(i) + ".png\"\n";
    fails(many, "textures; a graph has at most 16");

    // The cache's key follows the shader and the engine it was compiled for.
    const uint64_t a = materialGraphHash("void f() {}", 1), b = materialGraphHash("void f() {}", 2);
    const uint64_t c = materialGraphHash("void g() {}", 1);
    check(a != b && a != c && a == materialGraphHash("void f() {}", 1), "the hash covers the source and the engine");
    check(materialGraphShaderFile("cache", 0x1234).generic_string() == "cache/0000000000001234.spv", "cache names");

    // The node reference lists every type and output.
    const std::string schema = materialGraphSchemaMarkdown();
    for (const GraphNodeInfo& info : materialGraphNodeTypes())
        check(contains(schema, std::string("| `") + info.type + "` |"), std::string("the schema lists ") + info.type);
    check(contains(schema, "| `base_color` | vec3 |"), "the schema lists the outputs");
}

void materialChecks() {
    MaterialFile file;
    std::string error;
    check(
        parseMaterial("swmaterial 1\ngraph \"materials/lava.swmatgraph\"\ngraph_param glow 2 0.5 0.1\n", file, error),
        "graph and graph_param parse: " + error);
    check(
        file.graph == "materials/lava.swmatgraph" && file.graphParams.size() == 1 &&
            file.graphParams[0].name == "glow" && file.graphParams[0].values.size() == 3,
        "the graph and its param value");
    MaterialFile again;
    check(
        parseMaterial(writeMaterial(file), again, error) && again.graph == file.graph &&
            again.graphParams == file.graphParams,
        "graph records write and read back: " + writeMaterial(file));
    MaterialFile bad;
    check(
        !parseMaterial("swmaterial 1\ngraph \"lava.swmaterial\"\n", bad, error) && contains(error, ".swmatgraph"),
        "a graph is a .swmatgraph: " + error);
    check(
        !parseMaterial("swmaterial 1\ngraph_param glow\n", bad, error) && contains(error, "1 to 4 numbers"),
        "graph_param needs numbers: " + error);
    check(
        !parseMaterial("swmaterial 1\ngraph_param a 1\ngraph_param a 2\n", bad, error) && contains(error, "duplicate"),
        "graph_param twice: " + error);

    // A child keeps its parent's graph and changes one param.
    std::map<std::string, std::string> files = {
        {"base.swmaterial", "swmaterial 1\ngraph \"lava.swmatgraph\"\ngraph_param glow 1 1 1\ngraph_param speed 2\n"},
        {"child.swmaterial", "swmaterial 1\nparent \"base.swmaterial\"\ngraph_param glow 3 0 0\n"},
        {"glass.swmaterial", "swmaterial 1\nfamily transmission\ngraph \"lava.swmatgraph\"\n"},
        {"stray.swmaterial", "swmaterial 1\ngraph_param glow 1\n"},
        {"missing.swmaterial", "swmaterial 1\ngraph \"gone.swmatgraph\"\n"},
    };
    MaterialLookup lookup;
    lookup.file = [&](std::string_view name, MaterialFile& f, std::string& e) {
        const auto it = files.find(std::string(name));
        if (it == files.end()) {
            e = "can't open " + std::string(name);
            return false;
        }
        return parseMaterial(it->second, f, e);
    };
    lookup.graph = [](std::string_view name) { return name == "lava.swmatgraph"; };
    MaterialDesc desc;
    check(
        resolveMaterial("child.swmaterial", lookup, desc, error) == MaterialProblem::None, "child resolves: " + error);
    check(desc.graph == "lava.swmatgraph" && (desc.features & kMaterialGraph), "the parent's graph");
    check(
        desc.graphParams.size() == 2 && desc.graphParams[0].name == "glow" &&
            desc.graphParams[0].values == std::vector<float>{3, 0, 0} && desc.graphParams[1].name == "speed",
        "the child's glow over the parent's, the parent's speed kept");
    check(
        std::find(desc.dependencies.begin(), desc.dependencies.end(), "lava.swmatgraph") != desc.dependencies.end(),
        "the graph is a dependency");
    ShadingSettings settings;
    check(materialEffectiveShading(desc, settings) == "material graph", "draws as a material graph");
    settings.materialGraphs = false;
    check(materialEffectiveShading(desc, settings) == "standard (material_graphs off)", "off: its standard material");
    check(resolveMaterial("glass.swmaterial", lookup, desc, error) == MaterialProblem::None, "glass resolves");
    check(
        !(desc.features & kMaterialGraph) && !desc.warnings.empty() && contains(desc.warnings.back(), "only standard"),
        "a graph on glass does nothing, and says so");
    check(
        resolveMaterial("stray.swmaterial", lookup, desc, error) == MaterialProblem::None &&
            contains(desc.warnings.back(), "there's no graph"),
        "graph_param without a graph warns");
    check(
        resolveMaterial("missing.swmaterial", lookup, desc, error) == MaterialProblem::Reference &&
            contains(error, "graph gone.swmatgraph isn't in Content"),
        "a missing graph is a missing reference: " + error);
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// A 2x2 PNG, so the graph's textures have something to read.
void writePng(const fs::path& path) {
    const std::vector<uint8_t> rgba(16, 200);
    const std::vector<uint8_t> png = encodePngRgba8(2, 2, rgba);
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
}

void folderChecks(Engine& engine) {
    {
        const fs::path root = fs::temp_directory_path() / "shockwave_material_graph_test";
        fs::remove_all(root);
        const fs::path content = root / "Content";
        writePng(content / "textures/rock.png");
        writePng(content / "textures/mask.png");
        write(content / "lava.swmatgraph", kLavaValid);
        write(content / "broken.swmatgraph", "swmatgraph 1\nnode a add a=1\noutput base_color a\n");
        write(content / "lava.swmaterial", "swmaterial 1\ngraph \"lava.swmatgraph\"\ngraph_param glow 2 0.5 0.1\n");
        write(content / "typo.swmaterial", "swmaterial 1\ngraph \"lava.swmatgraph\"\ngraph_param glwo 1 1 1\n");
        write(content / "broken.swmaterial", "swmaterial 1\nbase_color 1 0 0 1\ngraph \"broken.swmatgraph\"\n");

        const bool compiler = !shaderCompilerTool().empty() && engineSurfaceShaderHash() != 0;
        AssetFolder folder(engine.renderer(), engine.assets(), engine.jobs());
        folder.setBackgroundCompression(false);
        folder.open(content);
        check(folder.shaderCacheFolder() == root / "Saved" / "Cache" / "Shaders", "the cache is the project's Saved");

        // A graph that doesn't compile leaves the plain material, and says why by node.
        const auto broken = engine.assets().material(engine.assets().findMaterial("broken.swmaterial"));
        check(broken && !broken->fallback && !broken->graph, "a broken graph draws the plain material");
        check(
            broken && !broken->desc.warnings.empty() &&
                contains(broken->desc.warnings.back(), "node 'a': add's input 'b' isn't connected"),
            "and says which node is wrong");

        if (!compiler) {
            std::printf("material graphs: no glslangValidator or engine shaders, so nothing was compiled\n");
            return;
        }
        auto lava = engine.assets().material(engine.assets().findMaterial("lava.swmaterial"));
        check(
            lava && lava->graph && lava->graph->shader.id != 0,
            "the lava material has its graph's shader: " +
                (lava && !lava->desc.warnings.empty() ? lava->desc.warnings.back() : std::string("no warning")));
        check(
            lava && lava->draw.shading && lava->draw.shading->graph == lava->graph.get(),
            "the draw points at the graph");
        check(lava && lava->graph->params[0] == glm::vec4(2, 0.5f, 0.1f, 0), "graph_param over the graph's default");
        check(lava && lava->graph->textures[0].id != 0 && lava->graph->textures[1].id != 0, "both textures bound");
        check(
            folder.graphStats().compiled + folder.graphStats().fromCache == 1 && folder.graphStats().loaded == 1,
            "one graph shader, shared by both materials");
        auto typo = engine.assets().material(engine.assets().findMaterial("typo.swmaterial"));
        check(
            typo && typo->graph && contains(typo->desc.warnings.back(), "has no param called that"),
            "a graph_param the graph doesn't have warns");
        check(engine.renderer().surfaceShaderCount() == 1, "one shader made");

        // The second open reads it from the cache.
        MaterialGraph graph;
        std::string error;
        parseMaterialGraph(kLavaValid, graph, error);
        MaterialGraphCode code;
        std::vector<MaterialGraphError> errors;
        compileMaterialGraph(graph, code, errors);
        const GraphShaderResult cached = materialGraphShader(code, folder.shaderCacheFolder(), false);
        check(cached.ok && cached.fromCache && cached.spirv.size() > 100, "it's in the cache: " + cached.error);
        const GraphShaderResult none = materialGraphShader(code, root / "elsewhere", false);
        check(!none.ok && contains(none.error, "isn't in the package"), "a package doesn't compile: " + none.error);
#if defined(_WIN32)
        _putenv_s("SHOCKWAVE_GLSLANG", "none");
#else
        setenv("SHOCKWAVE_GLSLANG", "none", 1);
#endif
        const GraphShaderResult off = materialGraphShader(code, root / "elsewhere");
        check(!off.ok && contains(off.error, "needs glslangValidator"), "no compiler: " + off.error);
#if defined(_WIN32)
        _putenv_s("SHOCKWAVE_GLSLANG", "");
#else
        unsetenv("SHOCKWAVE_GLSLANG");
#endif

        // Editing the graph swaps the shader; the old one is freed once nothing uses it.
        lava.reset();
        typo.reset();
        write(content / "lava.swmatgraph", std::string(kLavaValid) + "output metallic rough\n");
        fs::last_write_time(content / "lava.swmatgraph", fs::file_time_type::clock::now() + std::chrono::seconds(2));
        folder.refresh(true);
        const auto edited = engine.assets().material(engine.assets().findMaterial("lava.swmaterial"));
        check(edited && edited->graph && edited->graph->shader.id != 0, "the edited graph has a shader");
        check(engine.renderer().surfaceShaderCount() == 1, "and the old shader is gone");
        fs::remove_all(root);
    }
}

class App final : public Application {
public:
    void onStart() override {
        folderChecks(engine());
        engine().quit();
    }
};

}   // namespace

int main() {
    parserChecks();
    compileChecks();
    materialChecks();
    EngineConfig config;
    config.window = {"material graph test", 320, 240, false, true};
    config.renderer = "null";
    config.maxFrames = 0;
    config.showFpsInTitle = false;
    Engine engine(config);
    App app;
    engine.run(app);
    if (failures) return 1;
    std::printf("material graphs: all checks passed\n");
    return 0;
}
