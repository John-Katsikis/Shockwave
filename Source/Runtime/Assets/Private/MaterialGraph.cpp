#include <Assets/MaterialGraph.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace shockwave {

namespace {

constexpr GraphValue F = GraphValue::Float, V2 = GraphValue::Vec2, V3 = GraphValue::Vec3, V4 = GraphValue::Vec4,
                     A = GraphValue::Any;

const std::vector<GraphNodeInfo> kNodes = {
    {"constant", "1 to 4 numbers", {}, {{"out", A}}, "A fixed number, vec2, vec3 or vec4"},
    {"param",
     "name, then 1 to 4 numbers (its default)",
     {},
     {{"out", A}},
     "A named value a .swmaterial can change with \"graph_param <name> <values>\""},
    {"texture",
     "\"path\", then \"linear\" for data (normals, masks)",
     {{"uv", V2, true}},
     {{"rgba", V4}, {"rgb", V3}, {"r", F}, {"g", F}, {"b", F}, {"a", F}},
     "A texture from Content, read at uv (default: the mesh's first UV set)"},
    {"uv", "0 or 1 (default 0)", {}, {{"out", V2}}, "The mesh's texture coordinates"},
    {"world_position", "", {}, {{"out", V3}}, "The surface's position in the world, in metres"},
    {"world_normal", "", {}, {{"out", V3}}, "The surface's normal in the world, before any normal map"},
    {"vertex_color", "", {}, {{"rgba", V4}, {"rgb", V3}}, "The mesh's vertex colour"},
    {"time", "", {}, {{"out", F}}, "Seconds on the game's frame clock (repeats under --fixed-step)"},
    {"add", "", {{"a", A}, {"b", A}}, {{"out", A}}, "a + b"},
    {"subtract", "", {{"a", A}, {"b", A}}, {{"out", A}}, "a - b"},
    {"multiply", "", {{"a", A}, {"b", A}}, {{"out", A}}, "a * b, per component"},
    {"divide", "", {{"a", A}, {"b", A}}, {{"out", A}}, "a / b, per component"},
    {"lerp", "", {{"a", A}, {"b", A}, {"t", A}}, {{"out", A}}, "a where t is 0, b where it's 1"},
    {"clamp", "", {{"x", A}, {"min", A, true}, {"max", A, true}}, {{"out", A}}, "x kept within min (0) and max (1)"},
    {"saturate", "", {{"x", A}}, {{"out", A}}, "x kept within 0 and 1"},
    {"power", "", {{"a", A}, {"b", A}}, {{"out", A}}, "a to the power b (a below 0 counts as 0)"},
    {"one_minus", "", {{"x", A}}, {{"out", A}}, "1 - x"},
    {"dot", "", {{"a", A}, {"b", A}}, {{"out", F}}, "The dot product of a and b"},
    {"normalize", "", {{"x", A}}, {{"out", A}}, "x scaled to length 1"},
    {"split",
     "",
     {{"x", A}},
     {{"r", F}, {"g", F}, {"b", F}, {"a", F}},
     "x's components (missing ones are 0, a missing alpha 1)"},
    {"combine",
     "",
     {{"r", F}, {"g", F}, {"b", F, true}, {"a", F, true}},
     {{"rgba", V4}, {"rgb", V3}, {"rg", V2}},
     "Numbers put together into a vector (b defaults to 0, a to 1)"},
    {"fresnel",
     "",
     {{"power", F, true}},
     {{"out", F}},
     "0 facing the camera, rising to 1 at grazing angles: (1 - n.v) to the power (default 5)"},
    {"noise", "", {{"uv", V2}}, {{"out", F}}, "Smooth value noise of uv, 0 to 1, one cell per unit"},
    {"panner",
     "",
     {{"uv", V2, true}, {"speed", V2}},
     {{"out", V2}},
     "uv (default: the mesh's first UV set) plus time times speed: scrolling textures"},
    {"normal_blend",
     "",
     {{"a", V3}, {"b", V3}},
     {{"out", V3}},
     "Tangent-space normal b laid over a (reoriented normal mapping)"},
};

const std::vector<GraphSurfaceInfo> kSurfaces = {
    {"base_color", V3, "Linear RGB"},
    {"opacity", F, "Alpha for alpha_mode blend (mask still cuts out with the base colour's alpha)"},
    {"normal", V3, "Tangent-space normal, blue up (a texture node marked linear reads one)"},
    {"roughness", F, "Perceptual roughness, 0 mirror to 1 matte"},
    {"metallic", F, "0 dielectric to 1 metal"},
    {"emissive", V3, "Emitted light, linear RGB (replaces the material's)"},
    {"occlusion", F, "Darkens ambient light only, 1 none"},
};

const char* kSurfaceFields[] = {"baseColor", "opacity", "normal", "roughness", "metallic", "emissive", "occlusion"};

bool identifier(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(
        s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool number(std::string_view s, float& out) {
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    if (begin != end && *begin == '+') ++begin;
    const auto r = std::from_chars(begin, end, out);
    return r.ec == std::errc() && r.ptr == end && std::isfinite(out);
}

struct Token {
    std::string text;
    bool quoted = false;
};
bool tokenize(std::string_view line, std::vector<Token>& out, std::string& error) {
    out.clear();
    size_t i = 0;
    while (i < line.size()) {
        if (std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
            continue;
        }
        if (line[i] == '#') break;
        Token t;
        if (line[i] == '"') {
            t.quoted = true;
            ++i;
            bool closed = false;
            while (i < line.size()) {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    t.text += line[i + 1];
                    i += 2;
                    continue;
                }
                if (line[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                }
                t.text += line[i++];
            }
            if (!closed) {
                error = "a quoted string isn't closed";
                return false;
            }
        } else {
            while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])) && line[i] != '#')
                t.text += line[i++];
        }
        out.push_back(std::move(t));
    }
    return true;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// The shortest text that reads back exactly, never with an exponent for everyday sizes.
std::string formatNumber(float v) {
    char buffer[64];
    const auto r = std::to_chars(buffer, buffer + sizeof(buffer), v);
    return r.ec == std::errc() ? std::string(buffer, r.ptr) : "0";
}

std::string glslNumber(float v) {
    std::string s = formatNumber(v);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

const char* typeName(GraphValue t) {
    switch (t) {
    case GraphValue::Float: return "float";
    case GraphValue::Vec2: return "vec2";
    case GraphValue::Vec3: return "vec3";
    case GraphValue::Vec4: return "vec4";
    case GraphValue::Any: return "any";
    }
    return "float";
}

}   // namespace

const char* graphValueName(GraphValue type) { return typeName(type); }

const MaterialGraphNode* MaterialGraph::find(std::string_view id) const {
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const std::vector<GraphNodeInfo>& materialGraphNodeTypes() { return kNodes; }
const GraphNodeInfo* findMaterialGraphNodeType(std::string_view type) {
    for (const auto& n : kNodes)
        if (type == n.type) return &n;
    return nullptr;
}
const std::vector<GraphSurfaceInfo>& materialGraphSurfaces() { return kSurfaces; }

// ---- Text ----

bool parseMaterialGraph(std::string_view text, MaterialGraph& graph, std::string& error) {
    graph = {};
    std::istringstream in{std::string(text)};
    std::string line;
    int number_ = 0;
    bool header = false;
    std::vector<Token> tokens;
    auto fail = [&](const std::string& message) {
        error = "line " + std::to_string(number_) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++number_;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // "# layout id x y": the editor's positions.
        {
            std::string_view rest = line;
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.remove_prefix(1);
            if (rest.rfind("# layout ", 0) == 0) {
                std::istringstream layout{std::string(rest.substr(9))};
                std::string id, xs, ys;
                float x = 0.0f, y = 0.0f;
                if (!(layout >> id >> xs >> ys) || !number(xs, x) || !number(ys, y))
                    return fail("expected \"# layout <node> <x> <y>\"");
                bool found = false;
                for (auto& n : graph.nodes)
                    if (n.id == id) {
                        n.placed = true;
                        n.position = {x, y};
                        found = true;
                    }
                if (!found) return fail("layout for node '" + id + "', which isn't in the graph above it");
                continue;
            }
        }
        if (!tokenize(line, tokens, error)) return fail(error);
        if (tokens.empty()) continue;
        if (!header) {
            if (tokens.size() != 2 || tokens[0].text != "swmatgraph" || tokens[1].text != "1")
                return fail("a material graph starts with \"swmatgraph 1\"");
            header = true;
            continue;
        }
        const std::string& keyword = tokens[0].text;
        if (keyword == "node") {
            if (tokens.size() < 3) return fail("expected \"node <id> <type> ...\"");
            MaterialGraphNode node;
            node.id = tokens[1].text;
            node.type = tokens[2].text;
            node.line = number_;
            if (!identifier(node.id)) return fail("node id '" + node.id + "' isn't a name (letters, digits, _)");
            if (graph.find(node.id)) return fail("node '" + node.id + "' is defined twice");
            for (size_t i = 3; i < tokens.size(); ++i) {
                const Token& t = tokens[i];
                const size_t eq = t.quoted ? std::string::npos : t.text.find('=');
                if (eq != std::string::npos) {
                    MaterialGraphLink link;
                    link.pin = t.text.substr(0, eq);
                    const std::string target = t.text.substr(eq + 1);
                    if (link.pin.empty() || target.empty())
                        return fail("expected <pin>=<node>[.<output>] or <pin>=<number>");
                    float v = 0.0f;
                    if (number(target, v)) {
                        link.literal = true;
                        link.value = v;
                    } else {
                        const size_t dot = target.find('.');
                        link.node = target.substr(0, dot);
                        if (dot != std::string::npos) link.output = target.substr(dot + 1);
                        if (!identifier(link.node) || (dot != std::string::npos && !identifier(link.output)))
                            return fail("'" + target + "' isn't a node, node.output or number");
                    }
                    node.inputs.push_back(std::move(link));
                    continue;
                }
                float v = 0.0f;
                if (t.quoted) {
                    if (!node.text.empty()) return fail("node '" + node.id + "' has two quoted values");
                    node.text = t.text;
                    if (node.text.empty()) return fail("node '" + node.id + "' has an empty path");
                } else if (number(t.text, v)) {
                    node.values.push_back(v);
                } else if (t.text == "linear") {
                    node.linear = true;
                } else if (node.text.empty()) {
                    node.text = t.text;
                } else {
                    return fail("unexpected '" + t.text + "'");
                }
            }
            graph.nodes.push_back(std::move(node));
        } else if (keyword == "output") {
            if (tokens.size() != 3) return fail("expected \"output <surface> <node>[.<output>]\"");
            MaterialGraphOutput out;
            out.surface = tokens[1].text;
            const std::string& target = tokens[2].text;
            const size_t dot = target.find('.');
            out.node = target.substr(0, dot);
            if (dot != std::string::npos) out.output = target.substr(dot + 1);
            out.line = number_;
            if (!identifier(out.node)) return fail("'" + target + "' isn't a node or node.output");
            graph.outputs.push_back(std::move(out));
        } else {
            return fail("unknown line '" + keyword + "' (expected node, output or # layout)");
        }
    }
    if (!header) {
        number_ = 1;
        return fail("a material graph starts with \"swmatgraph 1\"");
    }
    return true;
}

std::string writeMaterialGraph(const MaterialGraph& graph) {
    std::string out = "swmatgraph 1\n";
    for (const auto& n : graph.nodes) {
        out += "node " + n.id + " " + n.type;
        if (!n.text.empty()) out += " " + (n.type == "param" && identifier(n.text) ? n.text : quote(n.text));
        if (n.linear) out += " linear";
        for (float v : n.values) out += " " + formatNumber(v);
        for (const auto& l : n.inputs) {
            out += " " + l.pin + "=";
            if (l.literal)
                out += formatNumber(l.value);
            else
                out += l.node + (l.output.empty() ? "" : "." + l.output);
        }
        out += "\n";
    }
    for (const auto& o : graph.outputs)
        out += "output " + o.surface + " " + o.node + (o.output.empty() ? "" : "." + o.output) + "\n";
    bool first = true;
    for (const auto& n : graph.nodes) {
        if (!n.placed) continue;
        if (first) out += "\n";
        first = false;
        out += "# layout " + n.id + " " + formatNumber(n.position.x) + " " + formatNumber(n.position.y) + "\n";
    }
    return out;
}

// ---- Compiling ----

bool compileMaterialGraph(
    const MaterialGraph& graph, MaterialGraphCode& code, std::vector<MaterialGraphError>& errors) {
    code = {};
    errors.clear();
    auto error = [&](const std::string& node, std::string message) { errors.push_back({node, std::move(message)}); };
    if (graph.nodes.size() > kMaxGraphNodes)
        error({}, std::to_string(graph.nodes.size()) + " nodes; a graph has at most " + std::to_string(kMaxGraphNodes));
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < graph.nodes.size(); ++i) index[graph.nodes[i].id] = i;

    // Each node's own values, and its slots.
    std::vector<const GraphNodeInfo*> info(graph.nodes.size(), nullptr);
    std::vector<int> slot(graph.nodes.size(), -1);
    std::set<std::string> paramNames;
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        const MaterialGraphNode& n = graph.nodes[i];
        info[i] = findMaterialGraphNodeType(n.type);
        if (!info[i]) {
            error(n.id, "unknown node type '" + n.type + "'");
            continue;
        }
        const bool counted = n.type == "constant" || n.type == "param";
        if (counted && (n.values.empty() || n.values.size() > 4))
            error(n.id, n.type + " needs 1 to 4 numbers, not " + std::to_string(n.values.size()));
        if (n.type == "uv" &&
            (n.values.size() > 1 || (n.values.size() == 1 && n.values[0] != 0.0f && n.values[0] != 1.0f)))
            error(n.id, "uv takes 0 or 1 (the UV set)");
        if (!counted && n.type != "uv" && !n.values.empty()) error(n.id, n.type + " takes no numbers");
        if (n.type == "param") {
            if (!identifier(n.text))
                error(n.id, "param needs a name (letters, digits, _)");
            else if (!paramNames.insert(n.text).second)
                error(n.id, "a second param called '" + n.text + "'");
            slot[i] = int(code.params.size());
            MaterialGraphParam p;
            p.name = n.text;
            p.components = int(std::clamp<size_t>(n.values.size(), 1, 4));
            for (size_t c = 0; c < n.values.size() && c < 4; ++c) p.value[int(c)] = n.values[c];
            code.params.push_back(p);
        } else if (n.type == "texture") {
            if (n.text.empty()) error(n.id, "texture needs a \"path\" in Content");
            slot[i] = int(code.textures.size());
            code.textures.push_back({n.text, n.linear});
        } else if (!n.text.empty()) {
            error(n.id, n.type + " takes no name or path ('" + n.text + "')");
        }
        if (n.linear && n.type != "texture") error(n.id, "only a texture can be linear");
    }
    if (code.params.size() > kMaxGraphParams)
        error(
            {}, std::to_string(code.params.size()) + " params; a graph has at most " + std::to_string(kMaxGraphParams));
    if (code.textures.size() > kMaxGraphTextures)
        error(
            {}, std::to_string(code.textures.size()) + " textures; a graph has at most " +
                    std::to_string(kMaxGraphTextures));

    // Links: known pins, each once, to nodes and outputs that exist.
    auto outputIndex = [&](size_t node, const std::string& name) -> int {
        if (!info[node]) return -1;
        const auto& outs = info[node]->outputs;
        if (name.empty()) return 0;
        for (size_t o = 0; o < outs.size(); ++o)
            if (name == outs[o].name) return int(o);
        return -1;
    };
    std::vector<std::vector<size_t>> sources(graph.nodes.size());
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        const MaterialGraphNode& n = graph.nodes[i];
        if (!info[i]) continue;
        std::set<std::string> filled;
        for (const auto& l : n.inputs) {
            const bool known = std::any_of(
                info[i]->inputs.begin(), info[i]->inputs.end(), [&](const GraphPinInfo& p) { return l.pin == p.name; });
            if (!known) {
                error(n.id, n.type + " has no input '" + l.pin + "'");
                continue;
            }
            if (!filled.insert(l.pin).second) error(n.id, "input '" + l.pin + "' is connected twice");
            if (l.literal) continue;
            const auto it = index.find(l.node);
            if (it == index.end()) {
                error(n.id, "input '" + l.pin + "' links to '" + l.node + "', which isn't in the graph");
                continue;
            }
            if (info[it->second] && outputIndex(it->second, l.output) < 0)
                error(
                    n.id, "input '" + l.pin + "' links to " + l.node + "." + l.output + ", but a " +
                              graph.nodes[it->second].type + " has no output '" + l.output + "'");
            sources[i].push_back(it->second);
        }
        for (const auto& p : info[i]->inputs)
            if (!p.optional && !filled.count(p.name)) error(n.id, n.type + "'s input '" + p.name + "' isn't connected");
    }
    std::set<std::string> surfaces;
    for (const auto& o : graph.outputs) {
        const bool known = std::any_of(
            kSurfaces.begin(), kSurfaces.end(), [&](const GraphSurfaceInfo& s) { return o.surface == s.name; });
        if (!known)
            error(
                o.node, "unknown output '" + o.surface +
                            "' (base_color, opacity, normal, roughness, "
                            "metallic, emissive or occlusion)");
        else if (!surfaces.insert(o.surface).second)
            error(o.node, "output " + o.surface + " is connected twice");
        const auto it = index.find(o.node);
        if (it == index.end())
            error(o.node, "output " + o.surface + " links to '" + o.node + "', which isn't in the graph");
        else if (info[it->second] && outputIndex(it->second, o.output) < 0)
            error(o.node, "output " + o.surface + " links to " + o.node + "." + o.output + ", which doesn't exist");
    }
    if (!errors.empty()) return false;

    // Dependency order; a cycle names a node on it.
    std::vector<int> state(graph.nodes.size(), 0);   // 0 new, 1 visiting, 2 done
    std::vector<size_t> order;
    bool cycle = false;
    std::function<void(size_t)> visit = [&](size_t i) {
        if (state[i] == 2 || cycle) return;
        if (state[i] == 1) {
            cycle = true;
            error(graph.nodes[i].id, "is part of a cycle: its inputs lead back to it");
            return;
        }
        state[i] = 1;
        for (size_t s : sources[i]) visit(s);
        state[i] = 2;
        order.push_back(i);
    };
    for (size_t i = 0; i < graph.nodes.size() && !cycle; ++i) visit(i);
    if (cycle) return false;

    // Types, in that order.
    std::vector<GraphValue> nodeType(graph.nodes.size(), F);
    auto outputType = [&](size_t node, const std::string& name) {
        const GraphValue t = info[node]->outputs[size_t(outputIndex(node, name))].type;
        return t == A ? nodeType[node] : t;
    };
    auto linkType = [&](const MaterialGraphLink& l) { return l.literal ? F : outputType(index[l.node], l.output); };
    auto linkName = [&](const MaterialGraphLink& l) {
        return l.literal ? std::string("a number") : "'" + l.node + "'";
    };
    for (size_t i : order) {
        const MaterialGraphNode& n = graph.nodes[i];
        if (n.type == "constant" || n.type == "param") {
            nodeType[i] = GraphValue(std::clamp<size_t>(n.values.size(), 1, 4));
            continue;
        }
        // The widest of the Any inputs; two different vectors don't mix.
        GraphValue widest = F;
        const MaterialGraphLink* widestLink = nullptr;
        for (const auto& l : n.inputs) {
            const auto pin = std::find_if(
                info[i]->inputs.begin(), info[i]->inputs.end(), [&](const GraphPinInfo& p) { return l.pin == p.name; });
            const GraphValue t = linkType(l);
            if (pin->type == A) {
                if (t == F) continue;
                if (widest != F && t != widest) {
                    error(
                        n.id, "input '" + l.pin + "' is a " + typeName(t) + " from " + linkName(l) + " but input '" +
                                  widestLink->pin + "' is a " + typeName(widest) + " from " + linkName(*widestLink) +
                                  "; only a float mixes with a vector");
                    continue;
                }
                widest = t;
                widestLink = &l;
            } else if (t != F && t != pin->type) {
                error(
                    n.id, "input '" + l.pin + "' needs a " + std::string(typeName(pin->type)) + " but " + linkName(l) +
                              " gives a " + typeName(t));
            }
        }
        nodeType[i] = widest;
    }
    for (const auto& o : graph.outputs) {
        const auto s = std::find_if(
            kSurfaces.begin(), kSurfaces.end(), [&](const GraphSurfaceInfo& x) { return o.surface == x.name; });
        const GraphValue t = outputType(index[o.node], o.output);
        if (t != F && t != s->type)
            error(
                o.node, "output " + o.surface + " needs a " + std::string(typeName(s->type)) + " but '" + o.node +
                            "' gives a " + typeName(t));
    }
    if (!errors.empty()) return false;

    // The GLSL: one statement per node.
    auto var = [&](size_t node) { return "n" + std::to_string(node); };
    auto expression = [&](size_t node, const std::string& output) {
        const MaterialGraphNode& n = graph.nodes[node];
        const std::string& name = info[node]->outputs[size_t(outputIndex(node, output))].name;
        const bool swizzled =
            n.type == "texture" || n.type == "split" || n.type == "combine" || n.type == "vertex_color";
        if (!swizzled || name == "rgba") return var(node);
        return var(node) + "." + name;
    };
    auto convert = [&](const std::string& value, GraphValue from, GraphValue to) {
        if (from == to || to == F) return value;
        return std::string(typeName(to)) + "(" + value + ")";
    };
    auto input = [&](size_t node, const char* pin, GraphValue to, const std::string& fallback) {
        for (const auto& l : graph.nodes[node].inputs) {
            if (l.pin != pin) continue;
            if (l.literal) return convert(glslNumber(l.value), F, to);
            const size_t source = index[l.node];
            return convert(expression(source, l.output), outputType(source, l.output), to);
        }
        return fallback;
    };
    std::ostringstream g;
    g << "// Generated from a .swmatgraph (Assets/MaterialGraph.cpp); edit the graph, not this.\n"
         "vec4 graphVec4(float x) { return vec4(x, x, x, 1.0); }\n"
         "vec4 graphVec4(vec2 x) { return vec4(x, 0.0, 1.0); }\n"
         "vec4 graphVec4(vec3 x) { return vec4(x, 1.0); }\n"
         "vec4 graphVec4(vec4 x) { return x; }\n"
         "float graphHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
         "float graphNoise(vec2 p) {\n"
         "    vec2 i = floor(p), f = fract(p);\n"
         "    vec2 u = f * f * (3.0 - 2.0 * f);\n"
         "    return mix(mix(graphHash(i), graphHash(i + vec2(1.0, 0.0)), u.x),\n"
         "               mix(graphHash(i + vec2(0.0, 1.0)), graphHash(i + vec2(1.0, 1.0)), u.x), u.y);\n"
         "}\n"
         "void materialGraphSurface(GraphShadingData g, float graphTime, vec3 graphView, inout MaterialGraphSurface s) "
         "{\n";
    for (size_t i : order) {
        const MaterialGraphNode& n = graph.nodes[i];
        const GraphValue T = nodeType[i];
        const std::string t = typeName(T);
        const std::string v = var(i);
        std::string statement;
        auto any = [&](const char* pin, const std::string& fallback = "") { return input(i, pin, T, fallback); };
        if (n.type == "constant") {
            std::string args;
            for (size_t c = 0; c < n.values.size(); ++c) args += (c ? ", " : "") + glslNumber(n.values[c]);
            statement = t + " " + v + " = " + (T == F ? args : t + "(" + args + ")");
        } else if (n.type == "param") {
            const char* swizzle[] = {".x", ".xy", ".xyz", ""};
            statement = t + " " + v + " = g.params[" + std::to_string(slot[i]) + "]" + swizzle[int(T) - 1];
        } else if (n.type == "texture") {
            const std::string k = std::to_string(slot[i]);
            statement = "vec4 " + v + " = texture(sampler2D(textures[nonuniformEXT(g.textures[" + k +
                        "])], materialSamplers[nonuniformEXT(g.samplers[" + k + "])]), " +
                        input(i, "uv", V2, "vUvs.xy") + ")";
        } else if (n.type == "uv") {
            statement = "vec2 " + v + " = " + (!n.values.empty() && n.values[0] == 1.0f ? "vUvs.zw" : "vUvs.xy");
        } else if (n.type == "world_position") {
            statement = "vec3 " + v + " = vWorldPosition";
        } else if (n.type == "world_normal") {
            statement = "vec3 " + v + " = normalize(vNormal)";
        } else if (n.type == "vertex_color") {
            statement = "vec4 " + v + " = vColor";
        } else if (n.type == "time") {
            statement = "float " + v + " = graphTime";
        } else if (n.type == "add" || n.type == "subtract" || n.type == "multiply" || n.type == "divide") {
            const char* op = n.type == "add"        ? " + "
                             : n.type == "subtract" ? " - "
                             : n.type == "multiply" ? " * "
                                                    : " / ";
            statement = t + " " + v + " = " + any("a") + op + any("b");
        } else if (n.type == "lerp") {
            statement = t + " " + v + " = mix(" + any("a") + ", " + any("b") + ", " + any("t") + ")";
        } else if (n.type == "clamp") {
            statement = t + " " + v + " = clamp(" + any("x") + ", " + any("min", convert("0.0", F, T)) + ", " +
                        any("max", convert("1.0", F, T)) + ")";
        } else if (n.type == "saturate") {
            statement =
                t + " " + v + " = clamp(" + any("x") + ", " + convert("0.0", F, T) + ", " + convert("1.0", F, T) + ")";
        } else if (n.type == "power") {
            statement = t + " " + v + " = pow(max(" + any("a") + ", " + convert("0.0", F, T) + "), " + any("b") + ")";
        } else if (n.type == "one_minus") {
            statement = t + " " + v + " = " + convert("1.0", F, T) + " - " + any("x");
        } else if (n.type == "dot") {
            statement = "float " + v + " = dot(" + any("a") + ", " + any("b") + ")";
        } else if (n.type == "normalize") {
            statement = t + " " + v + " = normalize(" + any("x") + ")";
        } else if (n.type == "split") {
            statement = "vec4 " + v + " = graphVec4(" + any("x") + ")";
        } else if (n.type == "combine") {
            statement = "vec4 " + v + " = vec4(" + input(i, "r", F, "0.0") + ", " + input(i, "g", F, "0.0") + ", " +
                        input(i, "b", F, "0.0") + ", " + input(i, "a", F, "1.0") + ")";
        } else if (n.type == "fresnel") {
            statement = "float " + v + " = pow(1.0 - clamp(dot(normalize(vNormal), graphView), 0.0, 1.0), " +
                        input(i, "power", F, "5.0") + ")";
        } else if (n.type == "noise") {
            statement = "float " + v + " = graphNoise(" + input(i, "uv", V2, "vUvs.xy") + ")";
        } else if (n.type == "panner") {
            statement = "vec2 " + v + " = " + input(i, "uv", V2, "vUvs.xy") + " + graphTime * " +
                        input(i, "speed", V2, "vec2(0.0)");
        } else if (n.type == "normal_blend") {
            statement = "vec3 " + v + " = composeNormals(normalize(" + input(i, "a", V3, "vec3(0.0, 0.0, 1.0)") +
                        "), normalize(" + input(i, "b", V3, "vec3(0.0, 0.0, 1.0)") + "))";
        }
        g << "    " << statement << ";   // " << n.id << " (" << n.type << ")\n";
    }
    for (const auto& o : graph.outputs) {
        size_t s = 0;
        while (kSurfaces[s].name != o.surface) ++s;
        const size_t source = index[o.node];
        g << "    s." << kSurfaceFields[s] << " = "
          << convert(expression(source, o.output), outputType(source, o.output), kSurfaces[s].type) << ";\n";
        if (o.surface == "normal") code.normal = true;
    }
    g << "}\n";
    code.glsl = g.str();
    return true;
}

std::string materialGraphErrorText(const std::vector<MaterialGraphError>& errors) {
    std::string out;
    for (const auto& e : errors) {
        if (!out.empty()) out += "; ";
        out += e.node.empty() ? e.message : "node '" + e.node + "': " + e.message;
    }
    return out;
}

uint64_t materialGraphHash(std::string_view glsl, uint64_t engineHash) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](unsigned char c) {
        h ^= c;
        h *= 1099511628211ull;
    };
    for (char c : glsl) mix(static_cast<unsigned char>(c));
    for (int i = 0; i < 8; ++i) mix(static_cast<unsigned char>(engineHash >> (8 * i)));
    return h;
}

std::string materialGraphShaderSource(const MaterialGraphCode& code) {
    std::string out = "#version 460\n"
                      "#extension GL_EXT_nonuniform_qualifier : require\n"
                      "#extension GL_EXT_buffer_reference : require\n"
                      "#extension GL_EXT_buffer_reference_uvec2 : require\n"
                      "#extension GL_EXT_scalar_block_layout : require\n"
                      "#extension GL_GOOGLE_include_directive : require\n"
                      "#define MATERIAL_GRAPH 1\n";
    if (code.normal) out += "#define MATERIAL_GRAPH_NORMAL 1\n";
    out += "#include \"mesh.glsl\"\n";
    return out + code.glsl;
}

std::string materialGraphSchemaMarkdown() {
    std::ostringstream out;
    out << "# Material graph nodes\n\n"
           "Generated by `player --material-graph-schema`; see docs/MaterialGraph.md for the file format.\n"
           "A float connects to any vector by repeating; `any` pins take the node's own type, the\n"
           "widest of what's connected to them.\n\n"
           "| Node | Values | Inputs | Outputs | What it does |\n|---|---|---|---|---|\n";
    for (const auto& n : kNodes) {
        std::string inputs, outputs;
        for (const auto& p : n.inputs)
            inputs += std::string(inputs.empty() ? "" : ", ") + "`" + p.name + "` " + typeName(p.type) +
                      (p.optional ? " (optional)" : "");
        for (const auto& p : n.outputs)
            outputs += std::string(outputs.empty() ? "" : ", ") + "`" + p.name + "` " + typeName(p.type);
        out << "| `" << n.type << "` | " << (*n.values ? n.values : "") << " | " << (inputs.empty() ? "" : inputs)
            << " | " << outputs << " | " << n.description << " |\n";
    }
    out << "\n## Outputs\n\n| Output | Type | Meaning |\n|---|---|---|\n";
    for (const auto& s : kSurfaces)
        out << "| `" << s.name << "` | " << typeName(s.type) << " | " << s.description << " |\n";
    out << "\nLimits: " << kMaxGraphNodes << " nodes, " << kMaxGraphTextures << " textures and " << kMaxGraphParams
        << " params per graph.\n";
    return out.str();
}

}   // namespace shockwave
