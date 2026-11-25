// The Material graph window (W4b, docs/MaterialGraph.md): a node editor for .swmatgraph
// files, built on imgui-node-editor with the same patterns as the Circuits window
// (CircuitEditorPanel.cpp): right-click or drop a wire on empty space to add a node, drag
// wires between pins, Delete removes, Ctrl+Z / Ctrl+Y undo and redo.
//
// The graph is checked on every edit, and each problem shows on its node. Half a second
// after the last edit the shader compiles on a background thread (glslangValidator, into
// the project's shader cache); until it's done, and whenever it fails, the last working
// shader stays. Preview draws a sphere with that shader in the viewport, in the scene's own
// light; it's never part of the scene. Saving writes the canonical file with "# layout"
// lines, and the materials that use the graph reload with it.
#include "Editor.h"

#include <Assets/AssetFolder.h>
#include <Assets/Assets.h>
#include <Assets/MaterialGraph.h>
#include <Assets/ShaderCompiler.h>
#include <Renderer/Renderer.h>
#include <Renderer/Shading.h>
#include <UI/Gui.h>

#include <imgui_node_editor.h>
#include <imgui_stdlib.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <sstream>

namespace ed = ax::NodeEditor;

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::GraphValue;
using shockwave::MaterialGraph;
using shockwave::MaterialGraphLink;
using shockwave::MaterialGraphNode;

namespace {

constexpr uint32_t kOutputNode = 1;   // the surface outputs, drawn as one node at the right
constexpr double kCompileDelay = 0.5;   // seconds after the last edit

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}   // namespace

struct MaterialGraphEditorState {
    std::string path;   // inside Content; empty when nothing is open
    MaterialGraph doc;
    bool dirty = false;
    ed::EditorContext* context = nullptr;
    std::vector<uint32_t> ids;   // per document node: its id in the node editor
    uint32_t nextId = kOutputNode + 1;
    bool placeNodes = false;   // move the editor's nodes to the document's positions next frame
    bool navigate = false;   // and show them all
    glm::vec2 outputAt{0.0f};   // the output node isn't in the file: placed right of the rest
    std::vector<MaterialGraph> undo, redo;
    // The check on every edit.
    std::map<std::string, std::vector<std::string>> nodeErrors;
    std::vector<std::string> graphErrors;
    shockwave::MaterialGraphCode code;   // the last check's, when it passed
    // The background compile.
    bool compilePending = false;
    double lastEdit = 0;
    std::future<shockwave::GraphShaderResult> job;
    shockwave::MaterialGraphCode jobCode;
    std::string compilerError;
    std::set<std::string> compilerNodes;
    std::string status;
    size_t compiles = 0;   // shaders made since the graph was opened
    // The working shader the preview draws with.
    shockwave::SurfaceShaderHandle shader;
    shockwave::MaterialGraphCode liveCode;
    shockwave::MaterialGraphShading shading;
    shockwave::MaterialShading materialShading;
    bool preview = true;
    bool previewPlaced = false;
    glm::vec3 previewAt{0.0f};
    // The add-node menu.
    std::string search;
    ImVec2 newNodeAt{0, 0};
    ed::PinId dragFrom = 0;
    bool focusSearch = false;
    std::string pendingOpen;
    std::string renaming, renameText;
    std::string focusNode;

    ~MaterialGraphEditorState() {
        if (job.valid()) job.wait();
        if (context) ed::DestroyEditor(context);
    }
};

namespace {

// ---- Ids ----------------------------------------------------------------------------------
uint64_t pinId(uint32_t node, bool output, uint32_t index) {
    return (uint64_t(node) << 16) | (output ? 0x8000u : 0u) | (index & 0x7fffu);
}
struct PinRef {
    uint32_t node = 0;
    bool output = false;
    uint32_t index = 0;
};
PinRef decodePin(uint64_t id) { return {uint32_t(id >> 16), (id & 0x8000u) != 0, uint32_t(id & 0x7fffu)}; }

ImVec4 valueColor(GraphValue type) {
    const gui::Theme& t = gui::currentTheme();
    switch (type) {
    case GraphValue::Float: return gui::semanticColor(t.circuitNumber);
    case GraphValue::Vec2:
    case GraphValue::Vec3:
    case GraphValue::Vec4: return gui::semanticColor(t.circuitVector);
    case GraphValue::Any: break;
    }
    return gui::semanticColor(t.circuitAsset);
}

void drawPinIcon(GraphValue type, bool connected) {
    const float size = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(size, size));
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const ImU32 color = ImGui::GetColorU32(valueColor(type));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = size * 0.28f;
    if (connected)
        draw->AddCircleFilled(c, r, color);
    else
        draw->AddCircle(c, r, color, 0, 1.5f);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return text;
}

bool identifier(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    });
}

int nodeIndex(const MaterialGraphEditorState& s, uint32_t id) {
    for (size_t i = 0; i < s.ids.size(); ++i)
        if (s.ids[i] == id) return static_cast<int>(i);
    return -1;
}
int nodeIndex(const MaterialGraphEditorState& s, const std::string& name) {
    for (size_t i = 0; i < s.doc.nodes.size(); ++i)
        if (s.doc.nodes[i].id == name) return static_cast<int>(i);
    return -1;
}

std::string uniqueId(const MaterialGraphEditorState& s, const std::string& base) {
    if (nodeIndex(s, base) < 0) return base;
    for (int n = 2;; ++n) {
        const std::string name = base + std::to_string(n);
        if (nodeIndex(s, name) < 0) return name;
    }
}

// The index of a node's output a link names (empty: the first), or -1.
int outputIndex(const shockwave::GraphNodeInfo& info, const std::string& output) {
    if (output.empty()) return info.outputs.empty() ? -1 : 0;
    for (size_t o = 0; o < info.outputs.size(); ++o)
        if (output == info.outputs[o].name) return static_cast<int>(o);
    return -1;
}

// ---- Checking and compiling -----------------------------------------------------------------

void check(MaterialGraphEditorState& s) {
    s.nodeErrors.clear();
    s.graphErrors.clear();
    std::vector<shockwave::MaterialGraphError> errors;
    shockwave::MaterialGraphCode code;
    if (shockwave::compileMaterialGraph(s.doc, code, errors))
        s.code = std::move(code);
    else
        s.code = {};
    for (const auto& e : errors) (e.node.empty() ? s.graphErrors : s.nodeErrors[e.node]).push_back(e.message);
}

void resetEditor(MaterialGraphEditorState& s) {
    if (s.context) ed::DestroyEditor(s.context);
    ed::Config config;
    config.SettingsFile = nullptr;   // positions live in the file's "# layout" lines
    s.context = ed::CreateEditor(&config);
    s.ids.clear();
    for (size_t i = 0; i < s.doc.nodes.size(); ++i) s.ids.push_back(s.nextId++);
    s.placeNodes = true;
    s.navigate = true;
}

// Call before every change, so it can be undone.
void remember(MaterialGraphEditorState& s) {
    s.undo.push_back(s.doc);
    if (s.undo.size() > 200) s.undo.erase(s.undo.begin());
    s.redo.clear();
}
void changed(MaterialGraphEditorState& s) {
    s.dirty = true;
    check(s);
    s.compilePending = true;
    s.lastEdit = nowSeconds();
}

void removeNode(MaterialGraphEditorState& s, int index) {
    const std::string id = s.doc.nodes[size_t(index)].id;
    s.doc.nodes.erase(s.doc.nodes.begin() + index);
    s.ids.erase(s.ids.begin() + index);
    for (auto& node : s.doc.nodes)
        std::erase_if(node.inputs, [&](const MaterialGraphLink& l) { return !l.literal && l.node == id; });
    std::erase_if(s.doc.outputs, [&](const auto& o) { return o.node == id; });
}

void renameNode(MaterialGraphEditorState& s, const std::string& from, const std::string& to) {
    for (auto& node : s.doc.nodes) {
        if (node.id == from) node.id = to;
        for (auto& link : node.inputs)
            if (!link.literal && link.node == from) link.node = to;
    }
    for (auto& output : s.doc.outputs)
        if (output.node == from) output.node = to;
}

GraphValue pinType(const MaterialGraphEditorState& s, PinRef pin) {
    if (pin.node == kOutputNode) {
        const auto& surfaces = shockwave::materialGraphSurfaces();
        return pin.index < surfaces.size() ? surfaces[pin.index].type : GraphValue::Any;
    }
    const int index = nodeIndex(s, pin.node);
    if (index < 0) return GraphValue::Any;
    const auto* info = shockwave::findMaterialGraphNodeType(s.doc.nodes[size_t(index)].type);
    if (!info) return GraphValue::Any;
    const auto& pins = pin.output ? info->outputs : info->inputs;
    return pin.index < pins.size() ? pins[pin.index].type : GraphValue::Any;
}

// Connects an output pin to an input pin (or a surface output), replacing what the input was
// connected to. Returns why not, or "" when it worked.
std::string connect(MaterialGraphEditorState& s, PinRef out, PinRef in, bool apply) {
    if (out.output == in.output) return "Connect an output to an input";
    if (!out.output) std::swap(out, in);
    if (out.node == kOutputNode) return "Connect an output to an input";
    if (out.node == in.node) return "A node can't connect to itself";
    const int from = nodeIndex(s, out.node);
    if (from < 0) return "Unknown node";
    const MaterialGraphNode& source = s.doc.nodes[size_t(from)];
    const auto* sourceInfo = shockwave::findMaterialGraphNodeType(source.type);
    if (!sourceInfo || out.index >= sourceInfo->outputs.size()) return "No such pin";
    const GraphValue a = pinType(s, out), b = pinType(s, in);
    if (a != GraphValue::Any && b != GraphValue::Any && a != GraphValue::Float && a != b)
        return std::string("A ") + shockwave::graphValueName(a) + " doesn't go into a " + shockwave::graphValueName(b);
    const std::string output = out.index == 0 ? std::string() : sourceInfo->outputs[out.index].name;
    if (in.node == kOutputNode) {
        const auto& surfaces = shockwave::materialGraphSurfaces();
        if (in.index >= surfaces.size()) return "No such output";
        if (!apply) return "";
        remember(s);
        const std::string surface = surfaces[in.index].name;
        std::erase_if(s.doc.outputs, [&](const auto& o) { return o.surface == surface; });
        shockwave::MaterialGraphOutput o;
        o.surface = surface;
        o.node = source.id;
        o.output = output;
        s.doc.outputs.push_back(o);
        changed(s);
        return "";
    }
    const int to = nodeIndex(s, in.node);
    if (to < 0) return "Unknown node";
    const auto* targetInfo = shockwave::findMaterialGraphNodeType(s.doc.nodes[size_t(to)].type);
    if (!targetInfo || in.index >= targetInfo->inputs.size()) return "No such pin";
    if (!apply) return "";
    remember(s);
    const std::string pin = targetInfo->inputs[in.index].name;
    auto& inputs = s.doc.nodes[size_t(to)].inputs;
    std::erase_if(inputs, [&](const MaterialGraphLink& l) { return l.pin == pin; });
    MaterialGraphLink link;
    link.pin = pin;
    link.node = source.id;
    link.output = output;
    inputs.push_back(link);
    changed(s);
    return "";
}

void disconnect(MaterialGraphEditorState& s, uint64_t linkPin) {
    const PinRef pin = decodePin(linkPin);   // links are named by their input pin
    if (pin.node == kOutputNode) {
        const auto& surfaces = shockwave::materialGraphSurfaces();
        if (pin.index < surfaces.size())
            std::erase_if(s.doc.outputs, [&](const auto& o) { return o.surface == surfaces[pin.index].name; });
        return;
    }
    const int index = nodeIndex(s, pin.node);
    if (index < 0) return;
    auto& node = s.doc.nodes[size_t(index)];
    const auto* info = shockwave::findMaterialGraphNodeType(node.type);
    if (!info || pin.index >= info->inputs.size()) return;
    std::erase_if(node.inputs, [&](const MaterialGraphLink& l) { return l.pin == info->inputs[pin.index].name; });
}

// A new node's starting values, so it checks straight away where it can.
void startingValues(MaterialGraphEditorState& s, MaterialGraphNode& node) {
    if (node.type == "constant") node.values = {1.0f};
    if (node.type == "param") {
        std::set<std::string> taken;
        for (const auto& n : s.doc.nodes)
            if (n.type == "param") taken.insert(n.text);
        node.text = "value";
        for (int n = 2; taken.count(node.text); ++n) node.text = "value" + std::to_string(n);
        node.values = {1.0f};
    }
}

}   // namespace

// Adds a node of this type at a canvas position and connects it to `from` if it can; its id.
static std::string addGraphNode(MaterialGraphEditorState& s, const std::string& type, ImVec2 at, ed::PinId from) {
    remember(s);
    MaterialGraphNode node;
    node.type = type;
    node.id = uniqueId(s, type);
    node.placed = true;
    node.position = {at.x, at.y};
    startingValues(s, node);
    s.doc.nodes.push_back(node);
    s.ids.push_back(s.nextId++);
    s.placeNodes = true;   // put it where it was asked for, next frame
    if (from) {
        const PinRef start = decodePin(from.Get());
        const auto* info = shockwave::findMaterialGraphNodeType(type);
        const size_t count = !info ? 0 : start.output ? info->inputs.size() : info->outputs.size();
        for (uint32_t k = 0; k < count; ++k) {
            const PinRef end{s.ids.back(), !start.output, k};
            if (connect(s, start, end, false).empty()) {
                s.undo.pop_back();   // connect remembers too; keep one step for both
                connect(s, start, end, true);
                break;
            }
        }
    }
    changed(s);
    return node.id;
}

// ---- Opening, saving, compiling -----------------------------------------------------------

void Editor::releaseMaterialGraphPreview() {
    if (!m_materialGraphEditor) return;
    MaterialGraphEditorState& s = *m_materialGraphEditor;
    if (s.job.valid()) s.job.wait();
    s.job = {};
    if (s.shader.id) engine().renderer().destroySurfaceShader(s.shader);
    s.shader = {};
    s.liveCode = {};
}

void Editor::openMaterialGraph(const std::string& path) {
    if (!m_materialGraphEditor) m_materialGraphEditor = std::make_shared<MaterialGraphEditorState>();
    MaterialGraphEditorState& s = *m_materialGraphEditor;
    m_showMaterialGraph = true;
    if (s.dirty && path != s.path) {
        s.pendingOpen = path;
        return;
    }
    std::ifstream in(m_project.assetFolder / fs::path(path), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    MaterialGraph doc;
    std::string err;
    if (!in || !shockwave::parseMaterialGraph(text.str(), doc, err)) {
        error("Couldn't open " + path + ": " + (err.empty() ? "can't read the file" : err));
        return;
    }
    if (path != s.path) releaseMaterialGraphPreview();
    s.path = path;
    s.doc = std::move(doc);
    s.dirty = false;
    s.undo.clear();
    s.redo.clear();
    s.pendingOpen.clear();
    s.compilerError.clear();
    s.compilerNodes.clear();
    s.compiles = 0;
    // Hand-written graphs without layout lines: a row, and the outputs to the right of it all.
    float x = 0.0f, right = 0.0f;
    for (auto& node : s.doc.nodes) {
        if (!node.placed) {
            node.position = {x, 300.0f};
            x += 240.0f;
        }
        right = std::max(right, node.position.x);
    }
    s.outputAt = {right + 320.0f, 0.0f};
    resetEditor(s);
    check(s);
    s.compilePending = true;
    s.lastEdit = 0;   // compile straight away: usually it's in the cache
}

std::string Editor::newMaterialGraph(const std::string& folder) {
    const std::string base = folder.empty() ? "materials/" : folder + "/";
    std::string path = base + "New graph.swmatgraph";
    for (int n = 2; fs::exists(m_project.assetFolder / path); ++n)
        path = base + "New graph " + std::to_string(n) + ".swmatgraph";
    std::error_code ec;
    fs::create_directories((m_project.assetFolder / path).parent_path(), ec);
    std::ofstream out(m_project.assetFolder / path, std::ios::binary);
    out << "swmatgraph 1\nnode color param color 0.8 0.8 0.8\nnode rough constant 0.5\noutput base_color color\n"
           "output roughness rough\n\n# layout color 0 0\n# layout rough 0 140\n";
    if (!out) {
        error("Couldn't create " + path);
        return {};
    }
    out.close();
    log("Created " + path + "; put it on a material with the Material window's Graph field");
    if (m_assetFolder) logAssetChanges(m_assetFolder->refresh());
    openMaterialGraph(path);
    return path;
}

bool Editor::saveMaterialGraph() {
    if (!m_materialGraphEditor || m_materialGraphEditor->path.empty()) return false;
    MaterialGraphEditorState& s = *m_materialGraphEditor;
    const fs::path file = m_project.assetFolder / fs::path(s.path);
    const fs::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!(out << shockwave::writeMaterialGraph(s.doc)) || !out.flush()) {
            error("Couldn't write " + temp.string());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) {
        error("Couldn't save " + s.path + ": " + ec.message());
        return false;
    }
    s.dirty = false;
    size_t problems = s.graphErrors.size();
    for (const auto& [node, list] : s.nodeErrors) problems += list.size();
    if (problems)
        warn(
            "Saved " + s.path + " with " + std::to_string(problems) +
            " problem(s); materials using it draw without it until they're fixed");
    else
        log("Saved " + s.path);
    if (m_assetFolder) logAssetChanges(m_assetFolder->refresh());   // its materials reload with it
    return true;
}

// Every frame: start the compile half a second after the last edit, and take its shader
// when it's done. Runs whether or not the window is showing.
void Editor::updateMaterialGraph() {
    if (!m_materialGraphEditor) return;
    MaterialGraphEditorState& s = *m_materialGraphEditor;
    if (s.job.valid() && s.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        shockwave::GraphShaderResult result = s.job.get();
        if (result.ok) {
            if (s.shader.id) engine().renderer().destroySurfaceShader(s.shader);   // freed after its frames
            s.shader = engine().renderer().createSurfaceShader(result.spirv);
            s.liveCode = s.jobCode;
            s.compilerError.clear();
            s.compilerNodes.clear();
            ++s.compiles;
            char line[128];
            std::snprintf(
                line, sizeof line, "%s in %.0f ms", result.fromCache ? "Read from the cache" : "Compiled",
                result.milliseconds);
            s.status = line;
        } else {
            s.compilerError = result.error;
            s.compilerNodes.clear();
            for (size_t at = result.error.find("node '"); at != std::string::npos;
                 at = result.error.find("node '", at + 6)) {
                const size_t end = result.error.find('\'', at + 6);
                if (end != std::string::npos) s.compilerNodes.insert(result.error.substr(at + 6, end - at - 6));
            }
            s.status =
                s.shader.id ? "The compile failed; the preview keeps the last working shader" : "The compile failed";
        }
    }
    if (!s.compilePending || s.job.valid() || s.path.empty() || nowSeconds() - s.lastEdit < kCompileDelay) return;
    s.compilePending = false;
    if (!s.nodeErrors.empty() || !s.graphErrors.empty()) {
        s.status = s.shader.id ? "Fix the problems to compile; the preview keeps the last working shader"
                               : "Fix the problems to compile";
        return;
    }
    s.jobCode = s.code;
    const fs::path cache = m_assetFolder ? m_assetFolder->shaderCacheFolder() : m_project.root / "Saved/Cache/Shaders";
    s.status = "Compiling...";
    s.job = std::async(
        std::launch::async, [code = s.jobCode, cache] { return shockwave::materialGraphShader(code, cache); });
}

// The preview sphere, in the viewport, while the window shows it.
void Editor::renderMaterialGraphPreview(shockwave::Renderer& renderer) {
    if (!m_materialGraphEditor || !m_showMaterialGraph || lookingThroughSceneCamera()) return;
    MaterialGraphEditorState& s = *m_materialGraphEditor;
    if (!s.preview || !s.shader.id || s.path.empty()) return;
    if (!s.previewPlaced) {
        s.previewAt = placementPoint();
        s.previewPlaced = true;
    }
    const shockwave::MeshHandle sphere = engine().assets().findMesh("sphere");
    if (!sphere.valid()) return;
    s.shading = {};
    s.shading.shader = s.shader;
    for (size_t i = 0; i < s.liveCode.params.size() && i < shockwave::kGraphSlots; ++i)
        s.shading.params[i] = s.liveCode.params[i].value;
    for (size_t i = 0; i < s.liveCode.textures.size() && i < shockwave::kGraphSlots; ++i)
        if (const auto texture = engine().assets().findTexture(s.liveCode.textures[i].asset))
            s.shading.textures[i] = *texture;
    s.materialShading = {};
    s.materialShading.graph = &s.shading;
    shockwave::DrawMaterial material;
    material.shading = &s.materialShading;
    renderer.drawMesh(sphere, glm::translate(glm::mat4(1.0f), s.previewAt), {}, glm::vec4(1.0f), material);
}

// ---- Drawing ----------------------------------------------------------------------------------

void Editor::drawMaterialGraphWindow() {
    ImGui::SetNextWindowSize(ImVec2(1000, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material graph", &m_showMaterialGraph)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject) {
        ImGui::TextDisabled("Open a project to edit its material graphs.");
        ImGui::End();
        return;
    }
    if (!m_materialGraphEditor) m_materialGraphEditor = std::make_shared<MaterialGraphEditorState>();
    MaterialGraphEditorState& s = *m_materialGraphEditor;
    const gui::Theme& theme = gui::currentTheme();
    const float dpi = ImGui::GetFontSize() / 14.0f;

    // ---- Toolbar ----
    std::vector<std::string> graphs;
    if (m_assetFolder)
        for (const auto& f : m_assetFolder->files())
            if (f.path.extension() == ".swmatgraph") graphs.push_back(f.name);
    ImGui::SetNextItemWidth(260.0f * dpi);
    const std::string shown = s.path.empty() ? "Pick a graph..." : s.path + (s.dirty ? " *" : "");
    if (ImGui::BeginCombo("##graph", shown.c_str())) {
        for (const std::string& name : graphs)
            if (ImGui::Selectable(name.c_str(), name == s.path)) openMaterialGraph(name);
        ImGui::EndCombo();
    }
    gui::toolbarNext(gui::buttonWidth("New"));
    if (gui::button("New")) newMaterialGraph("materials");
    ImGui::BeginDisabled(s.path.empty());
    gui::toolbarNext(gui::controlGroupWidth({"Save", "Undo", "Redo", "Fit"}));
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (gui::button("Save") || (focused && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))) saveMaterialGraph();
    ImGui::SameLine();
    ImGui::BeginDisabled(s.undo.empty());
    if (gui::button("Undo") || (focused && !s.undo.empty() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z))) {
        s.redo.push_back(std::move(s.doc));
        s.doc = std::move(s.undo.back());
        s.undo.pop_back();
        resetEditor(s);
        changed(s);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(s.redo.empty());
    if (gui::button("Redo") || (focused && !s.redo.empty() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y))) {
        s.undo.push_back(std::move(s.doc));
        s.doc = std::move(s.redo.back());
        s.redo.pop_back();
        resetEditor(s);
        changed(s);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool fit = gui::button("Fit");
    ImGui::SetItemTooltip("Show the whole graph");
    ImGui::EndDisabled();

    if (!s.pendingOpen.empty()) ImGui::OpenPopup("Unsaved graph");
    if (ImGui::BeginPopupModal("Unsaved graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved changes.", s.path.c_str());
        const std::string next = s.pendingOpen;
        if (gui::button("Save")) {
            saveMaterialGraph();
            s.pendingOpen.clear();
            openMaterialGraph(next);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (gui::button("Discard")) {
            s.dirty = false;
            s.pendingOpen.clear();
            openMaterialGraph(next);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (gui::button("Cancel")) {
            s.pendingOpen.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (s.path.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Pick a graph above, make a new one, or double-click a .swmatgraph in Content.");
        ImGui::End();
        return;
    }

    // ---- Side panel: preview, compile, params, problems ----
    ImGui::BeginChild("side", ImVec2(250.0f * dpi, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    ImGui::SeparatorText("Preview");
    ImGui::Checkbox("Sphere in the viewport", &s.preview);
    ImGui::SetItemTooltip("A sphere drawn with this graph in the scene's light; it isn't part of the scene");
    if (gui::button("Move it in front of the camera")) {
        s.previewAt = placementPoint();
        s.previewPlaced = true;
    }
    if (!engine().renderer().shadingSettings().materialGraphs)
        ImGui::TextColored(
            gui::semanticColor(theme.warning), "The graphics setting material_graphs is off: graphs draw plain");
    ImGui::SeparatorText("Shader");
    ImGui::TextWrapped("%s", s.status.empty() ? "Not compiled yet" : s.status.c_str());
    if (!s.compilerError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(gui::semanticColor(theme.error), "%s", s.compilerError.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::SeparatorText("Params");
    bool anyParam = false;
    for (const auto& node : s.doc.nodes) {
        if (node.type != "param") continue;
        anyParam = true;
        ImGui::BulletText("%s (%zu)", node.text.c_str(), node.values.size());
    }
    if (!anyParam) ImGui::TextDisabled("None. A param node is a value materials can change.");
    ImGui::SeparatorText("Problems");
    if (s.graphErrors.empty() && s.nodeErrors.empty()) ImGui::TextDisabled("None: the graph checks.");
    for (const auto& message : s.graphErrors) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(gui::semanticColor(theme.error), "%s", message.c_str());
        ImGui::PopTextWrapPos();
    }
    for (const auto& [node, messages] : s.nodeErrors) {
        for (const auto& message : messages) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(gui::semanticColor(theme.error), "%s: %s", node.c_str(), message.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::PushID(node.c_str());
        if (gui::button("Locate node")) s.focusNode = node;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- The graph ----
    ed::SetCurrentEditor(s.context);
    ed::Style& style = ed::GetStyle();
    style.NodeRounding = 0.0f;
    style.NodeBorderWidth = 1.0f;
    style.HoveredNodeBorderWidth = 1.0f;
    style.SelectedNodeBorderWidth = 2.0f;
    style.PinRounding = 0.0f;
    style.LinkStrength = 80.0f;
    style.NodePadding = ImVec4(8.0f * dpi, 6.0f * dpi, 8.0f * dpi, 6.0f * dpi);
    style.Colors[ed::StyleColor_Bg] = gui::semanticColor(theme.background);
    style.Colors[ed::StyleColor_Grid] =
        gui::semanticColor(ImVec4(theme.border.x, theme.border.y, theme.border.z, 0.35f));
    style.Colors[ed::StyleColor_NodeBg] = gui::semanticColor(theme.panel);
    style.Colors[ed::StyleColor_NodeBorder] = gui::semanticColor(theme.border);
    style.Colors[ed::StyleColor_HovNodeBorder] = gui::semanticColor(theme.accentHover);
    style.Colors[ed::StyleColor_SelNodeBorder] = gui::semanticColor(theme.accent);
    style.Colors[ed::StyleColor_HovLinkBorder] = gui::semanticColor(theme.accentHover);
    style.Colors[ed::StyleColor_SelLinkBorder] = gui::semanticColor(theme.accent);
    style.Colors[ed::StyleColor_NodeSelRect] = gui::semanticColor(theme.selection);
    style.Colors[ed::StyleColor_NodeSelRectBorder] = gui::semanticColor(theme.accent);
    style.Colors[ed::StyleColor_LinkSelRect] = gui::semanticColor(theme.selection);
    style.Colors[ed::StyleColor_LinkSelRectBorder] = gui::semanticColor(theme.accent);
    style.Colors[ed::StyleColor_PinRect] = gui::semanticColor(theme.selection);
    style.Colors[ed::StyleColor_PinRectBorder] = gui::semanticColor(theme.accent);

    ed::Begin("material graph");
    if (s.placeNodes) {
        for (size_t i = 0; i < s.doc.nodes.size(); ++i)
            ed::SetNodePosition(s.ids[i], ImVec2(s.doc.nodes[i].position.x, s.doc.nodes[i].position.y));
        ed::SetNodePosition(kOutputNode, ImVec2(s.outputAt.x, s.outputAt.y));
    }

    // Wires, and which pins have one.
    struct Wire {
        uint64_t id, from, to;
        GraphValue type;
    };
    std::vector<Wire> wires;
    std::set<uint64_t> connected;
    auto addWire = [&](const std::string& sourceId, const std::string& output, uint64_t to) {
        const int source = nodeIndex(s, sourceId);
        if (source < 0) return;
        const auto* info = shockwave::findMaterialGraphNodeType(s.doc.nodes[size_t(source)].type);
        const int o = info ? outputIndex(*info, output) : -1;
        if (o < 0) return;
        const uint64_t from = pinId(s.ids[size_t(source)], true, uint32_t(o));
        wires.push_back({to, from, to, info->outputs[size_t(o)].type});
        connected.insert(from);
        connected.insert(to);
    };
    for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
        const auto& node = s.doc.nodes[i];
        const auto* info = shockwave::findMaterialGraphNodeType(node.type);
        if (!info) continue;
        for (const auto& link : node.inputs) {
            if (link.literal) continue;
            for (size_t k = 0; k < info->inputs.size(); ++k)
                if (link.pin == info->inputs[k].name)
                    addWire(link.node, link.output, pinId(s.ids[i], false, uint32_t(k)));
        }
    }
    const auto& surfaces = shockwave::materialGraphSurfaces();
    for (const auto& output : s.doc.outputs)
        for (size_t k = 0; k < surfaces.size(); ++k)
            if (output.surface == surfaces[k].name)
                addWire(output.node, output.output, pinId(kOutputNode, false, uint32_t(k)));

    // A node's own values: a constant's or param's numbers, a texture's file, a uv's set.
    auto numbers = [&](MaterialGraphNode& node) {
        int count = std::clamp(int(node.values.size()), 1, 4);
        static const char* kinds[] = {"float", "vec2", "vec3", "vec4"};
        ImGui::SetNextItemWidth(70.0f * dpi);
        if (ImGui::BeginCombo("##kind", kinds[count - 1])) {
            for (int c = 1; c <= 4; ++c)
                if (ImGui::Selectable(kinds[c - 1], c == count)) {
                    remember(s);
                    node.values.resize(size_t(c), node.values.empty() ? 1.0f : node.values.back());
                    changed(s);
                }
            ImGui::EndCombo();
        }
        float v[4] = {0, 0, 0, 1};
        std::copy_n(node.values.begin(), std::min(node.values.size(), size_t(count)), v);
        ImGui::SetNextItemWidth(float(count) * 55.0f * dpi);
        bool edit = false;
        switch (count) {
        case 1: edit = ImGui::DragFloat("##v", v, 0.01f); break;
        case 2: edit = ImGui::DragFloat2("##v", v, 0.01f); break;
        case 3: edit = ImGui::ColorEdit3("##v", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR); break;
        default: edit = ImGui::ColorEdit4("##v", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR); break;
        }
        if (ImGui::IsItemActivated()) remember(s);
        if (edit) {
            node.values.assign(v, v + count);
            changed(s);
        }
    };

    for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
        auto& node = s.doc.nodes[i];
        const auto* info = shockwave::findMaterialGraphNodeType(node.type);
        const uint32_t id = s.ids[i];
        const auto errors = s.nodeErrors.find(node.id);
        const bool bad = errors != s.nodeErrors.end() || s.compilerNodes.count(node.id);
        if (bad) ed::PushStyleColor(ed::StyleColor_NodeBorder, gui::semanticColor(theme.error));
        ed::BeginNode(id);
        ImGui::PushID(static_cast<int>(id));
        ImGui::TextUnformatted(node.id.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", node.type.c_str());
        if (errors != s.nodeErrors.end()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 240.0f * dpi);
            for (const auto& message : errors->second)
                ImGui::TextColored(gui::semanticColor(theme.error), "%s", message.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!info) {
            ImGui::PopID();
            ed::EndNode();
            if (bad) ed::PopStyleColor();
            continue;
        }
        if (node.type == "constant") {
            numbers(node);
        } else if (node.type == "param") {
            std::string name = node.text;
            ImGui::SetNextItemWidth(120.0f * dpi);
            if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && identifier(name) &&
                name != node.text) {
                remember(s);
                node.text = name;
                changed(s);
            }
            ImGui::SetItemTooltip("The name a .swmaterial's graph_param uses; Enter to rename");
            numbers(node);
        } else if (node.type == "texture") {
            ImGui::SetNextItemWidth(180.0f * dpi);
            if (ImGui::BeginCombo(
                    "##texture", node.text.empty() ? "pick a texture" : node.text.c_str(),
                    ImGuiComboFlags_HeightLarge)) {
                if (m_assetFolder)
                    for (const auto& f : m_assetFolder->files())
                        if (f.kind == shockwave::AssetFolder::Kind::Texture &&
                            ImGui::Selectable(f.name.c_str(), f.name == node.text)) {
                            remember(s);
                            node.text = f.name;
                            changed(s);
                        }
                ImGui::EndCombo();
            }
            bool linear = node.linear;
            if (ImGui::Checkbox("Data (linear)", &linear)) {
                remember(s);
                node.linear = linear;
                changed(s);
            }
            ImGui::SetItemTooltip("Normals and masks are data, read as they are; colours aren't");
        } else if (node.type == "uv") {
            int set = node.values.empty() ? 0 : int(node.values[0]);
            ImGui::SetNextItemWidth(70.0f * dpi);
            if (ImGui::Combo("##set", &set, "set 0\0set 1\0")) {
                remember(s);
                node.values = set ? std::vector<float>{1.0f} : std::vector<float>{};
                changed(s);
            }
        }
        ImGui::BeginGroup();
        for (size_t k = 0; k < info->inputs.size(); ++k) {
            const auto& pin = info->inputs[k];
            const uint64_t pid = pinId(id, false, uint32_t(k));
            const bool wired = connected.count(pid) > 0;
            ed::BeginPin(pid, ed::PinKind::Input);
            drawPinIcon(pin.type, wired);
            ImGui::SameLine();
            ImGui::TextUnformatted(pin.name);
            ed::EndPin();
            if (wired) continue;
            // A number in place of a wire: pin=number.
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(k));
            auto literal = std::find_if(node.inputs.begin(), node.inputs.end(), [&](const MaterialGraphLink& l) {
                return l.pin == pin.name && l.literal;
            });
            float value = literal == node.inputs.end() ? 0.0f : literal->value;
            ImGui::SetNextItemWidth(60.0f * dpi);
            const char* format = literal == node.inputs.end() ? (pin.optional ? "default" : "needed") : "%.3f";
            const bool edit = ImGui::DragFloat("##lit", &value, 0.01f, 0.0f, 0.0f, format);
            if (ImGui::IsItemActivated()) remember(s);
            ImGui::SetItemTooltip("A number for %s, or drag a wire here", pin.name);
            if (edit) {
                std::erase_if(node.inputs, [&](const MaterialGraphLink& l) { return l.pin == pin.name; });
                MaterialGraphLink link;
                link.pin = pin.name;
                link.literal = true;
                link.value = value;
                node.inputs.push_back(link);
                changed(s);
            }
            if (literal != node.inputs.end() && pin.optional) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    remember(s);
                    std::erase_if(node.inputs, [&](const MaterialGraphLink& l) { return l.pin == pin.name; });
                    changed(s);
                }
                ImGui::SetItemTooltip("Back to the default");
            }
            ImGui::PopID();
        }
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, 16.0f * dpi);
        ImGui::BeginGroup();
        for (size_t k = 0; k < info->outputs.size(); ++k) {
            const uint64_t pid = pinId(id, true, uint32_t(k));
            ed::BeginPin(pid, ed::PinKind::Output);
            ImGui::TextUnformatted(info->outputs[k].name);
            ImGui::SameLine();
            drawPinIcon(info->outputs[k].type, connected.count(pid) > 0);
            ed::EndPin();
        }
        ImGui::EndGroup();
        ImGui::PopID();
        ed::EndNode();
        if (bad) ed::PopStyleColor();
    }
    // The surface outputs.
    ed::PushStyleColor(ed::StyleColor_NodeBorder, gui::semanticColor(theme.accent));
    ed::BeginNode(kOutputNode);
    ImGui::TextUnformatted("Surface");
    for (size_t k = 0; k < surfaces.size(); ++k) {
        const uint64_t pid = pinId(kOutputNode, false, uint32_t(k));
        ed::BeginPin(pid, ed::PinKind::Input);
        drawPinIcon(surfaces[k].type, connected.count(pid) > 0);
        ImGui::SameLine();
        ImGui::TextUnformatted(surfaces[k].name);
        ed::EndPin();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", surfaces[k].description);
    }
    ed::EndNode();
    ed::PopStyleColor();
    for (const Wire& wire : wires) ed::Link(wire.id, wire.from, wire.to, valueColor(wire.type), 2.0f);

    // New wires.
    if (ed::BeginCreate(gui::accent(), 2.0f)) {
        ed::PinId a = 0, b = 0;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            const std::string why = connect(s, decodePin(a.Get()), decodePin(b.Get()), false);
            if (!why.empty()) {
                ed::Suspend();
                ImGui::SetTooltip("%s", why.c_str());
                ed::Resume();
                ed::RejectNewItem(gui::semanticColor(theme.error), 2.0f);
            } else if (ed::AcceptNewItem(gui::accent(), 3.0f)) {
                connect(s, decodePin(a.Get()), decodePin(b.Get()), true);
            }
        }
        ed::PinId pin = 0;
        if (ed::QueryNewNode(&pin) && pin && ed::AcceptNewItem()) {
            s.dragFrom = pin;
            s.newNodeAt = ed::ScreenToCanvas(ImGui::GetMousePos());
            s.search.clear();
            s.focusSearch = true;
            ed::Suspend();
            ImGui::OpenPopup("Add node");
            ed::Resume();
        }
        ed::EndCreate();
    }

    // Deleting.
    if (ed::BeginDelete()) {
        bool first = true;
        ed::LinkId link = 0;
        while (ed::QueryDeletedLink(&link)) {
            if (!ed::AcceptDeletedItem()) continue;
            if (first) remember(s), first = false;
            disconnect(s, link.Get());
        }
        ed::NodeId nodeId = 0;
        while (ed::QueryDeletedNode(&nodeId)) {
            if (uint32_t(nodeId.Get()) == kOutputNode) {
                ed::RejectDeletedItem();   // the surface outputs always stay
                continue;
            }
            if (!ed::AcceptDeletedItem()) continue;
            if (first) remember(s), first = false;
            const int index = nodeIndex(s, uint32_t(nodeId.Get()));
            if (index >= 0) removeNode(s, index);
        }
        if (!first) changed(s);
        ed::EndDelete();
    }

    // Menus.
    ed::Suspend();
    static ed::NodeId menuNode = 0;
    static ed::LinkId menuLink = 0;
    if (ed::ShowBackgroundContextMenu()) {
        s.newNodeAt = ed::ScreenToCanvas(ImGui::GetMousePos());
        s.dragFrom = 0;
        s.search.clear();
        s.focusSearch = true;
        ImGui::OpenPopup("Add node");
    }
    if (ed::ShowNodeContextMenu(&menuNode) && uint32_t(menuNode.Get()) != kOutputNode) ImGui::OpenPopup("Node");
    if (ed::ShowLinkContextMenu(&menuLink)) ImGui::OpenPopup("Wire");
    if (ImGui::BeginPopup("Add node")) {
        if (s.focusSearch) {
            ImGui::SetKeyboardFocusHere();
            s.focusSearch = false;
        }
        ImGui::SetNextItemWidth(280.0f * dpi);
        ImGui::InputTextWithHint("##search", "search nodes", &s.search);
        const std::string filter = lower(s.search);
        ImGui::BeginChild("nodes", ImVec2(280.0f * dpi, 320.0f * dpi));
        for (const auto& type : shockwave::materialGraphNodeTypes()) {
            if (!filter.empty() &&
                lower(std::string(type.type) + " " + type.description).find(filter) == std::string::npos)
                continue;
            if (ImGui::Selectable(type.type)) {
                addGraphNode(s, type.type, s.newNodeAt, s.dragFrom);
                s.dragFrom = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("%s", type.description);
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Node")) {
        const int index = nodeIndex(s, uint32_t(menuNode.Get()));
        if (index >= 0) {
            ImGui::TextDisabled("%s", s.doc.nodes[size_t(index)].type.c_str());
            if (ImGui::MenuItem("Rename...")) {
                s.renaming = s.doc.nodes[size_t(index)].id;
                s.renameText = s.renaming;
            }
            if (ImGui::MenuItem("Delete")) ed::DeleteNode(menuNode);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Wire")) {
        if (ImGui::MenuItem("Delete")) ed::DeleteLink(menuLink);
        ImGui::EndPopup();
    }
    if (!s.renaming.empty()) ImGui::OpenPopup("Rename graph node");
    if (ImGui::BeginPopupModal("Rename graph node", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(200.0f * dpi);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##rename", &s.renameText, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = identifier(s.renameText) && (s.renameText == s.renaming || nodeIndex(s, s.renameText) < 0);
        if (!valid)
            ImGui::TextColored(gui::semanticColor(theme.error), "Letters, digits and _, not used by another node");
        ImGui::BeginDisabled(!valid);
        if (gui::button("Rename") || (enter && valid)) {
            if (s.renameText != s.renaming) {
                remember(s);
                renameNode(s, s.renaming, s.renameText);
                changed(s);
            }
            s.renaming.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (gui::button("Cancel")) {
            s.renaming.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    if (!s.focusNode.empty()) {
        const int index = nodeIndex(s, s.focusNode);
        if (index >= 0) {
            ed::SelectNode(s.ids[size_t(index)]);
            ed::NavigateToSelection();
        }
        s.focusNode.clear();
    }
    if (fit || s.navigate) ed::NavigateToContent(s.navigate ? 0.0f : -1.0f);
    s.placeNodes = false;
    s.navigate = false;
    ed::End();

    // Where nodes are now, for the file's layout lines.
    for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
        auto& node = s.doc.nodes[i];
        const ImVec2 at = ed::GetNodePosition(s.ids[i]);
        if (std::abs(at.x - node.position.x) > 0.5f || std::abs(at.y - node.position.y) > 0.5f || !node.placed) {
            node.position = {std::round(at.x), std::round(at.y)};
            node.placed = true;
            s.dirty = true;
        }
    }
    const ImVec2 out = ed::GetNodePosition(kOutputNode);
    s.outputAt = {out.x, out.y};
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

// ---- --material-graph-test ------------------------------------------------------------------
// The smoke test (Tests/MaterialGraphEditorCheck.cmake): with the window's own code, open the
// graph, add a constant wired to roughness, wait for the background compile, add a broken node
// and check its problem lands on it while the shader stays, undo that, save, read the file
// back, and check the preview sphere draws with the graph. Prints one line and exits 1 on any
// problem.
void Editor::runMaterialGraphTest() {
    static int frame = 0;
    static double started = 0;
    static std::vector<std::string> problems;
    static std::string added;
    static size_t compilesBefore = 0;
    static int phase = 0;
    if (++frame < 3) return;
    auto fail = [&](const std::string& why) {
        problems.push_back(why);
        phase = 99;
    };
    MaterialGraphEditorState* s = m_materialGraphEditor.get();
    if (phase == 0) {
        if (!s || s->path.empty()) return fail("--material-graph didn't open a graph");
        if (s->job.valid() || s->compilePending) return;   // the first compile, from the cache or not
        if (!s->shader.id) return fail("opening the graph made no shader: " + s->compilerError + s->status);
        compilesBefore = s->compiles;
        added = addGraphNode(*s, "constant", ImVec2(0, 400), 0);
        const int index = nodeIndex(*s, added);
        s->doc.nodes[size_t(index)].values = {0.123f};
        const auto& surfaces = shockwave::materialGraphSurfaces();
        uint32_t roughness = 0;
        while (roughness < surfaces.size() && std::string_view(surfaces[roughness].name) != "roughness") ++roughness;
        const std::string why = connect(*s, {s->ids[size_t(index)], true, 0}, {kOutputNode, false, roughness}, true);
        if (!why.empty()) return fail("couldn't wire the new node to roughness: " + why);
        s->lastEdit = 0;
        started = nowSeconds();
        phase = 1;
        return;
    }
    if (phase == 1) {
        if (s->job.valid() || s->compilePending) {
            if (nowSeconds() - started > 60) fail("the background compile didn't finish in a minute");
            return;
        }
        if (s->compiles != compilesBefore + 1) return fail("the edit didn't compile: " + s->compilerError + s->status);
        // A broken node: its problem shows on it, and the working shader stays.
        const shockwave::SurfaceShaderHandle working = s->shader;
        const std::string broken = addGraphNode(*s, "add", ImVec2(0, 600), 0);
        if (!s->nodeErrors.count(broken)) fail("the unconnected add node has no problem of its own");
        s->lastEdit = 0;
        updateMaterialGraph();
        if (s->shader != working || s->job.valid()) fail("a broken graph replaced or recompiled the shader");
        s->redo.push_back(s->doc);   // Undo, as the button does
        s->doc = s->undo.back();
        s->undo.pop_back();
        resetEditor(*s);
        changed(*s);
        if (nodeIndex(*s, broken) >= 0 || !s->nodeErrors.empty()) fail("undo didn't take the broken node away");
        if (!saveMaterialGraph()) return fail("saving failed");
        std::ifstream in(m_project.assetFolder / s->path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        MaterialGraph saved;
        std::string why;
        if (!shockwave::parseMaterialGraph(text.str(), saved, why)) return fail("the saved file doesn't read: " + why);
        const auto* node = saved.find(added);
        if (!node || node->values != std::vector<float>{0.123f}) fail("the saved file lacks the new constant");
        if (node && !node->placed) fail("the saved file has no layout line for the new node");
        const bool wired = std::any_of(saved.outputs.begin(), saved.outputs.end(), [&](const auto& o) {
            return o.surface == "roughness" && o.node == added;
        });
        if (!wired) fail("the saved file doesn't send the new node to roughness");
        if (text.str() != shockwave::writeMaterialGraph(saved)) fail("the saved file isn't in canonical order");
        if (phase != 99) phase = 2, started = frame;
        return;
    }
    if (phase == 2) {   // the preview sphere draws with the graph's shader (the scene has no graphs)
        if (frame - started < 3) return;
        if (engine().renderer().shadingStats().graphDraws == 0) fail("the preview sphere didn't draw with the graph");
        phase = 99;
    }
    for (const auto& p : problems) std::fprintf(stderr, "material graph editor: %s\n", p.c_str());
    std::printf(
        "material graph editor: added %s, compiled in the background, problems on their node, saved, previewed; %s\n",
        added.c_str(), problems.empty() ? "ok" : "FAILED");
    std::fflush(stdout);
    m_options.materialGraphTest = false;
    m_exitCode = problems.empty() ? 0 : 1;
    engine().quit();
}

}   // namespace editor
