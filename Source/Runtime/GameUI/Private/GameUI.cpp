// GameUI: one RmlUi context showing a world's UIDocuments, fed by Shockwave's Input and
// drawn through Renderer::drawUi. RmlUi's process-wide side is in RmlRuntime.cpp.
#include <GameUI/GameUI.h>

#include "RmlRuntime.h"
#include "UIEffects.h"
#include "UIText.h"

#include <Assets/Image.h>
#include <Core/Profile.h>
#include <Engine/Components.h>
#include <Engine/Engine.h>
#include <Input/Input.h>
#include <Renderer/Renderer.h>

#include <glm/gtc/type_ptr.hpp>
#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <variant>

namespace shockwave {

namespace fs = std::filesystem;

void registerGameUIComponents() {
    auto document = field("document", &UIDocument::document);
    document.type = FieldType::UIDocument;
    registerComponent<UIDocument>(
        "UIDocument",
        {document, field("visible", &UIDocument::visible), field("order", &UIDocument::order, 1.0f, -1000, 1000),
         field("focus_on_show", &UIDocument::focusOnShow), field("preview", &UIDocument::preview)},
        "UI");
}

namespace {

using Clock = std::chrono::steady_clock;

float msSince(Clock::time_point start) {
    return std::chrono::duration<float, std::milli>(Clock::now() - start).count();
}

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<const Engine*, GameUI*>& registry() {
    static std::unordered_map<const Engine*, GameUI*> r;
    return r;
}

// Input::events codes are USB HID usages, which SDL scancodes are too.
Rml::Input::KeyIdentifier rmlKey(uint16_t code) {
    using namespace Rml::Input;
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z)
        return static_cast<KeyIdentifier>(KI_A + (code - SDL_SCANCODE_A));
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9)
        return static_cast<KeyIdentifier>(KI_1 + (code - SDL_SCANCODE_1));
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F12)
        return static_cast<KeyIdentifier>(KI_F1 + (code - SDL_SCANCODE_F1));
    if (code >= SDL_SCANCODE_KP_1 && code <= SDL_SCANCODE_KP_9)
        return static_cast<KeyIdentifier>(KI_NUMPAD1 + (code - SDL_SCANCODE_KP_1));
    switch (code) {
    case SDL_SCANCODE_0: return KI_0;
    case SDL_SCANCODE_KP_0: return KI_NUMPAD0;
    case SDL_SCANCODE_RETURN: return KI_RETURN;
    case SDL_SCANCODE_KP_ENTER: return KI_NUMPADENTER;
    case SDL_SCANCODE_ESCAPE: return KI_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return KI_BACK;
    case SDL_SCANCODE_TAB: return KI_TAB;
    case SDL_SCANCODE_SPACE: return KI_SPACE;
    case SDL_SCANCODE_MINUS: return KI_OEM_MINUS;
    case SDL_SCANCODE_EQUALS: return KI_OEM_PLUS;
    case SDL_SCANCODE_LEFTBRACKET: return KI_OEM_4;
    case SDL_SCANCODE_RIGHTBRACKET: return KI_OEM_6;
    case SDL_SCANCODE_BACKSLASH: return KI_OEM_5;
    case SDL_SCANCODE_SEMICOLON: return KI_OEM_1;
    case SDL_SCANCODE_APOSTROPHE: return KI_OEM_7;
    case SDL_SCANCODE_GRAVE: return KI_OEM_3;
    case SDL_SCANCODE_COMMA: return KI_OEM_COMMA;
    case SDL_SCANCODE_PERIOD: return KI_OEM_PERIOD;
    case SDL_SCANCODE_SLASH: return KI_OEM_2;
    case SDL_SCANCODE_CAPSLOCK: return KI_CAPITAL;
    case SDL_SCANCODE_PAUSE: return KI_PAUSE;
    case SDL_SCANCODE_INSERT: return KI_INSERT;
    case SDL_SCANCODE_HOME: return KI_HOME;
    case SDL_SCANCODE_PAGEUP: return KI_PRIOR;
    case SDL_SCANCODE_DELETE: return KI_DELETE;
    case SDL_SCANCODE_END: return KI_END;
    case SDL_SCANCODE_PAGEDOWN: return KI_NEXT;
    case SDL_SCANCODE_RIGHT: return KI_RIGHT;
    case SDL_SCANCODE_LEFT: return KI_LEFT;
    case SDL_SCANCODE_DOWN: return KI_DOWN;
    case SDL_SCANCODE_UP: return KI_UP;
    case SDL_SCANCODE_NUMLOCKCLEAR: return KI_NUMLOCK;
    case SDL_SCANCODE_KP_DIVIDE: return KI_DIVIDE;
    case SDL_SCANCODE_KP_MULTIPLY: return KI_MULTIPLY;
    case SDL_SCANCODE_KP_MINUS: return KI_SUBTRACT;
    case SDL_SCANCODE_KP_PLUS: return KI_ADD;
    case SDL_SCANCODE_KP_PERIOD: return KI_DECIMAL;
    case SDL_SCANCODE_LCTRL: return KI_LCONTROL;
    case SDL_SCANCODE_RCTRL: return KI_RCONTROL;
    case SDL_SCANCODE_LSHIFT: return KI_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return KI_RSHIFT;
    case SDL_SCANCODE_LALT: return KI_LMENU;
    case SDL_SCANCODE_RALT: return KI_RMENU;
    case SDL_SCANCODE_LGUI: return KI_LMETA;
    case SDL_SCANCODE_RGUI: return KI_RMETA;
    default: return KI_UNKNOWN;
    }
}

int rmlModifiers(uint16_t m) {
    int out = 0;
    if (m & InputEvent::Ctrl) out |= Rml::Input::KM_CTRL;
    if (m & InputEvent::Shift) out |= Rml::Input::KM_SHIFT;
    if (m & InputEvent::Alt) out |= Rml::Input::KM_ALT;
    if (m & InputEvent::Super) out |= Rml::Input::KM_META;
    if (m & InputEvent::CapsLock) out |= Rml::Input::KM_CAPSLOCK;
    if (m & InputEvent::NumLock) out |= Rml::Input::KM_NUMLOCK;
    return out;
}

// SDL: 1 left, 2 middle, 3 right. RmlUi: 0 left, 1 right, 2 middle.
int rmlButton(uint16_t sdlButton) {
    switch (sdlButton) {
    case 1: return 0;
    case 2: return 2;
    case 3: return 1;
    default: return sdlButton - 1;
    }
}

uint32_t packColor(const Rml::ColourbPremultiplied& c) {
    return uint32_t(c.red) | uint32_t(c.green) << 8 | uint32_t(c.blue) << 16 | uint32_t(c.alpha) << 24;
}

// The event types handed to game code (UIWorld::onEvent). Listened for in the capture
// phase, so ones that don't bubble (focus, blur) arrive too.
constexpr Rml::EventId kForwardedEvents[] = {
    Rml::EventId::Click,     Rml::EventId::Dblclick, Rml::EventId::Change, Rml::EventId::Submit,
    Rml::EventId::Mouseover, Rml::EventId::Mouseout, Rml::EventId::Focus,  Rml::EventId::Blur,
};

// A key the UI handles itself when nothing that edits text has focus: navigation is
// driven by the ui_* actions instead, so rebinding them works and keys aren't doubled.
bool isNavigationKey(uint16_t code) {
    switch (code) {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return true;
    default: return false;
    }
}

bool isTextField(Rml::Element* e) {
    if (!e) return false;
    if (e->GetTagName() == "textarea") return true;
    if (e->GetTagName() != "input") return false;
    const Rml::String type = e->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password" || type == "number" || type == "search" || type == "email";
}

// ---- The "game" data model ----
// Every name a document refers to is bound before the document loads, so values game
// code sets later show up, and unset ones show nothing (or a preview).

const UIValue& unsetValue() {
    static const UIValue unset;
    return unset;
}

void toVariant(const UIValue& v, Rml::Variant& out) {
    switch (v.kind()) {
    case UIValue::Kind::Number: out = v.number(); break;
    case UIValue::Kind::Bool: out = v.flag(); break;
    case UIValue::Kind::Text: out = Rml::String(v.text()); break;
    case UIValue::Kind::Unset: out = Rml::String(); break;
    case UIValue::Kind::List:
    case UIValue::Kind::Record: out = Rml::String(v.toText()); break;
    }
}

UIValue fromVariant(const Rml::Variant& in) {
    switch (in.GetType()) {
    case Rml::Variant::BOOL: return UIValue(in.Get<bool>());
    case Rml::Variant::STRING: return UIValue(in.Get<Rml::String>());
    default: return UIValue(in.Get<double>());
    }
}

// A value inside a list or record ({{item.name}}, items[2]).
class ValueDefinition final : public Rml::VariableDefinition {
public:
    ValueDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Scalar) {}
    static ValueDefinition& instance() {
        static ValueDefinition definition;
        return definition;
    }
    bool Get(void* ptr, Rml::Variant& out) override {
        toVariant(*static_cast<const UIValue*>(ptr), out);
        return true;
    }
    bool Set(void* ptr, const Rml::Variant& in) override {
        if (ptr == &unsetValue()) return false;
        *static_cast<UIValue*>(ptr) = fromVariant(in);
        return true;
    }
    int Size(void* ptr) override { return sizeOf(*static_cast<const UIValue*>(ptr)); }
    Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override {
        return childOf(*static_cast<const UIValue*>(ptr), address);
    }
    Rml::StringList ReflectMemberNames() override { return {}; }

    static int sizeOf(const UIValue& v) { return v.isList() ? static_cast<int>(v.size()) : 0; }
    static Rml::DataVariable childOf(const UIValue& v, const Rml::DataAddressEntry& address) {
        const UIValue* child = &unsetValue();
        if (address.name.empty()) {
            if (v.isList() && address.index >= 0 && size_t(address.index) < v.size()) child = &v[size_t(address.index)];
        } else if (address.name == "size" && v.isList()) {
            return Rml::MakeLiteralIntVariable(static_cast<int>(v.size()));
        } else if (const UIValue* field = v.field(address.name)) {
            child = field;
        }
        return Rml::DataVariable(&instance(), const_cast<UIValue*>(child));
    }
};

// A top-level name: resolves to game code's value, a preview, or nothing.
struct ValueSlot {
    std::string name;
    UIValue placeholder;   // "{{name}}", shown while previewing
    std::function<const UIValue&(const ValueSlot&)> resolve;
    std::function<void(const ValueSlot&, UIValue)> assign;   // data-value writing back
};
class SlotDefinition final : public Rml::VariableDefinition {
public:
    SlotDefinition() : Rml::VariableDefinition(Rml::DataVariableType::Scalar) {}
    static SlotDefinition& instance() {
        static SlotDefinition definition;
        return definition;
    }
    bool Get(void* ptr, Rml::Variant& out) override;
    bool Set(void* ptr, const Rml::Variant& in) override;
    int Size(void* ptr) override;
    Rml::DataVariable Child(void* ptr, const Rml::DataAddressEntry& address) override;
};

}   // namespace

// ---- Impl ---------------------------------------------------------------------------------

struct GameUI::Impl final : ui::ContextOwner, Rml::RenderInterface, Rml::EventListener {
    struct Document {
        Entity entity;
        std::string path;   // as the UIDocument has it
        std::string name;   // the entity's Name, or the path
        Rml::ElementDocument* doc = nullptr;
        bool shown = false;
        int order = 0;
        bool focusOnShow = false;
        bool tried = false;   // a load was attempted (and doc is null if it failed)
        std::vector<UIProblem> problems;
        std::vector<std::pair<fs::path, fs::file_time_type>> files;   // what it read, for hot reload
    };
    struct Geometry {
        std::vector<Rml::Vertex> vertices;
        std::vector<int> indices;
    };
    struct Texture {
        TextureHandle gpu;
        bool live = false;
    };
    // A UI event, copied so game code runs after RmlUi is done processing input.
    struct QueuedEvent {
        std::string document, type, command, value;
        std::vector<std::string> ids;
        Entity entity;
    };
    using Slot = ValueSlot;
    struct GradientDraw {   // a gradient decorator's geometry, baked (see UIEffects.h)
        Rml::TextureHandle texture = 0;
        std::vector<UiVertex> vertices;   // untranslated, with the baked texture's coordinates
        std::vector<uint32_t> indices;
    };

    GameUI& owner;
    Engine* engine;
    Rml::Context* context = nullptr;
    std::string contextName;
    fs::path content;
    std::vector<Document> documents;
    std::vector<UIProblem> noProblems;
    glm::uvec2 size{1280, 720};
    float dpRatio = 1.0f;
    bool inputEnabled = false;
    bool pointerOverridden = false;
    std::optional<glm::vec2> pointer;
    bool pointerInside = false;
    double pollTimer = 0.0;
    bool orderDirty = true;
    std::set<fs::path> contentFonts;

    // Data model "game"
    Rml::DataModelConstructor model;
    Rml::DataModelHandle modelHandle;
    std::map<std::string, UIValue, std::less<>> values;   // set by game code
    std::map<std::string, Slot, std::less<>> slots;   // bound names
    bool previewOn = false;
    std::map<std::string, UIValue, std::less<>> previewValues;
    std::string previewSource;   // every UIDocument's preview text, to notice changes

    // Menu navigation
    float navHeld[4] = {};   // seconds each direction has been held, for repeats
    std::function<void(const UIEvent&)> listener;
    std::vector<QueuedEvent> queued;

    // Rendering
    std::vector<Geometry> geometry;
    std::vector<uint32_t> freeGeometry;
    std::vector<Texture> textures;
    std::vector<uint32_t> freeTextures;
    std::vector<TextureHandle> retired;   // released by RmlUi, destroyed in batches
    std::map<fs::path, std::string> imageSources;   // image file -> the name RmlUi caches it under
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<UiDrawCommand> commands;
    std::vector<glm::mat4> transforms;
    glm::ivec4 scissor{0, 0, -1, -1};
    uint32_t transform = kNoUiTransform;
    bool discard = false;   // validate(): lay out and draw, but keep nothing

    // Off-screen layers (box-shadow), filters and gradients; see UIEffects.h.
    std::vector<std::vector<ui::LayerOp>> layers;   // pushed layers; handle = index + 1
    std::map<uintptr_t, ui::LayerFilter> filters;
    std::map<uintptr_t, std::unique_ptr<ui::Gradient>> shaders;
    uintptr_t nextHandle = 1;
    std::map<std::pair<uintptr_t, uintptr_t>, GradientDraw> gradients;   // (shader, geometry)
    UIStats stats;

    Impl(GameUI& o, Engine* e) : owner(o), engine(e) {
        ui::acquire();
        static int counter = 0;
        contextName = "shockwave-ui-" + std::to_string(++counter);
        context = Rml::CreateContext(contextName, Rml::Vector2i(int(size.x), int(size.y)), this);
        if (!context) throw std::runtime_error("RmlUi couldn't create a context");
        ui::registerContext(context, this);
        createModel();
        if (const std::string& error = ui::defaultFontsError(); !error.empty())
            SW_LOG(LogUI, Error, "Game UI: {}", error);
    }

    ~Impl() override {
        closeAll();
        ui::unregisterContext(context);
        Rml::RemoveContext(contextName);
        context = nullptr;
        // RmlUi keeps a render manager per render interface; this one goes away with us.
        if (ui::users() == 1) {
            ui::release();   // shuts RmlUi down, releasing our textures and geometry
        } else {
            Rml::ReleaseTextures(this);
            Rml::ReleaseCompiledGeometry(this);
            Rml::ReleaseRenderManagers();
            ui::release();
        }
        if (engine) ui::setTextInput(nullptr);
        flushRetired(true);
    }

    void createModel() {
        model = context->CreateDataModel("game", nullptr, true);
        modelHandle = model.GetModelHandle();
    }

    void bind(std::string_view name) {
        if (!model || name.empty() || slots.find(name) != slots.end()) return;
        Slot& slot = slots[std::string(name)];   // map nodes don't move, so RmlUi can keep a pointer
        slot.name = std::string(name);
        slot.resolve = [this](const Slot& s) -> const UIValue& { return resolve(s); };
        slot.assign = [this](const Slot& s, UIValue v) { values[s.name] = std::move(v); };
        slot.placeholder = UIValue("{{" + slot.name + "}}");
        model.BindCustomDataVariable(slot.name, Rml::DataVariable(&SlotDefinition::instance(), &slot));
    }

    const UIValue& resolve(const Slot& slot) const {
        if (const auto it = values.find(slot.name); it != values.end() && it->second.kind() != UIValue::Kind::Unset)
            return it->second;
        if (!previewOn) return unsetValue();
        if (const auto it = previewValues.find(slot.name); it != previewValues.end()) return it->second;
        return slot.placeholder;
    }

    void setValue(std::string_view name, UIValue value) {
        bind(name);
        auto it = values.find(name);
        if (it == values.end()) {
            it = values.emplace(std::string(name), std::move(value)).first;
        } else {
            if (it->second == value) return;
            it->second = std::move(value);
        }
        if (modelHandle) modelHandle.DirtyVariable(it->first);
    }

    // Binds the names a document's file refers to, before RmlUi reads it.
    void bindReferenced(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        std::ostringstream text;
        text << in.rdbuf();
        for (const std::string& name : ui::referencedNames(text.str())) bind(name);
    }

    // Collects every UIDocument's preview values (the first document to name a value
    // wins) when any of them changed. Mistakes are reported with the document.
    void syncPreview(World& world) {
        std::string source;
        world.each<const UIDocument>([&](Entity, const UIDocument& c) { source += c.preview + '\x1f'; });
        if (source == previewSource) return;
        previewSource = std::move(source);
        previewValues.clear();
        for (Document& d : documents) {
            std::erase_if(d.problems, [](const UIProblem& p) { return p.message.rfind("preview: ", 0) == 0; });
            const UIDocument* c = world.get<UIDocument>(d.entity);
            if (!c || c->preview.empty()) continue;
            std::string error;
            const auto parsed = parseUIPreview(c->preview, &error);
            if (!parsed) {
                d.problems.push_back({d.path, "preview: " + error});
                continue;
            }
            for (const auto& [name, value] : *parsed) {
                bind(name);
                previewValues.emplace(name, value);
            }
        }
        if (modelHandle) modelHandle.DirtyAllVariables();
    }

    // ---- Documents ----

    Document* find(Entity e) {
        for (Document& d : documents)
            if (d.entity == e) return &d;
        return nullptr;
    }
    const Document* find(Entity e) const { return const_cast<Impl*>(this)->find(e); }
    Document* find(const Rml::ElementDocument* doc) {
        for (Document& d : documents)
            if (d.doc == doc) return &d;
        return nullptr;
    }

    void close(Document& d) {
        if (!d.doc) return;
        for (Rml::EventId id : kForwardedEvents) d.doc->RemoveEventListener(id, this, true);
        d.doc->Close();
        d.doc = nullptr;
        d.shown = false;
    }

    void closeAll() {
        for (Document& d : documents) close(d);
        documents.clear();
        if (context) context->Update();   // closed documents are destroyed here
    }

    void load(Document& d) {
        SW_PROFILE_SCOPE("UI document load");
        close(d);
        d.tried = true;
        d.problems.clear();
        d.files.clear();
        std::set<fs::path> opened;
        ui::Capture capture{d.path, &d.problems, &opened};
        ui::setRoot(content);
        ui::setCapture(&capture);
        std::error_code ec;
        const fs::path file = ui::resolve(d.path);
        if (d.path.empty()) {
            d.problems.push_back({d.path, "no document set"});
        } else if (fs::path(d.path).extension() != ".rml") {
            d.problems.push_back({d.path, "not an .rml document"});
        } else if (!fs::is_regular_file(file, ec)) {
            d.problems.push_back({d.path, "file not found in Content"});
        } else {
            bindReferenced(file);
            d.doc = context->LoadDocument(d.path);
            if (!d.doc && d.problems.empty()) d.problems.push_back({d.path, "RmlUi couldn't load the document"});
        }
        if (d.doc) {
            makeButtonsFocusable(d.doc);
            for (Rml::EventId id : kForwardedEvents) d.doc->AddEventListener(id, this, true);
            d.doc->SetProperty(Rml::PropertyId::ZIndex, Rml::Property(d.order, Rml::Unit::NUMBER));
            // Lay it out once now, so problems that show during layout (a missing font,
            // say) are recorded for this document.
            d.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            context->Update();
            d.doc->Hide();
            d.shown = false;
        }
        ui::setCapture(nullptr);
        previewSource.clear();   // re-check this document's preview values
        // Watch the document even when it's missing, so creating it loads it.
        opened.insert(file.lexically_normal());
        for (const fs::path& path : opened) d.files.emplace_back(path, fs::last_write_time(path, ec));
        // RmlUi logs its own complaints; these are ours.
        if (!d.doc && !d.problems.empty() && opened.size() <= 1)
            SW_LOG(
                LogUI, Warning, "{}: {}", d.path.empty() ? std::string("UIDocument") : d.path,
                d.problems.front().message);
        orderDirty = true;
    }

    void applyVisibility(Document& d, bool visible) {
        if (!d.doc || visible == d.shown) return;
        if (visible) {
            d.doc->Show(Rml::ModalFlag::None, d.focusOnShow ? Rml::FocusFlag::Document : Rml::FocusFlag::None);
            if (d.focusOnShow) focusFirst(d);
            orderDirty = true;
        } else {
            d.doc->Hide();
        }
        d.shown = visible;
    }

    void sortDocuments() {
        if (!orderDirty) return;
        orderDirty = false;
        std::vector<Document*> sorted;
        for (Document& d : documents)
            if (d.doc) sorted.push_back(&d);
        std::stable_sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->order < b->order; });
        for (Document* d : sorted) d->doc->PullToFront();
    }

    // Matches documents to the world's UIDocuments: new ones load, removed ones close.
    void sync(World& world) {
        std::vector<char> seen(documents.size(), 0);
        world.each<const UIDocument>([&](Entity e, const UIDocument& c) {
            Document* d = nullptr;
            for (size_t i = 0; i < documents.size(); ++i) {
                if (documents[i].entity == e) {
                    d = &documents[i];
                    seen[i] = 1;
                    break;
                }
            }
            if (!d) {
                documents.push_back({});
                seen.push_back(1);
                d = &documents.back();
                d->entity = e;
            }
            const Name* name = world.get<Name>(e);
            d->name = name && !name->value.empty() ? name->value : c.document;
            if (d->order != c.order) {
                d->order = c.order;
                if (d->doc) d->doc->SetProperty(Rml::PropertyId::ZIndex, Rml::Property(c.order, Rml::Unit::NUMBER));
                orderDirty = true;
            }
            d->focusOnShow = c.focusOnShow;
            if (d->path != c.document || !d->tried) {
                d->path = c.document;
                load(*d);
            }
            applyVisibility(*d, c.visible);
        });
        for (size_t i = documents.size(); i-- > 0;) {
            if (seen[i]) continue;
            close(documents[i]);
            documents.erase(documents.begin() + static_cast<std::ptrdiff_t>(i));
            orderDirty = true;
        }
        sortDocuments();
        syncPreview(world);
    }

    void pollFiles(double dt) {
        pollTimer += dt;
        if (pollTimer < 0.25) return;
        pollTimer = 0.0;
        std::error_code ec;
        bool any = false;
        for (Document& d : documents) {
            bool changed = false;
            for (const auto& [path, time] : d.files)
                if (fs::last_write_time(path, ec) != time) changed = true;
            if (!changed) continue;
            // Images are cached by RmlUi under the name the document used.
            for (const auto& [path, time] : d.files)
                if (const auto it = imageSources.find(path); it != imageSources.end())
                    Rml::ReleaseTexture(it->second, this);
            if (!any) {
                Rml::Factory::ClearStyleSheetCache();
                Rml::Factory::ClearTemplateCache();
                loadContentFonts();   // a document may now use a font file that was just added
                any = true;
            }
            const bool wasShown = d.shown;
            load(d);
            applyVisibility(d, wasShown);
            SW_LOG(LogUI, Info, "Reloaded {}{}", d.path, d.problems.empty() ? "" : " (with problems)");
        }
        if (any) sortDocuments();
    }

    void loadContentFonts() {
        if (content.empty()) return;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(content, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            const fs::path& p = it->path();
            const std::string ext = p.extension().string();
            if (ext != ".ttf" && ext != ".otf") continue;
            if (!contentFonts.insert(p).second) continue;
            if (!ui::loadFont(p, false)) SW_LOG(LogUI, Warning, "Couldn't load font {}", p.string());
        }
    }

    // ---- Input ----

    void feedInput() {
        if (!engine || !inputEnabled) return;
        SW_PROFILE_SCOPE("UI input");
        Input& input = engine->input();
        ui::setTextInput(&input);
        Window& window = engine->window();
        int pointsW = 0, pointsH = 0;
        SDL_GetWindowSize(window.handle(), &pointsW, &pointsH);
        const glm::vec2 toPixels(
            pointsW > 0 ? float(window.pixelWidth()) / float(pointsW) : 1.0f,
            pointsH > 0 ? float(window.pixelHeight()) / float(pointsH) : 1.0f);
        const uint32_t mainWindow = SDL_GetWindowID(window.handle());
        if (pointerOverridden) {
            if (pointer) {
                context->ProcessMouseMove(int(std::floor(pointer->x)), int(std::floor(pointer->y)), 0);
                pointerInside = true;
            } else if (pointerInside) {
                context->ProcessMouseLeave();
                pointerInside = false;
            }
        }
        for (const InputEvent& e : input.events()) {
            const int mods = rmlModifiers(e.modifiers);
            const bool mouseHere = pointerOverridden ? pointer.has_value() : e.window == mainWindow;
            switch (e.type) {
            case InputEvent::Type::KeyDown: {
                const auto key = rmlKey(e.code);
                if (isNavigationKey(e.code) && !isTextField(context->GetFocusElement())) break;   // ui_* actions
                if (key != Rml::Input::KI_UNKNOWN) context->ProcessKeyDown(key, mods);
                break;
            }
            case InputEvent::Type::KeyUp: {
                const auto key = rmlKey(e.code);
                if (isNavigationKey(e.code) && !isTextField(context->GetFocusElement())) break;
                if (key != Rml::Input::KI_UNKNOWN) context->ProcessKeyUp(key, mods);
                break;
            }
            case InputEvent::Type::Text: context->ProcessTextInput(Rml::String(input.eventText(e))); break;
            case InputEvent::Type::MouseMove:
                if (!pointerOverridden && e.window == mainWindow) {
                    const glm::vec2 p = e.position * toPixels;
                    context->ProcessMouseMove(int(std::floor(p.x)), int(std::floor(p.y)), mods);
                }
                break;
            case InputEvent::Type::MouseDown:
                if (mouseHere) {
                    if (!pointerOverridden) {
                        const glm::vec2 p = e.position * toPixels;
                        context->ProcessMouseMove(int(std::floor(p.x)), int(std::floor(p.y)), mods);
                    }
                    context->ProcessMouseButtonDown(rmlButton(e.code), mods);
                }
                break;
            case InputEvent::Type::MouseUp:
                // Always, so a press that started on a button and ended elsewhere lets go.
                context->ProcessMouseButtonUp(rmlButton(e.code), mods);
                break;
            case InputEvent::Type::Wheel:
                if (mouseHere) context->ProcessMouseWheel(Rml::Vector2f(0.0f, -e.wheel), mods);
                break;
            }
        }
    }

    bool wantsMouse() const {
        if (!context) return false;
        const Rml::Element* hover = const_cast<Rml::Context*>(context)->GetHoverElement();
        return hover && hover != const_cast<Rml::Context*>(context)->GetRootElement();
    }

    bool wantsKeyboard() const {
        if (!context) return false;
        Rml::Element* focus = const_cast<Rml::Context*>(context)->GetFocusElement();
        return focus && focus != context->GetRootElement() && focus != focus->GetOwnerDocument();
    }

    // ---- Menu navigation ----
    // Moving focus reuses RmlUi's own spatial navigation (the nav-up/-down/-left/-right
    // properties); an element without one navigates as "auto". Confirm is RmlUi's Enter,
    // which clicks the focused button, toggles a checkbox or opens a select.

    static bool focusable(Rml::Element* e) {
        return e && e->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto && e->IsVisible(true) &&
               !e->HasAttribute("disabled");
    }

    // RmlUi's <button> is a plain element; like a browser's, ours can take focus unless
    // the stylesheet says otherwise (tab-index: none). Run again before navigating, for
    // buttons that game code or data-for added since.
    static void makeButtonsFocusable(Rml::Element* e) {
        if (!e) return;
        if (e->GetTagName() == "button" && !e->GetLocalProperty(Rml::PropertyId::TabIndex))
            e->SetProperty(
                Rml::PropertyId::TabIndex,
                Rml::Property(static_cast<int>(Rml::Style::TabIndex::Auto), Rml::Unit::KEYWORD));
        for (int i = 0; i < e->GetNumChildren(); ++i) makeButtonsFocusable(e->GetChild(i));
    }

    static Rml::Element* firstFocusable(Rml::Element* e, bool autofocusOnly) {
        if (!e) return nullptr;
        for (int i = 0; i < e->GetNumChildren(); ++i) {
            Rml::Element* child = e->GetChild(i);
            if (focusable(child) && (!autofocusOnly || child->HasAttribute("autofocus"))) return child;
            if (Rml::Element* found = firstFocusable(child, autofocusOnly)) return found;
        }
        return nullptr;
    }

    // Focuses a menu's element with the autofocus attribute, or else its first button or
    // control, showing the focus (:focus-visible).
    bool focusFirst(Document& d) {
        if (!d.doc) return false;
        makeButtonsFocusable(d.doc);
        Rml::Element* target = firstFocusable(d.doc, true);
        if (!target) target = firstFocusable(d.doc, false);
        if (!target) return false;
        target->Focus(true);
        target->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest));
        return true;
    }

    // The shown menu on top (focus_on_show), which navigation starts in when nothing has focus.
    Document* topMenu() {
        Document* best = nullptr;
        for (Document& d : documents)
            if (d.doc && d.shown && d.focusOnShow && (!best || d.order >= best->order)) best = &d;
        return best;
    }

    // The focused element and its document, when it's an element in a shown document.
    std::pair<Document*, Rml::Element*> focusedElement() {
        Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
        Document* d = focus ? find(focus->GetOwnerDocument()) : nullptr;
        if (!d || !d->shown || focus == d->doc) return {nullptr, nullptr};
        return {d, focus};
    }

    void queueBack(Document& d, Rml::Element* from) {
        if (!listener) return;
        QueuedEvent q;
        q.document = d.name;
        q.entity = d.entity;
        q.type = "back";
        for (Rml::Element* el = from; el; el = el->GetParentNode()) {
            if (!el->GetId().empty()) q.ids.push_back(el->GetId());
            if (q.command.empty()) q.command = el->GetAttribute<Rml::String>("data-back", "");
            if (el == d.doc) break;
        }
        queued.push_back(std::move(q));
    }

    bool navigate(UINav nav) {
        auto [d, focus] = focusedElement();
        if (!focus) {
            Document* menu = topMenu();
            if (!menu) return false;
            if (nav == UINav::Back) {
                queueBack(*menu, menu->doc);
                dispatch();
                return true;
            }
            return focusFirst(*menu);   // the first press shows where focus is
        }
        makeButtonsFocusable(d->doc);
        using namespace Rml::Input;
        KeyIdentifier key = KI_RETURN;
        Rml::PropertyId property = Rml::PropertyId::NavUp;
        switch (nav) {
        case UINav::Up:
            key = KI_UP;
            property = Rml::PropertyId::NavUp;
            break;
        case UINav::Down:
            key = KI_DOWN;
            property = Rml::PropertyId::NavDown;
            break;
        case UINav::Left:
            key = KI_LEFT;
            property = Rml::PropertyId::NavLeft;
            break;
        case UINav::Right:
            key = KI_RIGHT;
            property = Rml::PropertyId::NavRight;
            break;
        case UINav::Confirm: key = KI_RETURN; break;
        case UINav::Back:
            queueBack(*d, focus);
            dispatch();
            return true;
        }
        if (nav != UINav::Confirm && !focus->GetLocalProperty(property))
            focus->SetProperty(property, Rml::Property(static_cast<int>(Rml::Style::Nav::Auto), Rml::Unit::KEYWORD));
        context->ProcessKeyDown(key, 0);
        context->ProcessKeyUp(key, 0);
        dispatch();
        return true;
    }

    // Reads the ui_* actions: a press moves once, holding repeats after a moment.
    void pollNavigation(double dt) {
        if (!engine || !inputEnabled) return;
        constexpr float kRepeatDelay = 0.4f, kRepeatInterval = 0.09f;
        const Input& input = engine->input();
        Rml::Element* focus = focusedElement().second;
        const bool typing = isTextField(focus);
        const bool multiline = focus && focus->GetTagName() == "textarea";
        constexpr uint8_t pads = Gamepads | Joysticks;
        // While typing, the keyboard's arrows, Enter and Backspace edit the text instead.
        for (int i = 0; i < 4; ++i) {
            const bool sideways = i >= 2;
            const uint8_t sources = typing && (sideways || multiline) ? pads : uint8_t(AllInputSources);
            const std::string action = kUINavActions[i];
            if (input.wasActionPressed(action, sources)) {
                navigate(static_cast<UINav>(i));
                navHeld[i] = 0.0f;
            } else if (input.isActionDown(action, sources)) {
                navHeld[i] += static_cast<float>(dt);
                if (navHeld[i] >= kRepeatDelay) {
                    navigate(static_cast<UINav>(i));
                    navHeld[i] -= kRepeatInterval;
                }
            } else {
                navHeld[i] = 0.0f;
            }
        }
        const uint8_t buttons = typing ? pads : uint8_t(AllInputSources);
        if (input.wasActionPressed(kUINavActions[4], buttons)) navigate(UINav::Confirm);
        if (input.wasActionPressed(kUINavActions[5], buttons)) navigate(UINav::Back);
    }

    // ---- Events ----

    void queue(Rml::Event& event, const std::string& command) {
        Rml::Element* current = event.GetCurrentElement();
        Rml::ElementDocument* doc = current ? current->GetOwnerDocument() : nullptr;
        const Document* d = doc ? find(doc) : nullptr;
        if (!d || !listener) return;
        QueuedEvent q;
        q.document = d->name;
        q.entity = d->entity;
        q.type = event.GetType();
        q.command = command;
        q.value = event.GetParameter<Rml::String>("value", "");
        // With a command, the element is the one with the handler; otherwise the target
        // and every parent with an id, innermost first.
        for (Rml::Element* el = command.empty() ? event.GetTargetElement() : current; el; el = el->GetParentNode()) {
            if (!el->GetId().empty()) q.ids.push_back(el->GetId());
            if (el == doc || !command.empty()) break;
        }
        queued.push_back(std::move(q));
    }

    void ProcessEvent(Rml::Event& event) override { queue(event, {}); }
    void OnDetach(Rml::Element*) override {}   // owned by Impl
    void command(Rml::Event& event, const std::string& text) override { queue(event, text); }

    void dispatch() {
        if (queued.empty()) return;
        std::vector<QueuedEvent> events;
        events.swap(queued);
        for (const QueuedEvent& q : events) {
            if (!listener) break;
            std::vector<std::string_view> ids(q.ids.begin(), q.ids.end());
            UIEvent e;
            e.document = q.document;
            e.element = ids.empty() ? std::string_view() : ids.front();
            e.ids = ids;
            e.type = q.type;
            e.command = q.command;
            e.value = q.value;
            e.entity = q.entity;
            listener(e);
        }
        if (queued.empty()) {
            events.clear();
            queued.swap(events);   // keep the capacity
        }
    }

    // ---- Rml::RenderInterface ----

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> v, Rml::Span<const int> i) override {
        uint32_t slot;
        if (!freeGeometry.empty()) {
            slot = freeGeometry.back();
            freeGeometry.pop_back();
        } else {
            slot = static_cast<uint32_t>(geometry.size());
            geometry.emplace_back();
        }
        geometry[slot].vertices.assign(v.begin(), v.end());
        geometry[slot].indices.assign(i.begin(), i.end());
        return static_cast<Rml::CompiledGeometryHandle>(slot + 1);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        dropGradients([&](const auto& key) { return key.second == handle; });
        const uint32_t slot = static_cast<uint32_t>(handle - 1);
        if (slot >= geometry.size()) return;
        geometry[slot].vertices.clear();
        geometry[slot].indices.clear();
        freeGeometry.push_back(slot);
    }

    TextureHandle gpuTexture(uintptr_t texture) const {
        const uint32_t slot = static_cast<uint32_t>(texture - 1);
        return texture && slot < textures.size() ? textures[slot].gpu : TextureHandle{};
    }

    // Adds a draw to this frame's list, merged with the previous one when it can be.
    void addCommand(uint32_t first, uint32_t count, TextureHandle gpu, uint32_t xform, glm::ivec4 clip) {
        if (!commands.empty()) {
            UiDrawCommand& last = commands.back();
            if (last.texture.id == gpu.id && last.transform == xform && last.scissor == clip &&
                last.firstIndex + last.indexCount == first) {
                last.indexCount += count;
                return;
            }
        }
        commands.push_back({first, count, gpu, xform, clip});
    }

    void append(
        std::span<const UiVertex> v, glm::vec2 translation, std::span<const uint32_t> idx, TextureHandle gpu,
        uint32_t xform, glm::ivec4 clip, float opacity = 1.0f) {
        if (idx.empty()) return;
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (UiVertex vertex : v) {
            vertex.position += translation;
            if (opacity < 1.0f) {
                uint32_t c = 0;
                for (int k = 0; k < 4; ++k)
                    c |= uint32_t(float((vertex.color >> (8 * k)) & 0xff) * opacity + 0.5f) << (8 * k);
                vertex.color = c;
            }
            vertices.push_back(vertex);
        }
        const uint32_t first = static_cast<uint32_t>(indices.size());
        for (uint32_t index : idx) indices.push_back(base + index);
        addCommand(first, static_cast<uint32_t>(idx.size()), gpu, xform, clip);
    }

    // Geometry drawn into an off-screen layer is kept as a copy: RmlUi may release and
    // reuse the handle before the layer is used.
    ui::LayerOp layerDraw(const Geometry& g, Rml::Vector2f translation) const {
        ui::LayerOp op;
        op.vertices.reserve(g.vertices.size());
        for (const Rml::Vertex& v : g.vertices)
            op.vertices.push_back(
                {glm::vec2(v.position.x + translation.x, v.position.y + translation.y), packColor(v.colour),
                 glm::vec2(v.tex_coord.x, v.tex_coord.y)});
        op.indices.assign(g.indices.begin(), g.indices.end());
        op.transform = transform;
        op.scissor = scissor;
        return op;
    }

    void
    RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override {
        if (discard) return;
        const uint32_t slot = static_cast<uint32_t>(handle - 1);
        if (slot >= geometry.size()) return;
        const Geometry& g = geometry[slot];
        if (g.indices.empty()) return;
        if (!layers.empty()) {
            ui::LayerOp op = layerDraw(g, translation);
            op.texture = texture;
            layers.back().push_back(std::move(op));
            return;
        }
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (const Rml::Vertex& v : g.vertices) {
            vertices.push_back(
                {glm::vec2(v.position.x + translation.x, v.position.y + translation.y), packColor(v.colour),
                 glm::vec2(v.tex_coord.x, v.tex_coord.y)});
        }
        const uint32_t first = static_cast<uint32_t>(indices.size());
        for (int index : g.indices) indices.push_back(base + static_cast<uint32_t>(index));
        addCommand(first, static_cast<uint32_t>(g.indices.size()), gpuTexture(texture), transform, scissor);
    }

    Rml::TextureHandle addTexture(uint32_t width, uint32_t height, std::span<const uint8_t> rgba) {
        Texture t;
        t.live = true;
        if (engine && width && height) t.gpu = engine->renderer().createTexture(width, height, rgba, false);
        uint32_t slot;
        if (!freeTextures.empty()) {
            slot = freeTextures.back();
            freeTextures.pop_back();
            textures[slot] = t;
        } else {
            slot = static_cast<uint32_t>(textures.size());
            textures.push_back(t);
        }
        return static_cast<Rml::TextureHandle>(slot + 1);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        const fs::path path = ui::resolve(source);
        Image image;
        std::string error;
        if (!loadImage(path, image, error)) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Couldn't load image '%s': %s", source.c_str(), error.c_str());
            return 0;
        }
        if (ui::Capture* c = ui::capture(); c && c->opened) c->opened->insert(path.lexically_normal());
        imageSources[path.lexically_normal()] = source;
        // RmlUi draws with premultiplied alpha.
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
            const uint32_t a = image.rgba[i + 3];
            for (size_t k = 0; k < 3; ++k)
                image.rgba[i + k] = static_cast<uint8_t>((image.rgba[i + k] * a + 127) / 255);
        }
        dimensions = Rml::Vector2i(int(image.width), int(image.height));
        return addTexture(image.width, image.height, image.rgba);
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override {
        return addTexture(
            uint32_t(dimensions.x), uint32_t(dimensions.y),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(source.data()), source.size()));
    }

    void ReleaseTexture(Rml::TextureHandle handle) override {
        const uint32_t slot = static_cast<uint32_t>(handle - 1);
        if (slot >= textures.size() || !textures[slot].live) return;
        if (textures[slot].gpu.id) retired.push_back(textures[slot].gpu);
        textures[slot] = {};
        freeTextures.push_back(slot);
    }

    // Destroying a texture waits for the GPU, so released ones are kept until there are
    // enough to be worth one wait (glyph atlases are replaced only as new glyphs appear).
    void flushRetired(bool all) {
        if (retired.empty() || (!all && retired.size() < 16)) return;
        if (engine)
            for (TextureHandle t : retired) engine->renderer().destroyTexture(t);
        retired.clear();
    }

    void EnableScissorRegion(bool enable) override {
        if (!enable) scissor = {0, 0, -1, -1};
    }
    void SetScissorRegion(Rml::Rectanglei r) override { scissor = {r.Left(), r.Top(), r.Width(), r.Height()}; }

    void SetTransform(const Rml::Matrix4f* m) override {
        if (!m) {
            transform = kNoUiTransform;
            return;
        }
        transform = static_cast<uint32_t>(transforms.size());
        transforms.push_back(glm::make_mat4(m->data()));
    }

    // ---- Layers, filters and gradients (RmlUi's optional render features) ----

    Rml::LayerHandle PushLayer() override {
        layers.emplace_back();
        return static_cast<Rml::LayerHandle>(layers.size());
    }

    // Draws a layer's contents straight into the frame, without its filters (except
    // opacity): CSS filters on elements aren't drawn yet.
    void flatten(const std::vector<ui::LayerOp>& ops, float opacity) {
        for (const ui::LayerOp& op : ops) {
            if (op.kind == ui::LayerOp::Kind::Composite) {
                float inner = opacity;
                for (const ui::LayerFilter& f : op.filters)
                    if (f.kind == ui::LayerFilter::Kind::Opacity) inner *= f.value;
                flatten(op.children, inner);
            } else if (op.kind == ui::LayerOp::Kind::Draw && !op.gradient) {
                append(
                    op.vertices, glm::vec2(0.0f), op.indices, gpuTexture(op.texture), op.transform, op.scissor,
                    opacity);
            }
        }
    }

    void CompositeLayers(
        Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend,
        Rml::Span<const Rml::CompiledFilterHandle> filterHandles) override {
        if (source == 0 || source > layers.size() || destination > layers.size() || source == destination) return;
        if (destination == 0) {
            flatten(layers[source - 1], 1.0f);
            return;
        }
        ui::LayerOp op;
        op.kind = ui::LayerOp::Kind::Composite;
        op.children = layers[source - 1];
        op.replace = blend == Rml::BlendMode::Replace;
        for (Rml::CompiledFilterHandle h : filterHandles)
            if (const auto it = filters.find(h); it != filters.end()) op.filters.push_back(it->second);
        layers[destination - 1].push_back(std::move(op));
    }

    void PopLayer() override {
        if (!layers.empty()) layers.pop_back();
    }

    // box-shadow: RmlUi draws the shadow into a layer once, then keeps it as a texture.
    Rml::TextureHandle SaveLayerAsTexture() override {
        if (layers.empty() || scissor.z <= 0 || scissor.w <= 0) return 0;
        SW_PROFILE_SCOPE("UI shadow texture");
        ui::ParallelFor parallel;
        if (engine)
            parallel = [this](size_t count, const std::function<void(size_t, size_t)>& fn) {
                // A few batches per thread: enough to balance, few enough to stay cheap.
                JobSystem& jobs = engine->jobs();
                jobs.parallelFor(count, std::max<size_t>(64, count / (size_t(jobs.threadCount()) * 4)), fn);
            };
        const std::vector<uint8_t> rgba = ui::rasterizeLayer(layers.back(), scissor, parallel);
        return addTexture(uint32_t(scissor.z), uint32_t(scissor.w), rgba);
    }

    void EnableClipMask(bool enable) override {
        if (layers.empty()) return;   // on screen, clip masks aren't drawn yet
        ui::LayerOp op;
        op.kind = ui::LayerOp::Kind::ClipEnable;
        op.enable = enable;
        layers.back().push_back(std::move(op));
    }

    void RenderToClipMask(
        Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation) override {
        const uint32_t slot = static_cast<uint32_t>(handle - 1);
        if (layers.empty() || slot >= geometry.size()) return;
        ui::LayerOp op = layerDraw(geometry[slot], translation);
        op.kind = ui::LayerOp::Kind::ClipMask;
        op.clip = operation == Rml::ClipMaskOperation::Set          ? ui::LayerOp::ClipOp::Set
                  : operation == Rml::ClipMaskOperation::SetInverse ? ui::LayerOp::ClipOp::SetInverse
                                                                    : ui::LayerOp::ClipOp::Intersect;
        layers.back().push_back(std::move(op));
    }

    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override {
        ui::LayerFilter filter;
        if (name == "blur") {
            filter = {ui::LayerFilter::Kind::Blur, Rml::Get(parameters, "sigma", 0.0f)};
        } else if (name == "opacity") {
            filter = {ui::LayerFilter::Kind::Opacity, Rml::Get(parameters, "value", 1.0f)};
        } else {
            Rml::Log::Message(
                Rml::Log::LT_INFO, "The '%s' filter isn't drawn yet; the element shows without it.", name.c_str());
            return 0;
        }
        const uintptr_t handle = nextHandle++;
        filters.emplace(handle, filter);
        return static_cast<Rml::CompiledFilterHandle>(handle);
    }

    void ReleaseFilter(Rml::CompiledFilterHandle filter) override { filters.erase(filter); }

    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override {
        auto gradient = std::make_unique<ui::Gradient>();
        gradient->repeating = Rml::Get(parameters, "repeating", false);
        if (name == "linear-gradient") {
            gradient->kind = ui::Gradient::Kind::Linear;
            const Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
            const Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f));
            gradient->p = {p0.x, p0.y};
            gradient->v = glm::vec2(p1.x, p1.y) - gradient->p;
        } else if (name == "radial-gradient") {
            gradient->kind = ui::Gradient::Kind::Radial;
            const Rml::Vector2f c = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
            const Rml::Vector2f r = Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
            gradient->p = {c.x, c.y};
            gradient->v = glm::vec2(1.0f) / glm::max(glm::vec2(r.x, r.y), glm::vec2(1e-4f));
        } else if (name == "conic-gradient") {
            gradient->kind = ui::Gradient::Kind::Conic;
            const Rml::Vector2f c = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
            const float angle = Rml::Get(parameters, "angle", 0.0f);
            gradient->p = {c.x, c.y};
            gradient->v = {std::cos(angle), std::sin(angle)};
        } else {
            Rml::Log::Message(Rml::Log::LT_WARNING, "The '%s' shader isn't supported.", name.c_str());
            return 0;
        }
        if (const auto it = parameters.find("color_stop_list");
            it != parameters.end() && it->second.GetType() == Rml::Variant::COLORSTOPLIST) {
            for (const Rml::ColorStop& stop : it->second.GetReference<Rml::ColorStopList>()) {
                gradient->positions.push_back(stop.position.number);
                gradient->colors.push_back(
                    glm::vec4(stop.color.red, stop.color.green, stop.color.blue, stop.color.alpha) / 255.0f);
            }
        }
        const uintptr_t handle = nextHandle++;
        shaders.emplace(handle, std::move(gradient));
        return static_cast<Rml::CompiledShaderHandle>(handle);
    }

    template <class Pred>
    void dropGradients(Pred&& pred) {
        for (auto it = gradients.begin(); it != gradients.end();) {
            if (pred(it->first)) {
                ReleaseTexture(it->second.texture);
                it = gradients.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ReleaseShader(Rml::CompiledShaderHandle shader) override {
        dropGradients([&](const auto& key) { return key.first == shader; });
        shaders.erase(shader);
    }

    void RenderShader(
        Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
        Rml::TextureHandle) override {
        if (discard) return;
        const auto found = shaders.find(shader);
        const uint32_t slot = static_cast<uint32_t>(handle - 1);
        if (found == shaders.end() || slot >= geometry.size() || geometry[slot].indices.empty()) return;
        const Geometry& g = geometry[slot];
        if (!layers.empty()) {
            ui::LayerOp op = layerDraw(g, translation);
            op.gradient = found->second.get();
            for (const Rml::Vertex& v : g.vertices) op.gradientPoints.push_back({v.tex_coord.x, v.tex_coord.y});
            layers.back().push_back(std::move(op));
            return;
        }
        // Baked once per decorator; drawn every frame like any other UI geometry.
        auto [it, added] = gradients.try_emplace({shader, handle});
        GradientDraw& draw = it->second;
        if (added) {
            SW_PROFILE_SCOPE("UI gradient bake");
            std::vector<glm::vec2> points;
            points.reserve(g.vertices.size());
            for (const Rml::Vertex& v : g.vertices) points.push_back({v.tex_coord.x, v.tex_coord.y});
            const ui::BakedGradient baked = ui::bakeGradient(*found->second, points);
            draw.texture = addTexture(baked.width, baked.height, baked.rgba);
            for (size_t i = 0; i < g.vertices.size(); ++i) {
                const Rml::Vertex& v = g.vertices[i];
                draw.vertices.push_back(
                    {glm::vec2(v.position.x, v.position.y), packColor(v.colour),
                     i < baked.uvs.size() ? baked.uvs[i] : glm::vec2(0.5f)});
            }
            draw.indices.assign(g.indices.begin(), g.indices.end());
        }
        append(
            draw.vertices, glm::vec2(translation.x, translation.y), draw.indices, gpuTexture(draw.texture), transform,
            scissor);
    }

    // ---- Frame ----

    void resize() {
        glm::uvec2 want = size;
        float ratio = 1.0f;
        if (engine) {
            want = engine->renderer().uiOutputSize();
            const float scale = SDL_GetWindowDisplayScale(engine->window().handle());
            if (scale > 0.0f && std::isfinite(scale)) ratio = scale;
        }
        want = glm::max(want, glm::uvec2(1));
        if (want != size || !context->GetDimensions().x) {
            size = want;
            context->SetDimensions(Rml::Vector2i(int(size.x), int(size.y)));
        }
        if (ratio != dpRatio) {
            dpRatio = ratio;
            context->SetDensityIndependentPixelRatio(ratio);
        }
    }

    UiDrawList layout() {
        const auto start = Clock::now();
        vertices.clear();
        indices.clear();
        commands.clear();
        transforms.clear();
        scissor = {0, 0, -1, -1};
        transform = kNoUiTransform;
        ui::setRoot(content);
        {
            SW_PROFILE_SCOPE("UI layout");
            context->Update();
        }
        {
            SW_PROFILE_SCOPE("UI draw list");
            context->Render();
        }
        stats.layoutMs = msSince(start);
        stats.drawCommands = static_cast<uint32_t>(commands.size());
        stats.vertices = static_cast<uint32_t>(vertices.size());
        stats.textures = static_cast<uint32_t>(textures.size() - freeTextures.size());
        UiDrawList list;
        list.vertices = vertices;
        list.indices = indices;
        list.commands = commands;
        list.transforms = transforms;
        list.size = size;
        return list;
    }

    Rml::Element* element(Entity e, std::string_view id) {
        Document* d = find(e);
        if (!d || !d->doc) return nullptr;
        return d->doc->GetElementById(Rml::String(id));
    }
};

namespace {

const UIValue& slotValue(void* ptr) {
    const auto* slot = static_cast<const ValueSlot*>(ptr);
    return slot->resolve(*slot);
}

bool SlotDefinition::Get(void* ptr, Rml::Variant& out) {
    toVariant(slotValue(ptr), out);
    return true;
}

// data-value on a form control writes back into game code's values.
bool SlotDefinition::Set(void* ptr, const Rml::Variant& in) {
    const auto* slot = static_cast<const ValueSlot*>(ptr);
    slot->assign(*slot, fromVariant(in));
    return true;
}

int SlotDefinition::Size(void* ptr) { return ValueDefinition::sizeOf(slotValue(ptr)); }

Rml::DataVariable SlotDefinition::Child(void* ptr, const Rml::DataAddressEntry& address) {
    return ValueDefinition::childOf(slotValue(ptr), address);
}

}   // namespace

// ---- GameUI -------------------------------------------------------------------------------

GameUI::GameUI(Engine* engine) : m(std::make_unique<Impl>(*this, engine)) {
    if (engine) {
        bindDefaultControls(engine->input());
        std::lock_guard lock(registryMutex());
        registry()[engine] = this;
    }
}

GameUI::~GameUI() {
    if (m->engine) {
        std::lock_guard lock(registryMutex());
        if (auto it = registry().find(m->engine); it != registry().end() && it->second == this) registry().erase(it);
    }
}

GameUI* GameUI::forEngine(const Engine* engine) {
    if (!engine) return nullptr;
    std::lock_guard lock(registryMutex());
    const auto it = registry().find(engine);
    return it == registry().end() ? nullptr : it->second;
}

void GameUI::setContentFolder(const fs::path& folder) {
    const fs::path normal = folder.lexically_normal();
    if (normal == m->content) return;
    m->closeAll();
    m->content = normal;
    ui::setRoot(normal);
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    m->loadContentFonts();
}

const fs::path& GameUI::contentFolder() const { return m->content; }

void GameUI::setSize(glm::uvec2 size) {
    if (!m->engine) m->size = glm::max(size, glm::uvec2(1));
    m->context->SetDimensions(Rml::Vector2i(int(m->size.x), int(m->size.y)));
}

glm::uvec2 GameUI::size() const { return m->size; }

void GameUI::update(World& world, double frameDt) {
    SW_PROFILE_SCOPE("Game UI update");
    const auto start = Clock::now();
    ui::setRoot(m->content);
    m->flushRetired(false);
    m->resize();
    m->pollFiles(frameDt);
    m->sync(world);
    m->feedInput();
    m->pollNavigation(frameDt);
    m->dispatch();
    m->stats.documents = 0;
    m->stats.visible = 0;
    for (const auto& d : m->documents) {
        m->stats.documents += d.doc != nullptr;
        m->stats.visible += d.shown;
    }
    m->stats.updateMs = msSince(start);
}

UiDrawList GameUI::layout() { return m->layout(); }

void GameUI::render(Renderer& renderer) {
    SW_PROFILE_SCOPE("Game UI render");
    const UiDrawList list = m->layout();
    renderer.drawUi(list);
}

void GameUI::setInputEnabled(bool enabled) {
    if (enabled == m->inputEnabled) return;
    m->inputEnabled = enabled;
    if (!enabled) {
        if (m->pointerInside || !m->pointerOverridden) m->context->ProcessMouseLeave();
        m->pointerInside = false;
        if (Rml::Element* focus = m->context->GetFocusElement()) focus->Blur();
        if (m->engine) m->engine->input().setTextInput(false);
    }
}

void GameUI::setPointer(std::optional<glm::vec2> pixels) {
    m->pointerOverridden = true;
    m->pointer = pixels;
}

void GameUI::clearPointerOverride() {
    m->pointerOverridden = false;
    m->pointer.reset();
}

bool GameUI::wantsMouse() const { return m->inputEnabled && m->wantsMouse(); }
bool GameUI::wantsKeyboard() const { return m->inputEnabled && m->wantsKeyboard(); }

bool GameUI::navigate(UINav nav) { return m->navigate(nav); }

std::optional<std::pair<Entity, std::string>> GameUI::focused() const {
    const auto [d, focus] = m->focusedElement();
    if (!focus) return std::nullopt;
    return std::pair<Entity, std::string>(d->entity, focus->GetId());
}

void GameUI::bindDefaultControls(Input& input) {
    // Only actions nothing binds yet, so a game's own built-in bindings stay.
    for (const char* action : kUINavActions)
        if (input.actions().count(action)) return;
    using R = InputBinding::Range;
    input.bindAction("ui_up", Key::Up);
    input.bindAction("ui_down", Key::Down);
    input.bindAction("ui_left", Key::Left);
    input.bindAction("ui_right", Key::Right);
    input.bindAction("ui_confirm", Key::Enter);
    input.bindAction("ui_back", Key::Escape);
    input.bindAction("ui_back", Key::Backspace);
    input.bindAction("ui_up", GamepadButton::DpadUp);
    input.bindAction("ui_down", GamepadButton::DpadDown);
    input.bindAction("ui_left", GamepadButton::DpadLeft);
    input.bindAction("ui_right", GamepadButton::DpadRight);
    input.bindAction("ui_up", GamepadAxis::LeftY, R::Positive);
    input.bindAction("ui_down", GamepadAxis::LeftY, R::Negative);
    input.bindAction("ui_left", GamepadAxis::LeftX, R::Negative);
    input.bindAction("ui_right", GamepadAxis::LeftX, R::Positive);
    input.bindAction("ui_confirm", GamepadButton::South);
    input.bindAction("ui_back", GamepadButton::East);
}

void GameUI::reloadAll() {
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    m->loadContentFonts();
    for (auto& d : m->documents) {
        const bool shown = d.shown;
        m->load(d);
        m->applyVisibility(d, shown);
    }
    m->flushRetired(true);
    m->sortDocuments();
}

const std::vector<UIProblem>& GameUI::problems(Entity documentEntity) const {
    const Impl::Document* d = m->find(documentEntity);
    return d ? d->problems : m->noProblems;
}

std::vector<UIProblem> GameUI::validate(std::string_view document) {
    std::vector<UIProblem> problems;
    std::set<fs::path> opened;
    ui::Capture capture{std::string(document), &problems, &opened};
    ui::setRoot(m->content);
    const fs::path file = ui::resolve(std::string(document));
    std::error_code ec;
    if (document.empty()) return {{std::string(document), "no document set"}};
    if (fs::path(document).extension() != ".rml") return {{std::string(document), "not an .rml document"}};
    if (!fs::is_regular_file(file, ec)) return {{std::string(document), "file not found in Content"}};
    // Its own context, so nothing the game shows changes; the same render interface, so
    // fonts and images are shared.
    static int counter = 0;
    const std::string name = m->contextName + "-check-" + std::to_string(++counter);
    Rml::Context* check = Rml::CreateContext(name, Rml::Vector2i(int(m->size.x), int(m->size.y)), m.get());
    if (!check) return {{std::string(document), "RmlUi couldn't create a context"}};
    {
        // Every name it refers to exists, unset, as it would in the game.
        Rml::DataModelConstructor model = check->CreateDataModel("game", nullptr, true);
        static UIValue unset;
        for (const std::string& name : referencedValues(document))
            model.BindCustomDataVariable(name, Rml::DataVariable(&ValueDefinition::instance(), &unset));
    }
    ui::setCapture(&capture);
    m->discard = true;
    if (Rml::ElementDocument* doc = check->LoadDocument(Rml::String(document))) {
        doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        check->Update();
        check->Render();
        // nav-up: #id and friends must name an element in this document.
        std::vector<Rml::Element*> stack{doc};
        while (!stack.empty()) {
            Rml::Element* el = stack.back();
            stack.pop_back();
            for (int i = 0; i < el->GetNumChildren(); ++i) stack.push_back(el->GetChild(i));
            for (const auto& [id, name] :
                 {std::pair{Rml::PropertyId::NavUp, "nav-up"}, std::pair{Rml::PropertyId::NavDown, "nav-down"},
                  std::pair{Rml::PropertyId::NavLeft, "nav-left"}, std::pair{Rml::PropertyId::NavRight, "nav-right"}}) {
                const Rml::Property* p = el->GetLocalProperty(id);
                if (!p || p->unit != Rml::Unit::STRING) continue;
                const Rml::String target = p->Get<Rml::String>();
                const Rml::String wanted = target.size() > 1 && target[0] == '#' ? target.substr(1) : Rml::String();
                if (wanted.empty() || !doc->GetElementById(wanted))
                    problems.push_back(
                        {std::string(document), std::string(name) + ": " + target + " on <" + el->GetTagName() +
                                                    (el->GetId().empty() ? "" : " id=\"" + el->GetId() + "\"") +
                                                    "> doesn't name an element in this document (write #id)"});
            }
        }
        doc->Close();
        check->Update();
    } else if (problems.empty()) {
        problems.push_back({std::string(document), "RmlUi couldn't load the document"});
    }
    m->discard = false;
    ui::setCapture(nullptr);
    Rml::RemoveContext(name);
    // Drop duplicates (the same complaint for every element it applies to).
    std::vector<UIProblem> unique;
    for (UIProblem& p : problems) {
        const bool seen =
            std::any_of(unique.begin(), unique.end(), [&](const UIProblem& u) { return u.message == p.message; });
        if (!seen) unique.push_back(std::move(p));
    }
    return unique;
}

std::vector<std::string> GameUI::documentFiles() const {
    std::vector<std::string> files;
    if (m->content.empty()) return files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(m->content, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".rml")
            files.push_back(it->path().lexically_relative(m->content).generic_string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool GameUI::loaded(World& world, Entity documentEntity) {
    if (!m->find(documentEntity) && world.has<UIDocument>(documentEntity)) m->sync(world);
    const Impl::Document* d = m->find(documentEntity);
    return d && d->doc;
}

bool GameUI::setText(Entity e, std::string_view id, std::string_view text) {
    Rml::Element* el = m->element(e, id);
    if (!el) return false;
    const Rml::String encoded = Rml::StringUtilities::EncodeRml(Rml::String(text));
    if (el->GetInnerRML() != encoded) el->SetInnerRML(encoded);
    return true;
}

std::optional<std::string> GameUI::text(Entity e, std::string_view id) const {
    Rml::Element* el = m->element(e, id);
    if (!el) return std::nullopt;
    return Rml::StringUtilities::DecodeRml(el->GetInnerRML());
}

bool GameUI::setRml(Entity e, std::string_view id, std::string_view rml) {
    Rml::Element* el = m->element(e, id);
    if (!el) return false;
    el->SetInnerRML(Rml::String(rml));
    return true;
}

bool GameUI::setClass(Entity e, std::string_view id, std::string_view name, bool on) {
    Rml::Element* el = m->element(e, id);
    if (!el) return false;
    el->SetClass(Rml::String(name), on);
    return true;
}

bool GameUI::setAttribute(Entity e, std::string_view id, std::string_view name, std::string_view value) {
    Rml::Element* el = m->element(e, id);
    if (!el) return false;
    el->SetAttribute(Rml::String(name), Rml::String(value));
    return true;
}

bool GameUI::setProperty(Entity e, std::string_view id, std::string_view name, std::string_view value) {
    Rml::Element* el = m->element(e, id);
    if (!el) return false;
    return el->SetProperty(Rml::String(name), Rml::String(value));
}

bool GameUI::focus(Entity e, std::string_view id) {
    Rml::Element* el = m->element(e, id);
    return el && el->Focus();
}

std::optional<glm::vec4> GameUI::elementRect(Entity e, std::string_view id) {
    Rml::Element* el = m->element(e, id);
    if (!el) return std::nullopt;
    m->context->Update();
    const Rml::Vector2f offset = el->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = el->GetBox().GetSize(Rml::BoxArea::Border);
    return glm::vec4(offset.x, offset.y, size.x, size.y);
}

bool GameUI::click(Entity e, std::string_view id) {
    Rml::Element* el = m->element(e, id);
    if (!el) return false;
    el->Click();
    m->dispatch();
    return true;
}

void GameUI::setValue(std::string_view name, double value) { m->setValue(name, UIValue(value)); }
void GameUI::setValue(std::string_view name, bool value) { m->setValue(name, UIValue(value)); }
void GameUI::setValue(std::string_view name, std::string_view value) { m->setValue(name, UIValue(value)); }
void GameUI::setValue(std::string_view name, UIValue value) { m->setValue(name, std::move(value)); }

const UIValue& GameUI::value(std::string_view name) const {
    const auto it = m->values.find(name);
    return it == m->values.end() ? unsetValue() : it->second;
}

std::string GameUI::valueText(std::string_view name) const { return value(name).toText(); }
double GameUI::valueNumber(std::string_view name) const { return value(name).number(); }

void GameUI::setPreview(bool on) {
    if (on == m->previewOn) return;
    m->previewOn = on;
    if (m->modelHandle) m->modelHandle.DirtyAllVariables();
}

bool GameUI::preview() const { return m->previewOn; }

std::vector<std::string> GameUI::referencedValues(std::string_view document) const {
    std::ifstream in(ui::resolve(std::string(document)), std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return ui::referencedNames(text.str());
}

void GameUI::clearValues() {
    if (m->values.empty()) return;
    m->values.clear();
    if (m->modelHandle) m->modelHandle.DirtyAllVariables();
}

void GameUI::setEventListener(std::function<void(const UIEvent&)> listener) {
    m->listener = std::move(listener);
    if (!m->listener) m->queued.clear();
}

UIStats GameUI::stats() const { return m->stats; }

}   // namespace shockwave
