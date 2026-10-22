// Values, the .swcircuit text format, and the Circuit / CircuitVars components.
#include <Circuits/Circuit.h>

#include <Engine/Components.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace shockwave {

namespace {
constexpr const char* kPinTypes[] = {"Exec",     "Bool",   "Int",    "Float", "Vec2",    "Vec3", "Color",
                                     "Rotation", "String", "Entity", "Mesh",  "Texture", "Model"};

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool parseFloats(std::string_view text, float* out, size_t count, size_t& read) {
    read = 0;
    text = trim(text);
    while (!text.empty() && read < count) {
        float value = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || !std::isfinite(value)) return false;
        out[read++] = value;
        text = trim(text.substr(size_t(end - text.data())));
    }
    return text.empty();
}

// A quoted string with \" and \\ escapes, as std::quoted writes it.
bool parseQuoted(std::string_view text, std::string& out) {
    text = trim(text);
    std::istringstream in{std::string(text)};
    if (text.empty() || text.front() != '"' || !(in >> std::quoted(out))) return false;
    in >> std::ws;
    return in.peek() == EOF;
}

std::string quote(std::string_view text) {
    std::ostringstream out;
    out << std::quoted(std::string(text));
    return out.str();
}

std::string formatFloat(float f) {
    char buffer[32];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), f);
    return ec == std::errc{} ? std::string(buffer, end) : std::string("0");
}
}   // namespace

const char* pinTypeName(PinType type) {
    const auto index = static_cast<size_t>(type);
    return index < std::size(kPinTypes) ? kPinTypes[index] : "?";
}

bool parsePinType(std::string_view name, PinType& type) {
    for (size_t i = 0; i < std::size(kPinTypes); ++i) {
        if (name == kPinTypes[i]) {
            type = static_cast<PinType>(i);
            return true;
        }
    }
    return false;
}

Value Value::boolean(bool b) {
    Value v;
    v.type = PinType::Bool;
    v.i = b ? 1 : 0;
    return v;
}
Value Value::integer(int32_t n) {
    Value v;
    v.type = PinType::Int;
    v.i = n;
    return v;
}
Value Value::number(float f) {
    Value v;
    v.type = PinType::Float;
    v.v.x = f;
    return v;
}
Value Value::vec2(const glm::vec2& value) {
    Value v;
    v.type = PinType::Vec2;
    v.v = glm::vec4(value, 0.0f, 0.0f);
    return v;
}
Value Value::vec3(const glm::vec3& value) {
    Value v;
    v.type = PinType::Vec3;
    v.v = glm::vec4(value, 0.0f);
    return v;
}
Value Value::color(const glm::vec4& c) {
    Value v;
    v.type = PinType::Color;
    v.v = c;
    return v;
}
Value Value::rotation(const glm::quat& q) {
    Value v;
    v.type = PinType::Rotation;
    v.v = glm::vec4(q.x, q.y, q.z, q.w);
    return v;
}
Value Value::string(std::string s) {
    Value v;
    v.type = PinType::String;
    v.text = std::move(s);
    return v;
}
Value Value::entityValue(Entity e) {
    Value v;
    v.type = PinType::Entity;
    v.entity = e;
    return v;
}
Value Value::self() {
    Value v;
    v.type = PinType::Entity;
    v.text = "self";
    return v;
}

Value defaultValue(PinType type) {
    switch (type) {
    case PinType::Rotation: return Value::rotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    case PinType::Color: return Value::color(glm::vec4(1.0f));
    case PinType::Entity: return Value::entityValue(kNullEntity);
    case PinType::Mesh:
    case PinType::Model: {   // no mesh or model; texture 0 is white, which is right for it
        Value v;
        v.type = type;
        v.i = -1;
        return v;
    }
    default: {
        Value v;
        v.type = type;
        return v;
    }
    }
}

bool parseValue(std::string_view text, PinType type, Value& value, std::string& error) {
    text = trim(text);
    value = defaultValue(type);
    auto fail = [&](const std::string& expected) {
        error = "'" + std::string(text) + "' isn't " + expected;
        return false;
    };
    float f[4] = {0, 0, 0, 1};
    size_t read = 0;
    switch (type) {
    case PinType::Exec: error = "exec pins take no value"; return false;
    case PinType::Bool:
        if (text == "true" || text == "1")
            value.i = 1;
        else if (text == "false" || text == "0")
            value.i = 0;
        else
            return fail("true or false");
        return true;
    case PinType::Int: {
        int32_t n = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
        if (ec != std::errc{} || end != text.data() + text.size()) return fail("a whole number");
        value.i = n;
        return true;
    }
    case PinType::Float:
        if (!parseFloats(text, f, 1, read) || read != 1) return fail("a number");
        value.v.x = f[0];
        return true;
    case PinType::Vec2:
        if (!parseFloats(text, f, 2, read) || read != 2) return fail("two numbers (x y)");
        value.v = glm::vec4(f[0], f[1], 0, 0);
        return true;
    case PinType::Vec3:
        if (!parseFloats(text, f, 3, read) || read != 3) return fail("three numbers (x y z)");
        value.v = glm::vec4(f[0], f[1], f[2], 0);
        return true;
    case PinType::Color:
        if (!parseFloats(text, f, 4, read) || read < 3) return fail("a color (r g b, or r g b a)");
        value.v = glm::vec4(f[0], f[1], f[2], read == 4 ? f[3] : 1.0f);
        return true;
    case PinType::Rotation:
        if (!parseFloats(text, f, 3, read) || read != 3) return fail("a rotation (x y z, in degrees)");
        value = Value::rotation(fromEulerDegrees(glm::vec3(f[0], f[1], f[2])));
        return true;
    case PinType::String:
        if (!parseQuoted(text, value.text)) return fail("a quoted string");
        return true;
    case PinType::Entity:
        if (text == "self") {
            value = Value::self();
            return true;
        }
        if (text == "none") return true;
        if (!parseQuoted(text, value.text) || value.text.empty()) return fail("self, none or an entity's \"Name\"");
        return true;
    case PinType::Mesh:
    case PinType::Texture:
    case PinType::Model:
        if (!parseQuoted(text, value.text)) return fail("an asset's quoted name");
        return true;
    }
    return fail("a value");
}

std::string formatValue(const Value& value) {
    switch (value.type) {
    case PinType::Exec: return "";
    case PinType::Bool: return value.i ? "true" : "false";
    case PinType::Int: return std::to_string(value.i);
    case PinType::Float: return formatFloat(value.v.x);
    case PinType::Vec2: return formatFloat(value.v.x) + " " + formatFloat(value.v.y);
    case PinType::Vec3: return formatFloat(value.v.x) + " " + formatFloat(value.v.y) + " " + formatFloat(value.v.z);
    case PinType::Color:
        return formatFloat(value.v.x) + " " + formatFloat(value.v.y) + " " + formatFloat(value.v.z) + " " +
               formatFloat(value.v.w);
    case PinType::Rotation: {
        const glm::vec3 e = eulerDegrees(value.asRotation());
        return formatFloat(e.x) + " " + formatFloat(e.y) + " " + formatFloat(e.z);
    }
    case PinType::String: return quote(value.text);
    case PinType::Entity:
        if (value.text == "self") return "self";
        return value.text.empty() ? "none" : quote(value.text);
    case PinType::Mesh:
    case PinType::Texture:
    case PinType::Model: return quote(value.text);
    }
    return "";
}

// ---- Components ----------------------------------------------------------------------------

const char* circuitExecutionName(CircuitExecution execution) {
    switch (execution) {
    case CircuitExecution::Authority: return "Authority";
    case CircuitExecution::LocalOwner: return "LocalOwner";
    case CircuitExecution::Presentation: return "Presentation";
    }
    return "?";
}

void registerCircuitComponents() {
    registerComponent<Circuit>(
        "Circuit",
        {field("circuit", &Circuit::circuit), field("enabled", &Circuit::enabled),
         enumField("execution", &Circuit::execution, {"Authority", "LocalOwner", "Presentation"})},
        "Gameplay");
    registerComponent<CircuitVars>("CircuitVars", {field("values", &CircuitVars::values)}, "Gameplay");
}

std::vector<std::pair<std::string, std::string>> parseCircuitVars(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> result;
    std::string part;
    bool inQuotes = false, escaped = false;
    auto flush = [&] {
        const std::string_view item = trim(part);
        const size_t equals = item.find('=');
        if (equals != std::string_view::npos) {
            const std::string_view name = trim(item.substr(0, equals));
            if (!name.empty()) result.emplace_back(std::string(name), std::string(trim(item.substr(equals + 1))));
        }
        part.clear();
    };
    for (const char c : text) {
        if (inQuotes) {
            part += c;
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inQuotes = false;
            continue;
        }
        if (c == '"') inQuotes = true;
        if (c == ';') {
            flush();
            continue;
        }
        part += c;
    }
    flush();
    return result;
}

std::string formatCircuitVars(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string out;
    for (const auto& [name, value] : values) {
        if (!out.empty()) out += "; ";
        out += name + "=" + value;
    }
    return out;
}

// ---- The file --------------------------------------------------------------------------------

namespace {
bool identifier(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(
        s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// "node.pin" -> both parts, if that's what it is.
bool splitReference(std::string_view text, std::string& node, std::string& pin) {
    const size_t dot = text.find('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view a = text.substr(0, dot), b = text.substr(dot + 1);
    if (!identifier(a) || !identifier(b)) return false;
    node = a;
    pin = b;
    return true;
}
}   // namespace

bool parseCircuit(std::istream& in, const std::string& file, CircuitDocument& document, std::string& error) {
    CircuitDocument result;
    std::string line;
    int number = 0;
    bool header = false, inEditor = false, started = false;
    CircuitDocument::Node* node = nullptr;
    auto fail = [&](const std::string& message) {
        error = file + ":" + std::to_string(number) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string_view text = trim(line);
        if (text.empty()) continue;
        if (text.front() == '#') {
            if (!started && header) {
                std::string_view comment = trim(text.substr(1));
                result.description += (result.description.empty() ? "" : "\n") + std::string(comment);
            }
            continue;
        }
        std::istringstream row{std::string(text)};
        std::string keyword;
        row >> keyword;
        if (!header) {
            int version = 0;
            if (keyword != "shockwave_circuit" || !(row >> version)) return fail("expected shockwave_circuit 1 first");
            if (version != 1) return fail("unsupported circuit version " + std::to_string(version));
            header = true;
            continue;
        }
        if (inEditor) {
            if (keyword == "end") {
                inEditor = false;
                continue;
            }
            if (keyword == "@note") {
                std::string name, note;
                if (!(row >> name >> std::quoted(note))) return fail("@note needs a node and quoted text");
                row >> std::ws;
                if (!row.eof()) return fail("unexpected text after @note");
                auto found = std::find_if(
                    result.nodes.begin(), result.nodes.end(), [&](const auto& n) { return n.name == name; });
                if (found == result.nodes.end()) return fail("@note names unknown node '" + name + "'");
                found->note = std::move(note);
                continue;
            }
            glm::vec2 position;
            if (!(row >> position.x >> position.y)) return fail("editor lines are: <node> <x> <y>");
            auto found = std::find_if(
                result.nodes.begin(), result.nodes.end(), [&](const auto& n) { return n.name == keyword; });
            if (found == result.nodes.end()) return fail("the editor block places unknown node '" + keyword + "'");
            found->position = position;
            found->placed = true;
            continue;
        }
        if (keyword == "var") {
            started = true;
            node = nullptr;
            CircuitDocument::Variable variable;
            std::string type;
            if (!(row >> variable.name >> type)) return fail("var lines are: var <name> <Type> [value]");
            if (!identifier(variable.name)) return fail("'" + variable.name + "' isn't a name (letters, digits, _)");
            if (!parsePinType(type, variable.type) || variable.type == PinType::Exec)
                return fail("unknown type '" + type + "'");
            std::string rest;
            std::getline(row, rest);
            variable.value = std::string(trim(rest));
            variable.line = number;
            if (std::any_of(result.variables.begin(), result.variables.end(), [&](const auto& v) {
                    return v.name == variable.name;
                }))
                return fail("variable '" + variable.name + "' is declared twice");
            result.variables.push_back(std::move(variable));
        } else if (keyword == "node") {
            started = true;
            CircuitDocument::Node next;
            if (!(row >> next.name >> next.type)) return fail("node lines are: node <name> <Type>");
            if (!identifier(next.name)) return fail("'" + next.name + "' isn't a node name (letters, digits, _)");
            if (next.name == "self") return fail("'self' is reserved");
            if (std::any_of(
                    result.nodes.begin(), result.nodes.end(), [&](const auto& n) { return n.name == next.name; }))
                return fail("node '" + next.name + "' is defined twice");
            next.line = number;
            result.nodes.push_back(std::move(next));
            node = &result.nodes.back();
        } else if (keyword == "in") {
            if (!node) return fail("'in' belongs under a node");
            std::string rest;
            std::getline(row, rest);
            const size_t equals = rest.find('=');
            if (equals == std::string::npos) return fail("in lines are: in <pin> = <value, variable or node.pin>");
            CircuitDocument::Input input;
            input.pin = std::string(trim(std::string_view(rest).substr(0, equals)));
            input.text = std::string(trim(std::string_view(rest).substr(equals + 1)));
            input.line = number;
            if (!identifier(input.pin)) return fail("'" + input.pin + "' isn't a pin name");
            if (input.text.empty()) return fail("pin '" + input.pin + "' has nothing after =");
            if (std::any_of(
                    node->inputs.begin(), node->inputs.end(), [&](const auto& i) { return i.pin == input.pin; }))
                return fail("pin '" + input.pin + "' is given twice");
            node->inputs.push_back(std::move(input));
        } else if (keyword == "exec") {
            if (!node) return fail("'exec' belongs under a node");
            std::string rest;
            std::getline(row, rest);
            CircuitDocument::Exec exec;
            exec.line = number;
            std::string_view source = trim(rest);
            if (const size_t equals = source.find('='); equals != std::string_view::npos) {
                exec.pin = std::string(trim(source.substr(0, equals)));
                source = trim(source.substr(equals + 1));
                if (!identifier(exec.pin)) return fail("'" + exec.pin + "' isn't an exec pin name");
            }
            if (!splitReference(source, exec.node, exec.output))
                return fail("exec lines are: exec <node>.<output> or exec <pin> = <node>.<output>");
            node->execs.push_back(std::move(exec));
        } else if (keyword == "editor") {
            node = nullptr;
            inEditor = true;
        } else
            return fail("unknown line '" + keyword + "' (expected var, node, in, exec or editor)");
    }
    if (in.bad()) return fail("error reading the file");
    if (!header) {
        number = 1;
        return fail("expected shockwave_circuit 1 first");
    }
    if (inEditor) return fail("the editor block has no end");
    document = std::move(result);
    error.clear();
    return true;
}

bool parseCircuitText(std::string_view text, const std::string& file, CircuitDocument& document, std::string& error) {
    std::istringstream in{std::string(text)};
    return parseCircuit(in, file, document, error);
}

void writeCircuit(std::ostream& out, const CircuitDocument& document) {
    out << "shockwave_circuit 1\n";
    if (!document.description.empty()) {
        std::istringstream lines(document.description);
        std::string line;
        while (std::getline(lines, line)) out << "# " << line << "\n";
    }
    if (!document.variables.empty()) out << "\n";
    for (const auto& variable : document.variables) {
        out << "var " << variable.name << " " << pinTypeName(variable.type);
        if (!variable.value.empty()) out << " " << variable.value;
        out << "\n";
    }
    for (const auto& node : document.nodes) {
        out << "\nnode " << node.name << " " << node.type << "\n";
        for (const auto& exec : node.execs)
            out << "  exec " << (exec.pin.empty() ? "" : exec.pin + " = ") << exec.node << "." << exec.output << "\n";
        for (const auto& input : node.inputs) out << "  in " << input.pin << " = " << input.text << "\n";
    }
    // Positions last, so moving nodes in the editor doesn't touch the logic above.
    bool any = false;
    for (const auto& node : document.nodes) any = any || node.placed || !node.note.empty();
    if (!any) return;
    out << "\neditor\n";
    for (const auto& node : document.nodes)
        if (node.placed)
            out << "  " << node.name << " " << formatFloat(node.position.x) << " " << formatFloat(node.position.y)
                << "\n";
    for (const auto& node : document.nodes)
        if (!node.note.empty()) out << "  @note " << node.name << " " << quote(node.note) << "\n";
    out << "end\n";
}

void appendCircuitNodes(CircuitDocument& destination, const CircuitDocument& source, glm::vec2 offset) {
    std::set<std::string> used;
    for (const auto& node : destination.nodes) used.insert(node.name);
    std::map<std::string, std::string> rename;
    for (const auto& node : source.nodes) {
        std::string name = node.name;
        for (int i = 2; used.contains(name); ++i) name = node.name + "_" + std::to_string(i);
        used.insert(name);
        rename[node.name] = name;
    }
    std::map<std::string, std::string> variables;
    std::set<std::string> variableNames;
    for (const auto& variable : destination.variables) variableNames.insert(variable.name);
    for (const auto& variable : source.variables) {
        const auto found = std::find_if(destination.variables.begin(), destination.variables.end(), [&](const auto& v) {
            return v.name == variable.name;
        });
        if (found != destination.variables.end() && found->type == variable.type) {
            variables[variable.name] = variable.name;
            continue;
        }
        auto copy = variable;
        for (int i = 2; variableNames.contains(copy.name); ++i) copy.name = variable.name + "_" + std::to_string(i);
        variableNames.insert(copy.name);
        variables[variable.name] = copy.name;
        destination.variables.push_back(std::move(copy));
    }
    for (const auto& node : source.nodes) {
        auto copy = node;
        copy.name = rename.at(node.name);
        if ((copy.type.starts_with("Get.") || copy.type.starts_with("Set.")) && variables.contains(copy.type.substr(4)))
            copy.type = copy.type.substr(0, 4) + variables.at(copy.type.substr(4));
        copy.position += offset;
        copy.placed = true;
        for (auto& wire : copy.execs)
            if (rename.contains(wire.node)) wire.node = rename.at(wire.node);
        for (auto& input : copy.inputs) {
            if (variables.contains(input.text)) input.text = variables.at(input.text);
            const size_t dot = input.text.find('.');
            if (dot != std::string::npos && rename.contains(input.text.substr(0, dot)))
                input.text = rename.at(input.text.substr(0, dot)) + input.text.substr(dot);
        }
        destination.nodes.push_back(std::move(copy));
    }
}

}   // namespace shockwave
