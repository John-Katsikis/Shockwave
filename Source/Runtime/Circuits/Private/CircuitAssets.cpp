// What a circuit refers to, for the asset graph (S04, docs/AssetResidency.md): literal
// values given to pins that take an asset, read with the circuit parser.
#include <Circuits/Circuit.h>
#include <Circuits/CircuitNodes.h>

#include <Engine/AssetReferences.h>
#include <Engine/Components.h>

#include <algorithm>
#include <optional>
#include <string>

namespace shockwave {

namespace {

// String pins of built-in nodes that name a Content file.
struct StringPin {
    const char* node;
    const char* pin;
    AssetKind kind;
};
constexpr StringPin kStringPins[] = {
    {"Entity.Spawn", "prefab", AssetKind::Prefab},
    {"Entity.SpawnAt", "prefab", AssetKind::Prefab},
    {"Audio.PlaySound", "clip", AssetKind::Audio},
    {"Audio.PlaySoundAt", "clip", AssetKind::Audio},
    {"Audio.PlaySoundOn", "clip", AssetKind::Audio},
    // UI nodes take a UIDocument entity's name or a document's file: only a file counts.
    {"UI.Show", "document", AssetKind::UIDocument},
    {"UI.Hide", "document", AssetKind::UIDocument},
};

const ComponentInfo* componentById(uint32_t id) {
    for (const ComponentInfo& info : registeredComponents())
        if (info.id == id) return &info;
    return nullptr;
}

}   // namespace

void registerCircuitAssetProvider() {
    registerAssetPathField("Circuit", "circuit", AssetKind::Circuit);
    registerAssetProvider("circuit", {".swcircuit"}, [](AssetReader& r, std::string& error) {
        CircuitDocument document;
        if (!parseCircuitText(r.text(), r.id(), document, error)) return false;
        auto literal = [](const std::string& text, PinType type, std::string& out) {
            Value value;
            std::string why;
            if (!parseValue(text, type, value, why)) return false;
            out = value.text;
            return !out.empty();
        };
        auto assetPin = [&](PinType type, const std::string& text, const std::string& via, int line) {
            std::string name;
            if (!literal(text, type, name)) return;
            if (type == PinType::Mesh) r.named(name, AssetKind::Builtin, via, line);
            if (type == PinType::Texture) r.named(name, AssetKind::Texture, via, line);
            if (type == PinType::Model) r.named(name, AssetKind::Model, via, line);
        };
        for (const CircuitDocument::Variable& v : document.variables)
            assetPin(v.type, v.value, "variable " + v.name, v.line);
        for (const CircuitDocument::Node& node : document.nodes) {
            const auto desc = findNode(node.type);
            if (!desc) continue;   // the compiler reports unknown nodes
            const ComponentInfo* component = desc->component ? componentById(desc->component) : nullptr;
            for (const CircuitDocument::Input& input : node.inputs) {
                const auto pin = std::find_if(
                    desc->inputs.begin(), desc->inputs.end(), [&](const PinDesc& p) { return p.name == input.pin; });
                if (pin == desc->inputs.end()) continue;
                const std::string via = "node " + node.name + " (" + node.type + "), input " + input.pin;
                if (pin->type != PinType::String) {
                    assetPin(pin->type, input.text, via, input.line);
                    continue;
                }
                std::optional<AssetKind> kind;
                for (const StringPin& s : kStringPins)
                    if (node.type == s.node && input.pin == s.pin) kind = s.kind;
                if (component) {   // a reflected Component.<Name> node
                    if (const auto field = assetPathField(component->name, input.pin)) kind = field->kind;
                    for (const Field& f : component->fields)
                        if (f.name == input.pin && f.type == FieldType::AudioClip)
                            kind = AssetKind::Audio;
                        else if (f.name == input.pin && f.type == FieldType::UIDocument)
                            kind = AssetKind::UIDocument;
                }
                std::string name;
                if (!kind || !literal(input.text, PinType::String, name)) continue;
                if (*kind == AssetKind::UIDocument && assetKindOf(name) != AssetKind::UIDocument) continue;
                r.content(name, via, input.line, *kind);
            }
        }
        return true;
    });
}

}   // namespace shockwave
