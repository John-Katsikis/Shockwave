// The Circuits window: a node graph editor for .swcircuit files (docs/Circuits.md), built on
// imgui-node-editor. It edits the CircuitDocument and writes the same text file the engine
// and the player read, so hand edits, AI edits and the graph all work on one thing.
//
// Every change is compiled straight away; problems show on their node and in the list. While
// playing, saving the file swaps the new version into the running game, and the variables
// list shows live values for the game circuit or the selected entity.
#include "Editor.h"

#include <Circuits/Circuit.h>
#include <Circuits/CircuitNodes.h>
#include <Circuits/CircuitProgram.h>
#include <Circuits/CircuitWorld.h>
#include <UI/Gui.h>

#include <imgui_node_editor.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace ed = ax::NodeEditor;

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
using shockwave::CircuitDocument;
using shockwave::NodeDesc;
using shockwave::PinDesc;
using shockwave::PinType;

struct CircuitEditorState {
    std::string path;   // inside Content; empty when nothing is open
    shockwave::CircuitKind kind = shockwave::CircuitKind::Entity;
    CircuitDocument doc;
    bool dirty = false;
    ed::EditorContext* context = nullptr;
    std::vector<uint32_t> ids;   // per document node: its id in the node editor
    uint32_t nextId = 1;
    bool placeNodes = false;   // move the editor's nodes to the document's positions next frame
    std::vector<CircuitDocument> undo, redo;
    std::vector<shockwave::CircuitProblem> problems;
    std::set<std::string> errorNodes, warningNodes;
    // The add-node menu.
    std::string search;
    ImVec2 newNodeAt{0, 0};
    ed::PinId dragFrom = 0;   // a wire dropped on empty space: connect the new node to it
    bool focusSearch = false;
    // Opening another circuit with unsaved changes asks first.
    std::string pendingOpen;
    // Rename.
    std::string renaming, renameText;
    std::string newVariable;

    ~CircuitEditorState() {
        if (context) ed::DestroyEditor(context);
    }
};

namespace {

// ---- Ids ---------------------------------------------------------------------------------
// Pins: node id, then whether it's an output, whether it's exec, and its index.
uint64_t pinId(uint32_t node, bool output, bool exec, uint32_t index) {
    return (uint64_t(node) << 16) | (output ? 0x8000u : 0u) | (exec ? 0x4000u : 0u) | (index & 0x3fffu);
}
struct PinRef {
    uint32_t node = 0;
    bool output = false, exec = false;
    uint32_t index = 0;
};
PinRef decodePin(uint64_t id) {
    return {uint32_t(id >> 16), (id & 0x8000u) != 0, (id & 0x4000u) != 0, uint32_t(id & 0x3fffu)};
}

// Get.<variable> and Set.<variable>, as the compiler makes them.
std::shared_ptr<const NodeDesc> variableNode(const std::string& type, PinType valueType) {
    NodeDesc node;
    node.id = type;
    node.category = "Variable";
    PinDesc value;
    value.name = "value";
    value.type = valueType;
    if (type.rfind("Set.", 0) == 0) {
        node.execInputs = {"exec"};
        node.execOutputs = {"then"};
        value.required = true;
        node.inputs = {value};
        value.required = false;
    }
    node.outputs = {value};
    return std::make_shared<const NodeDesc>(std::move(node));
}

std::shared_ptr<const NodeDesc> descFor(const CircuitDocument& doc, const CircuitDocument::Node& node) {
    if (node.type.rfind("Set.", 0) == 0 || node.type.rfind("Get.", 0) == 0) {
        const std::string name = node.type.substr(4);
        for (const auto& variable : doc.variables)
            if (variable.name == name) return variableNode(node.type, variable.type);
        return nullptr;
    }
    return shockwave::findNode(node.type);
}

ImVec4 pinColor(PinType type) {
    const gui::Theme& t = gui::currentTheme();
    switch (type) {
    case PinType::Exec: return gui::semanticColor(t.circuitExec);
    case PinType::Bool: return gui::semanticColor(t.circuitBool);
    case PinType::Int:
    case PinType::Float: return gui::semanticColor(t.circuitNumber);
    case PinType::Vec2:
    case PinType::Vec3:
    case PinType::Color: return gui::semanticColor(t.circuitVector);
    case PinType::Rotation: return gui::semanticColor(t.circuitRotation);
    case PinType::String: return gui::semanticColor(t.circuitText);
    case PinType::Entity: return gui::semanticColor(t.circuitEntity);
    case PinType::Mesh:
    case PinType::Texture:
    case PinType::Model: return gui::semanticColor(t.circuitAsset);
    }
    return gui::semanticColor(t.text);
}

bool compatible(PinType from, PinType to) { return from == to || (from == PinType::Int && to == PinType::Float); }

// An exec pin is a small arrow; a data pin a circle, filled when something's connected.
void drawPinIcon(PinType type, bool connected) {
    const float size = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(size, size));
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const ImU32 color = ImGui::GetColorU32(pinColor(type));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float r = size * 0.28f;
    if (type == PinType::Exec) {
        const ImVec2 points[3] = {{c.x - r, c.y - r * 1.2f}, {c.x + r * 1.1f, c.y}, {c.x - r, c.y + r * 1.2f}};
        if (connected)
            draw->AddConvexPolyFilled(points, 3, color);
        else
            draw->AddPolyline(points, 3, color, ImDrawFlags_Closed, 1.5f);
        return;
    }
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
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(
        s.begin(), s.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; });
}

// "Component.Transform.Set.position" -> "set_position"; "Flow.Branch" -> "branch".
std::string baseName(const std::string& type) {
    std::string tail = type;
    const size_t last = type.rfind('.');
    if (last != std::string::npos) {
        tail = type.substr(last + 1);
        // Component.X.Get.field: keep the verb so names say what they do.
        const size_t before = type.rfind('.', last - 1);
        const std::string verb = before == std::string::npos ? "" : type.substr(before + 1, last - before - 1);
        if (type.rfind("Component.", 0) == 0 && (verb == "Get" || verb == "Set")) tail = verb + "_" + tail;
    }
    std::string name;
    for (const char ch : tail) {
        if (std::isupper(static_cast<unsigned char>(ch)) && !name.empty() && name.back() != '_') name += '_';
        name += std::isalnum(static_cast<unsigned char>(ch)) ? char(std::tolower(static_cast<unsigned char>(ch))) : '_';
    }
    return identifier(name) ? name : "node";
}

}   // namespace

// ---- Opening, saving, editing ---------------------------------------------------------------

namespace {

void resetEditor(CircuitEditorState& s) {
    if (s.context) ed::DestroyEditor(s.context);
    ed::Config config;
    config.SettingsFile = nullptr;   // positions live in the .swcircuit's editor block
    s.context = ed::CreateEditor(&config);
    s.ids.clear();
    for (size_t i = 0; i < s.doc.nodes.size(); ++i) s.ids.push_back(s.nextId++);
    s.placeNodes = true;
}

void compile(CircuitEditorState& s) {
    s.problems.clear();
    s.errorNodes.clear();
    s.warningNodes.clear();
    shockwave::compileCircuit(s.doc, s.path, s.kind, s.problems);
    for (const auto& problem : s.problems) {
        // Messages name their node as node 'name'.
        const size_t at = problem.message.find("node '");
        if (at == std::string::npos) continue;
        const size_t end = problem.message.find('\'', at + 6);
        if (end == std::string::npos) continue;
        (problem.warning ? s.warningNodes : s.errorNodes).insert(problem.message.substr(at + 6, end - at - 6));
    }
}

// Call before every change, so it can be undone.
void remember(CircuitEditorState& s) {
    s.undo.push_back(s.doc);
    if (s.undo.size() > 200) s.undo.erase(s.undo.begin());
    s.redo.clear();
}
void changed(CircuitEditorState& s) {
    s.dirty = true;
    compile(s);
}

int nodeIndex(const CircuitEditorState& s, uint32_t id) {
    for (size_t i = 0; i < s.ids.size(); ++i)
        if (s.ids[i] == id) return static_cast<int>(i);
    return -1;
}
int nodeIndex(const CircuitEditorState& s, const std::string& name) {
    for (size_t i = 0; i < s.doc.nodes.size(); ++i)
        if (s.doc.nodes[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string uniqueNodeName(const CircuitEditorState& s, const std::string& base) {
    if (nodeIndex(s, base) < 0 && base != "self") return base;
    for (int n = 2;; ++n) {
        const std::string name = base + std::to_string(n);
        if (nodeIndex(s, name) < 0) return name;
    }
}

// The node and output an input reads from, when it's a node.pin link.
bool linkSource(const CircuitEditorState& s, const std::string& text, int& node, std::string& output) {
    const size_t dot = text.find('.');
    if (dot == std::string::npos) return false;
    const std::string name = text.substr(0, dot);
    if (!identifier(name) || !identifier(text.substr(dot + 1))) return false;
    node = nodeIndex(s, name);
    output = text.substr(dot + 1);
    return node >= 0;
}

void removeNode(CircuitEditorState& s, int index) {
    const std::string name = s.doc.nodes[size_t(index)].name;
    s.doc.nodes.erase(s.doc.nodes.begin() + index);
    s.ids.erase(s.ids.begin() + index);
    for (auto& node : s.doc.nodes) {
        std::erase_if(node.inputs, [&](const auto& in) { return in.text.rfind(name + ".", 0) == 0; });
        std::erase_if(node.execs, [&](const auto& exec) { return exec.node == name; });
    }
}

void renameNode(CircuitEditorState& s, const std::string& from, const std::string& to) {
    for (auto& node : s.doc.nodes) {
        if (node.name == from) node.name = to;
        for (auto& in : node.inputs)
            if (in.text.rfind(from + ".", 0) == 0) in.text = to + in.text.substr(from.size());
        for (auto& exec : node.execs)
            if (exec.node == from) exec.node = to;
    }
}

void renameVariable(CircuitEditorState& s, const std::string& from, const std::string& to) {
    for (auto& variable : s.doc.variables)
        if (variable.name == from) variable.name = to;
    for (auto& node : s.doc.nodes) {
        if (node.type == "Set." + from) node.type = "Set." + to;
        if (node.type == "Get." + from) node.type = "Get." + to;
        for (auto& in : node.inputs)
            if (in.text == from) in.text = to;
    }
}

// Connects an output pin to an input pin, replacing what the input (data) or the output
// (exec) was connected to. Returns why not, or "" when it worked.
std::string connect(CircuitEditorState& s, PinRef out, PinRef in, bool apply) {
    if (out.output == in.output) return "Connect an output to an input";
    if (!out.output) std::swap(out, in);
    if (out.exec != in.exec) return "Exec pins connect to exec pins";
    const int from = nodeIndex(s, out.node), to = nodeIndex(s, in.node);
    if (from < 0 || to < 0) return "Unknown node";
    if (from == to) return "A node can't connect to itself";
    const auto source = descFor(s.doc, s.doc.nodes[size_t(from)]);
    const auto target = descFor(s.doc, s.doc.nodes[size_t(to)]);
    if (!source || !target) return "Unknown node type";
    const std::string& sourceName = s.doc.nodes[size_t(from)].name;
    if (out.exec) {
        if (out.index >= source->execOutputs.size() || in.index >= target->execInputs.size()) return "No such pin";
        if (!apply) return "";
        remember(s);
        const std::string output = source->execOutputs[out.index];
        for (auto& node : s.doc.nodes)   // one output leads to one node
            std::erase_if(node.execs, [&](const auto& e) { return e.node == sourceName && e.output == output; });
        CircuitDocument::Exec exec;
        exec.pin = in.index == 0 ? "" : target->execInputs[in.index];
        exec.node = sourceName;
        exec.output = output;
        s.doc.nodes[size_t(to)].execs.push_back(exec);
        changed(s);
        return "";
    }
    if (out.index >= source->outputs.size() || in.index >= target->inputs.size()) return "No such pin";
    const PinDesc& o = source->outputs[out.index];
    const PinDesc& i = target->inputs[in.index];
    if (i.constant) return std::string(i.name) + " takes only a written value";
    if (!compatible(o.type, i.type))
        return std::string(i.name) + " wants " + shockwave::pinTypeName(i.type) + ", not " +
               shockwave::pinTypeName(o.type);
    if (!apply) return "";
    remember(s);
    auto& inputs = s.doc.nodes[size_t(to)].inputs;
    std::erase_if(inputs, [&](const auto& x) { return x.pin == i.name; });
    CircuitDocument::Input input;
    input.pin = i.name;
    input.text = sourceName + "." + o.name;
    inputs.push_back(input);
    changed(s);
    return "";
}

void disconnect(CircuitEditorState& s, uint64_t linkPin) {
    const PinRef pin = decodePin(linkPin);
    const int index = nodeIndex(s, pin.node);
    if (index < 0) return;
    auto& node = s.doc.nodes[size_t(index)];
    const auto desc = descFor(s.doc, node);
    if (!desc) return;
    if (pin.exec) {   // exec links are named by their output pin
        if (pin.index >= desc->execOutputs.size()) return;
        const std::string output = desc->execOutputs[pin.index];
        for (auto& other : s.doc.nodes)
            std::erase_if(other.execs, [&](const auto& e) { return e.node == node.name && e.output == output; });
    } else {   // data links by their input pin
        if (pin.index >= desc->inputs.size()) return;
        std::erase_if(node.inputs, [&](const auto& x) { return x.pin == desc->inputs[pin.index].name; });
    }
}

// Adds a node of this type at a canvas position, and connects it to `from` if it can.
void addNode(CircuitEditorState& s, const std::string& type, ImVec2 at, ed::PinId from) {
    remember(s);
    CircuitDocument::Node node;
    node.type = type;
    node.name = uniqueNodeName(s, baseName(type));
    node.position = {at.x, at.y};
    node.placed = true;
    s.doc.nodes.push_back(node);
    s.ids.push_back(s.nextId++);
    ed::SetNodePosition(s.ids.back(), at);
    if (from) {
        const PinRef start = decodePin(from.Get());
        const auto desc = descFor(s.doc, s.doc.nodes.back());
        const uint32_t id = s.ids.back();
        if (desc) {
            // The first pin on the new node that fits.
            const size_t count = start.exec ? (start.output ? desc->execInputs.size() : desc->execOutputs.size())
                                            : (start.output ? desc->inputs.size() : desc->outputs.size());
            for (uint32_t k = 0; k < count; ++k) {
                const PinRef end{id, !start.output, start.exec, k};
                if (connect(s, start, end, false).empty()) {
                    s.undo.pop_back();   // connect remembers too; keep one step for both
                    connect(s, start, end, true);
                    break;
                }
            }
        }
    }
    changed(s);
}

}   // namespace

void Editor::openCircuit(const std::string& path) {
    if (!m_circuitEditor) m_circuitEditor = std::make_shared<CircuitEditorState>();
    CircuitEditorState& s = *m_circuitEditor;
    m_showCircuits = true;
    if (s.dirty && path != s.path) {
        s.pendingOpen = path;
        return;
    }
    const fs::path file = m_project.assetFolder / fs::path(path);
    std::ifstream in(file, std::ios::binary);
    CircuitDocument doc;
    std::string err;
    if (!in || !shockwave::parseCircuit(in, path, doc, err)) {
        error("Couldn't open " + path + ": " + (err.empty() ? "can't read the file" : err));
        return;
    }
    s.path = path;
    s.kind = path == m_project.gameCircuitPath() ? shockwave::CircuitKind::Game : shockwave::CircuitKind::Entity;
    s.doc = std::move(doc);
    s.dirty = false;
    s.undo.clear();
    s.redo.clear();
    s.pendingOpen.clear();
    resetEditor(s);
    compile(s);
}

std::string Editor::newCircuit(const std::string& folder) {
    std::string base = folder.empty() ? "Circuits/" : folder + "/";
    std::string path = base + "New circuit.swcircuit";
    for (int n = 2; fs::exists(m_project.assetFolder / path); ++n)
        path = base + "New circuit " + std::to_string(n) + ".swcircuit";
    std::error_code ec;
    fs::create_directories((m_project.assetFolder / path).parent_path(), ec);
    std::ofstream out(m_project.assetFolder / path, std::ios::binary);
    out << "shockwave_circuit 1\n# Attach it to an entity with a Circuit component, or make it the project's game\n"
           "# circuit in Project settings.\n\nnode start Event.BeginPlay\n\nnode hello Debug.Print\n"
           "  exec start.then\n  in text = \"Hello from a circuit\"\n\neditor\n  start -260 0\n  hello 0 0\nend\n";
    if (!out) {
        error("Couldn't create " + path);
        return {};
    }
    out.close();
    log("Created " + path);
    openCircuit(path);
    return path;
}

void Editor::saveCircuit() {
    if (!m_circuitEditor || m_circuitEditor->path.empty()) return;
    CircuitEditorState& s = *m_circuitEditor;
    std::ostringstream text;
    shockwave::writeCircuit(text, s.doc);
    const fs::path file = m_project.assetFolder / fs::path(s.path);
    const fs::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!(out << text.str()) || !out.flush()) {
            error("Couldn't write " + temp.string());
            return;
        }
    }
    std::error_code ec;
    fs::rename(temp, file, ec);
    if (ec) {
        error("Couldn't save " + s.path + ": " + ec.message());
        return;
    }
    s.dirty = false;
    size_t errors = 0;
    for (const auto& p : s.problems) errors += !p.warning;
    if (errors)
        warn("Saved " + s.path + " with " + std::to_string(errors) + " problem(s); it won't run until they're fixed");
    else
        log("Saved " + s.path);
}

bool Editor::attachCircuit(shockwave::Entity e, const std::string& path) {
    if (!m_world.alive(e)) return false;
    if (playing()) {
        warn("Stop playing to attach circuits; changes made while playing are undone on Stop");
        return false;
    }
    pushUndo(m_world);
    m_world.add<shockwave::Circuit>(e, {path});
    m_dirty = true;
    log(displayName(e) + " runs " + path);
    return true;
}

void Editor::attachCircuit(const std::string& path) {
    for (shockwave::Entity e : selection()) attachCircuit(e, path);
}

// ---- Drawing ----------------------------------------------------------------------------------

void Editor::drawCircuitsWindow() {
    ImGui::SetNextWindowSize(ImVec2(1000, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Circuits", &m_showCircuits)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject) {
        ImGui::TextDisabled("Open a project to edit its circuits.");
        ImGui::End();
        return;
    }
    if (!m_circuitEditor) m_circuitEditor = std::make_shared<CircuitEditorState>();
    CircuitEditorState& s = *m_circuitEditor;
    const gui::Theme& theme = gui::currentTheme();
    const float dpi = ImGui::GetFontSize() / 14.0f;

    // ---- Toolbar: which circuit, save, undo ----
    std::vector<std::string> circuits;
    if (m_assetFolder)
        for (const auto& f : m_assetFolder->files())
            if (f.path.extension() == ".swcircuit") circuits.push_back(f.name);
    std::sort(circuits.begin(), circuits.end());
    ImGui::SetNextItemWidth(260.0f * dpi);
    const std::string shown = s.path.empty() ? "Pick a circuit..." : s.path + (s.dirty ? " *" : "");
    if (ImGui::BeginCombo("##circuit", shown.c_str())) {
        for (const std::string& name : circuits)
            if (ImGui::Selectable(name.c_str(), name == s.path)) openCircuit(name);
        ImGui::EndCombo();
    }
    gui::toolbarNext(gui::buttonWidth("New"));
    if (gui::button("New")) newCircuit("Circuits");
    ImGui::BeginDisabled(s.path.empty());
    gui::toolbarNext(gui::controlGroupWidth({"Save", "Undo", "Redo", "Fit"}));
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (gui::button("Save") || (focused && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))) saveCircuit();
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
    ImGui::SetItemTooltip("Show the whole circuit (F)");
    ImGui::EndDisabled();
    if (!s.path.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s circuit", s.kind == shockwave::CircuitKind::Game ? "Game" : "Entity");
    }

    // Unsaved changes when opening another circuit.
    if (!s.pendingOpen.empty()) ImGui::OpenPopup("Unsaved circuit");
    if (ImGui::BeginPopupModal("Unsaved circuit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved changes.", s.path.c_str());
        const std::string next = s.pendingOpen;
        if (gui::button("Save")) {
            saveCircuit();
            s.pendingOpen.clear();
            openCircuit(next);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (gui::button("Discard")) {
            s.dirty = false;
            s.pendingOpen.clear();
            openCircuit(next);
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
        ImGui::TextDisabled("Pick a circuit above, make a new one, or double-click a .swcircuit in Content.");
        ImGui::End();
        return;
    }

    // ---- Side panel: variables and problems ----
    const float sideWidth = 250.0f * dpi;
    ImGui::BeginChild("side", ImVec2(sideWidth, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    ImGui::SeparatorText("Variables");
    // Live values: the game circuit's, or the selected entity's while it runs this circuit.
    const shockwave::CircuitWorld* live = m_session ? m_session->circuits() : nullptr;
    shockwave::Entity liveEntity = shockwave::kNullEntity;
    if (live && s.kind == shockwave::CircuitKind::Entity) {
        for (shockwave::Entity e : selection())
            if (const auto* c = m_world.get<shockwave::Circuit>(e); c && c->circuit == s.path) liveEntity = e;
        if (!liveEntity.valid()) live = nullptr;
    }
    if (live && liveEntity.valid())
        ImGui::TextDisabled("Values of %s now:", displayName(liveEntity).c_str());
    else if (live)
        ImGui::TextDisabled("Values now:");
    int removeVariable = -1;
    for (size_t v = 0; v < s.doc.variables.size(); ++v) {
        auto& variable = s.doc.variables[v];
        ImGui::PushID(static_cast<int>(v));
        std::string name = variable.name;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && identifier(name) &&
            name != variable.name) {
            const bool taken = std::any_of(
                s.doc.variables.begin(), s.doc.variables.end(), [&](const auto& x) { return x.name == name; });
            if (!taken) {
                remember(s);
                renameVariable(s, variable.name, name);
                changed(s);
            }
        }
        ImGui::SetItemTooltip("Press Enter to rename; uses are renamed too");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 4.0f * dpi);
        if (ImGui::BeginCombo("##type", shockwave::pinTypeName(variable.type))) {
            for (int t = int(PinType::Bool); t <= int(PinType::Model); ++t) {
                if (ImGui::Selectable(shockwave::pinTypeName(PinType(t)), PinType(t) == variable.type)) {
                    remember(s);
                    variable.type = PinType(t);
                    variable.value.clear();
                    changed(s);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) removeVariable = static_cast<int>(v);
        ImGui::SetItemTooltip("Remove the variable");
        std::string value = variable.value;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##value", "starting value", &value)) {
            variable.value = value;
            changed(s);
        }
        if (ImGui::IsItemActivated()) remember(s);
        if (live)
            if (const shockwave::Value* now = live->variable(liveEntity, variable.name))
                ImGui::TextColored(gui::accent(), "  now %s", shockwave::formatValue(*now).c_str());
        ImGui::PopID();
    }
    if (removeVariable >= 0) {
        remember(s);
        s.doc.variables.erase(s.doc.variables.begin() + removeVariable);
        changed(s);
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - gui::buttonWidth("Add") - 4.0f * dpi);
    ImGui::InputTextWithHint("##newvar", "new variable", &s.newVariable);
    ImGui::SameLine();
    const bool newValid =
        identifier(s.newVariable) && std::none_of(s.doc.variables.begin(), s.doc.variables.end(), [&](const auto& x) {
            return x.name == s.newVariable;
        });
    ImGui::BeginDisabled(!newValid);
    if (gui::button("Add")) {
        remember(s);
        CircuitDocument::Variable variable;
        variable.name = s.newVariable;
        variable.type = PinType::Float;
        s.doc.variables.push_back(variable);
        s.newVariable.clear();
        changed(s);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Right-click the graph for Get and Set nodes.");

    ImGui::SeparatorText("Problems");
    if (s.problems.empty()) ImGui::TextDisabled("None: the circuit compiles.");
    for (const auto& problem : s.problems) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(
            gui::semanticColor(problem.warning ? theme.warning : theme.error), "%s", problem.message.c_str());
        ImGui::PopTextWrapPos();
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

    ed::Begin("graph");
    if (s.placeNodes) {
        float autoX = 0.0f;
        for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
            auto& node = s.doc.nodes[i];
            if (!node.placed) {   // hand-written circuits without an editor block: a row
                node.position = {autoX, 400.0f};
                autoX += 260.0f;
            }
            ed::SetNodePosition(s.ids[i], ImVec2(node.position.x, node.position.y));
        }
    }

    // Which inputs are linked, so unconnected ones get a value field.
    struct Link {
        uint64_t id, from, to;
        PinType type;
    };
    std::vector<Link> links;
    std::set<uint64_t> connectedPins;
    for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
        const auto& node = s.doc.nodes[i];
        const auto desc = descFor(s.doc, node);
        if (!desc) continue;
        for (size_t k = 0; k < desc->inputs.size(); ++k) {
            const auto given = std::find_if(
                node.inputs.begin(), node.inputs.end(), [&](const auto& in) { return in.pin == desc->inputs[k].name; });
            int source = -1;
            std::string output;
            if (given == node.inputs.end() || !linkSource(s, given->text, source, output)) continue;
            const auto sourceDesc = descFor(s.doc, s.doc.nodes[size_t(source)]);
            if (!sourceDesc) continue;
            for (size_t o = 0; o < sourceDesc->outputs.size(); ++o) {
                if (sourceDesc->outputs[o].name != output) continue;
                const uint64_t from = pinId(s.ids[size_t(source)], true, false, uint32_t(o));
                const uint64_t to = pinId(s.ids[i], false, false, uint32_t(k));
                links.push_back({to, from, to, sourceDesc->outputs[o].type});
                connectedPins.insert(from);
                connectedPins.insert(to);
            }
        }
        for (const auto& exec : node.execs) {
            const int source = nodeIndex(s, exec.node);
            if (source < 0) continue;
            const auto sourceDesc = descFor(s.doc, s.doc.nodes[size_t(source)]);
            if (!sourceDesc) continue;
            const auto out = std::find(sourceDesc->execOutputs.begin(), sourceDesc->execOutputs.end(), exec.output);
            const auto in = exec.pin.empty() ? desc->execInputs.begin()
                                             : std::find(desc->execInputs.begin(), desc->execInputs.end(), exec.pin);
            if (out == sourceDesc->execOutputs.end() || in == desc->execInputs.end()) continue;
            const uint64_t from =
                pinId(s.ids[size_t(source)], true, true, uint32_t(out - sourceDesc->execOutputs.begin()));
            const uint64_t to = pinId(s.ids[i], false, true, uint32_t(in - desc->execInputs.begin()));
            links.push_back({from, from, to, PinType::Exec});
            connectedPins.insert(from);
            connectedPins.insert(to);
        }
    }

    // Nodes.
    const ImU32 eventHeader = ImGui::GetColorU32(gui::semanticColor(theme.circuitEvent));
    std::vector<std::pair<uint32_t, bool>> headers;   // node id, is an event
    for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
        auto& node = s.doc.nodes[i];
        const auto desc = descFor(s.doc, node);
        const uint32_t id = s.ids[i];
        const bool bad = s.errorNodes.count(node.name) > 0, odd = s.warningNodes.count(node.name) > 0;
        if (bad)
            ed::PushStyleColor(ed::StyleColor_NodeBorder, gui::semanticColor(theme.error));
        else if (odd)
            ed::PushStyleColor(ed::StyleColor_NodeBorder, gui::semanticColor(theme.warning));
        ed::BeginNode(id);
        ImGui::PushID(static_cast<int>(id));
        ImGui::TextUnformatted(node.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", node.type.c_str());
        if (!desc) {
            ImGui::TextColored(gui::semanticColor(theme.error), "Unknown node type");
            ImGui::PopID();
            ed::EndNode();
            if (bad || odd) ed::PopStyleColor();
            continue;
        }
        ImGui::BeginGroup();
        for (size_t k = 0; k < desc->execInputs.size(); ++k) {
            const uint64_t pid = pinId(id, false, true, uint32_t(k));
            ed::BeginPin(pid, ed::PinKind::Input);
            drawPinIcon(PinType::Exec, connectedPins.count(pid) > 0);
            if (k > 0 || desc->execInputs.size() > 1) {
                ImGui::SameLine();
                ImGui::TextUnformatted(desc->execInputs[k].c_str());
            }
            ed::EndPin();
        }
        for (size_t k = 0; k < desc->inputs.size(); ++k) {
            const PinDesc& pin = desc->inputs[k];
            const uint64_t pid = pinId(id, false, false, uint32_t(k));
            const bool linked = connectedPins.count(pid) > 0;
            ed::BeginPin(pid, ed::PinKind::Input);
            drawPinIcon(pin.type, linked);
            ImGui::SameLine();
            ImGui::TextUnformatted(pin.name.c_str());
            ed::EndPin();
            if (linked) continue;
            // A value field for the unconnected input: a literal, or a variable's name.
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(k));
            auto given = std::find_if(
                node.inputs.begin(), node.inputs.end(), [&](const auto& in) { return in.pin == pin.name; });
            std::string text = given == node.inputs.end() ? std::string() : given->text;
            bool edited = false;
            if (pin.type == PinType::Bool && (text.empty() || text == "true" || text == "false")) {
                bool on = text.empty() ? pin.defaultValue == "true" : text == "true";
                if (ImGui::Checkbox("##v", &on)) {
                    remember(s);
                    text = on ? "true" : "false";
                    edited = true;
                }
            } else {
                ImGui::SetNextItemWidth(
                    (pin.type == PinType::String || pin.type == PinType::Vec3 ? 120.0f : 70.0f) * dpi);
                const std::string hint = pin.defaultValue.empty() ? (pin.required ? "needed" : "") : pin.defaultValue;
                if (ImGui::InputTextWithHint("##v", hint.c_str(), &text)) edited = true;
                if (ImGui::IsItemActivated()) remember(s);
            }
            if (edited) {
                std::erase_if(node.inputs, [&](const auto& in) { return in.pin == pin.name; });
                if (!text.empty()) {
                    CircuitDocument::Input input;
                    input.pin = pin.name;
                    input.text = text;
                    node.inputs.push_back(input);
                }
                changed(s);
            }
            ImGui::PopID();
        }
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, 16.0f * dpi);
        ImGui::BeginGroup();
        for (size_t k = 0; k < desc->execOutputs.size(); ++k) {
            const uint64_t pid = pinId(id, true, true, uint32_t(k));
            ed::BeginPin(pid, ed::PinKind::Output);
            if (desc->execOutputs[k] != "then" || desc->execOutputs.size() > 1) {
                ImGui::TextUnformatted(desc->execOutputs[k].c_str());
                ImGui::SameLine();
            }
            drawPinIcon(PinType::Exec, connectedPins.count(pid) > 0);
            ed::EndPin();
        }
        for (size_t k = 0; k < desc->outputs.size(); ++k) {
            const uint64_t pid = pinId(id, true, false, uint32_t(k));
            ed::BeginPin(pid, ed::PinKind::Output);
            ImGui::TextUnformatted(desc->outputs[k].name.c_str());
            ImGui::SameLine();
            drawPinIcon(desc->outputs[k].type, connectedPins.count(pid) > 0);
            ed::EndPin();
        }
        ImGui::EndGroup();
        ImGui::PopID();
        ed::EndNode();
        if (bad || odd) ed::PopStyleColor();
        headers.emplace_back(id, desc->event != shockwave::CircuitEvent::None);
    }
    // Event nodes get a coloured header behind their title.
    for (const auto& [id, isEvent] : headers) {
        if (!isEvent) continue;
        ImDrawList* draw = ed::GetNodeBackgroundDrawList(id);
        const ImVec2 pos = ed::GetNodePosition(id), size = ed::GetNodeSize(id);
        draw->AddRectFilled(
            pos, ImVec2(pos.x + size.x, pos.y + ImGui::GetTextLineHeight() + style.NodePadding.y * 1.5f), eventHeader);
    }
    for (const Link& link : links)
        ed::Link(link.id, link.from, link.to, pinColor(link.type), link.type == PinType::Exec ? 2.5f : 2.0f);

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
        if (ed::QueryNewNode(&pin) && pin) {
            if (ed::AcceptNewItem()) {
                s.dragFrom = pin;
                s.newNodeAt = ed::ScreenToCanvas(ImGui::GetMousePos());
                s.search.clear();
                s.focusSearch = true;
                ed::Suspend();
                ImGui::OpenPopup("Add node");
                ed::Resume();
            }
        }
        ed::EndCreate();   // only after a BeginCreate that returned true
    }

    // Deleting (Delete key, or the menus below).
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
    if (ed::ShowNodeContextMenu(&menuNode)) ImGui::OpenPopup("Node");
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
        std::vector<std::pair<std::string, std::string>> choices;   // type, description
        for (const auto& variable : s.doc.variables) {
            choices.emplace_back("Get." + variable.name, "The variable " + variable.name);
            choices.emplace_back("Set." + variable.name, "Sets the variable " + variable.name);
        }
        for (const auto& node : shockwave::allNodes()) {
            if (node->event != shockwave::CircuitEvent::None && !shockwave::eventAllowed(node->event, s.kind)) continue;
            choices.emplace_back(node->id, node->description);
        }
        std::string category;
        int shownCount = 0;
        for (const auto& [type, description] : choices) {
            if (!filter.empty() && lower(type).find(filter) == std::string::npos) continue;
            // Components make hundreds of nodes; without a search, list the rest first.
            if (filter.empty() && type.rfind("Component.", 0) == 0) continue;
            const std::string group = type.substr(0, type.find('.'));
            if (group != category) {
                category = group;
                ImGui::SeparatorText(category.c_str());
            }
            if (ImGui::Selectable(type.c_str())) {
                addNode(s, type, s.newNodeAt, s.dragFrom);
                s.dragFrom = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("%s", description.c_str());
            if (++shownCount > 400) break;
        }
        if (filter.empty()) ImGui::TextDisabled("Type to search component nodes too.");
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Node")) {
        const int index = nodeIndex(s, uint32_t(menuNode.Get()));
        if (index >= 0) {
            ImGui::TextDisabled("%s", s.doc.nodes[size_t(index)].type.c_str());
            if (ImGui::MenuItem("Rename...")) {
                s.renaming = s.doc.nodes[size_t(index)].name;
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
    if (!s.renaming.empty()) ImGui::OpenPopup("Rename node");
    if (ImGui::BeginPopupModal("Rename node", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(200.0f * dpi);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##rename", &s.renameText, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = identifier(s.renameText) && s.renameText != "self" &&
                           (s.renameText == s.renaming || nodeIndex(s, s.renameText) < 0);
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

    if (fit || (s.placeNodes && !s.doc.nodes.empty())) ed::NavigateToContent(s.placeNodes ? 0.0f : -1.0f);
    s.placeNodes = false;
    ed::End();

    // Where nodes are now, for the file's editor block.
    for (size_t i = 0; i < s.doc.nodes.size(); ++i) {
        auto& node = s.doc.nodes[i];
        const ImVec2 at = ed::GetNodePosition(s.ids[i]);
        if (std::abs(at.x - node.position.x) > 0.5f || std::abs(at.y - node.position.y) > 0.5f || !node.placed) {
            node.position = {std::round(at.x), std::round(at.y)};
            node.placed = true;
            s.dirty = true;
        }
    }
    ed::SetCurrentEditor(nullptr);
    ImGui::End();
}

}   // namespace editor
