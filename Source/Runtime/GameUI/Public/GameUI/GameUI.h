#pragma once

// Game UI: menus, HUDs and on-screen text, written as .rml documents (HTML-like markup)
// styled with .rcss (CSS), drawn over the frame at native resolution. Built on RmlUi,
// with FreeType rendering glyphs and HarfBuzz shaping text (kerning, ligatures, Arabic,
// Devanagari...). See docs/GameUI.md.
//
// A scene shows a document with a UIDocument component. Game code changes what's shown
// through game.ui() (UIWorld.h). The editor and the player each own one GameUI and call
// update() and render() once per frame.
//
// Threading: everything here is main-thread only. RmlUi isn't thread-safe, and the UI is
// cheap enough to lay out on the main thread (see docs/GameUI.md for costs).

#include <Engine/World.h>
#include <Renderer/RenderTypes.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace shockwave {

class Engine;
class Input;
class Renderer;

// Shows an .rml document from the project's Content folder over the scene.
struct UIDocument {
    std::string document;   // path inside Content, like "UI/hud.rml"
    bool visible = true;
    int order = 0;   // higher draws on top and gets the mouse first
    // Takes keyboard and gamepad focus when shown (menus); a HUD leaves it off.
    bool focusOnShow = false;
    // Stand-in {{values}} for the editor while not playing, and player --ui-preview:
    // "score=1200; name=Ana; items=[Sword, Shield]". See UIValue::parse.
    std::string preview;
};
// Registers UIDocument for scenes, the Inspector and player --schema. The hosts call it
// at startup, like registerAudioComponents.
void registerGameUIComponents();

// A {{value}} for documents: a number, true or false, text, a list (for data-for), or a
// record of named values ({{item.name}}). Lists and records nest.
//
//     UIValue::list({"Sword", "Shield"})
//     UIValue::record({{"name", "Sword"}, {"count", 2}})
//
// As text (preview values, player --ui-preview, UIWorld::get) it's written like this:
//     12   -3.5   true   false   Sword   "Text, with commas"
//     [Sword, Shield]   {name=Sword, count=2}   [{name=Sword}, {name=Bow}]
class UIValue {
public:
    enum class Kind : uint8_t { Unset, Number, Bool, Text, List, Record };

    UIValue() = default;
    template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    UIValue(T v) : m_kind(Kind::Number), m_number(static_cast<double>(v)) {}
    UIValue(bool v) : m_kind(Kind::Bool), m_number(v ? 1.0 : 0.0) {}
    UIValue(const char* v) : m_kind(Kind::Text), m_text(v ? v : "") {}
    UIValue(std::string_view v) : m_kind(Kind::Text), m_text(v) {}
    UIValue(const std::string& v) : m_kind(Kind::Text), m_text(v) {}
    UIValue(std::string&& v) : m_kind(Kind::Text), m_text(std::move(v)) {}

    static UIValue list(std::initializer_list<UIValue> items = {});
    static UIValue record(std::initializer_list<std::pair<std::string, UIValue>> fields = {});
    // Reads the text form. nullopt and an error message when it isn't valid.
    static std::optional<UIValue> parse(std::string_view text, std::string* error = nullptr);

    Kind kind() const { return m_kind; }
    bool isList() const { return m_kind == Kind::List; }
    bool isRecord() const { return m_kind == Kind::Record; }
    double number() const;   // text is parsed; 0 when it isn't a number
    bool flag() const { return m_kind == Kind::Bool ? m_number != 0.0 : number() != 0.0; }
    const std::string& text() const { return m_text; }   // Text only
    std::string toText() const;   // the text form; plain text is written as it is

    // Lists
    UIValue& push(UIValue item);
    size_t size() const { return m_items.size(); }   // list items or record fields
    const UIValue& operator[](size_t index) const { return m_items[index]; }
    // Records
    UIValue& set(std::string_view key, UIValue value);
    const UIValue* field(std::string_view key) const;
    const std::vector<std::string>& keys() const { return m_keys; }
    const std::vector<UIValue>& items() const { return m_items; }   // list items, or record values

    bool operator==(const UIValue& other) const;

private:
    Kind m_kind = Kind::Unset;
    double m_number = 0.0;
    std::string m_text;
    std::vector<UIValue> m_items;
    std::vector<std::string> m_keys;   // records: parallel to m_items
};

// Reads preview values: "name=value" entries separated by ';' or new lines, each value
// in UIValue's text form. nullopt and an error message when it isn't valid.
std::optional<std::vector<std::pair<std::string, UIValue>>>
parseUIPreview(std::string_view text, std::string* error = nullptr);

// Directions and buttons for moving around a menu without a mouse (GameUI::navigate).
enum class UINav : uint8_t { Up, Down, Left, Right, Confirm, Back };
// The input actions that drive menu navigation, bound by default to the arrow keys,
// Enter and Escape/Backspace, and on gamepads to the d-pad, left stick, South and East.
// Rebind them in Config/Input.swinput like any action.
inline constexpr const char* kUINavActions[] = {"ui_up", "ui_down", "ui_left", "ui_right", "ui_confirm", "ui_back"};

// Something a document did that game code can react to (UIWorld::on, UIWorld::onClick).
struct UIEvent {
    std::string_view document;   // the UIDocument entity's name, or its path if it has none
    std::string_view element;   // id of the element, or of its nearest parent that has one; "" if none
    std::span<const std::string_view> ids;   // element, then every parent's id, innermost first
    std::string_view type;   // "click", "change", "submit", "back", ...
    // An inline handler's text: onclick="resume" gives "resume". For "back", the
    // data-back attribute nearest the focused element: <body data-back="resume">.
    std::string_view command;
    std::string_view value;   // a form control's value, if any
    Entity entity;   // the UIDocument's entity
};

// What a document needs that isn't there, or what's wrong in it, from loading it the way
// the game would.
struct UIProblem {
    std::string file;   // inside Content
    std::string message;
};

// Per-frame cost and size, for the Stats window and tools.
struct UIStats {
    uint32_t documents = 0;   // loaded
    uint32_t visible = 0;
    uint32_t drawCommands = 0;   // after merging
    uint32_t vertices = 0;
    uint32_t textures = 0;   // glyph atlases, images, shadows and gradients
    float updateMs = 0;   // input, hot reload, data bindings
    float layoutMs = 0;   // layout and building the draw list
};

class GameUI {
public:
    // engine: where to draw and read input; null for tools and tests (documents still
    // load, lay out and report problems, nothing is drawn).
    explicit GameUI(Engine* engine);
    ~GameUI();
    GameUI(const GameUI&) = delete;
    GameUI& operator=(const GameUI&) = delete;

    // The GameUI an engine's host made, for game.ui(); null if there's none.
    static GameUI* forEngine(const Engine* engine);

    // The folder documents, stylesheets, images and fonts are read from: the project's
    // Content. Changing it unloads every document. Fonts (.ttf, .otf) in it load too.
    void setContentFolder(const std::filesystem::path& folder);
    const std::filesystem::path& contentFolder() const;

    // Once per frame: shows, hides, loads and orders documents to match the world's
    // UIDocuments, reloads changed files, and feeds input to the UI.
    void update(World& world, double frameDt);
    // Lays out and draws the visible documents (from Application::onRender).
    void render(Renderer& renderer);
    // Lays out and builds this frame's draw list without drawing it (render() does both).
    // Valid until the next layout or render. For tests and tools.
    UiDrawList layout();
    // The size documents are laid out at, in pixels. With an engine it follows the
    // window or editor viewport by itself; without one (tools, tests) it's 1280x720 until
    // set.
    void setSize(glm::uvec2 size);
    glm::uvec2 size() const;

    // ---- Input ----
    // Feeds input in this frame's update. Off, the UI only draws (the editor while
    // editing).
    void setInputEnabled(bool enabled);
    // Where the mouse is in UI pixels, for hosts that show the UI somewhere other than
    // the whole window (the editor viewport). nullopt: outside. Without a call, the
    // mouse's window position is used.
    void setPointer(std::optional<glm::vec2> pixels);
    void clearPointerOverride();
    // The mouse is over a document, or a document has keyboard focus: gameplay may want
    // to ignore clicks and keys this frame.
    bool wantsMouse() const;
    bool wantsKeyboard() const;

    // ---- Menu navigation (keyboard and gamepad) ----
    // While a shown document with focus_on_show is up, or an element has focus, the
    // ui_* actions (kUINavActions) move focus between buttons and controls, press the
    // focused one, and send "back". Held directions repeat. Needs input enabled.
    // navigate() does one step by hand: for game code, tools and tests. Returns false
    // when there was nothing to navigate.
    bool navigate(UINav nav);
    // The focused element's id ("" when it has none), and its document's entity.
    // nullopt when nothing has focus.
    std::optional<std::pair<Entity, std::string>> focused() const;
    // Binds the ui_* actions to their defaults where the project doesn't bind them. The
    // player and the editor call it at startup, like FlyCamera::bindDefaultControls.
    static void bindDefaultControls(Input& input);

    // ---- Documents ----
    // Reloads every document from disk (styles, templates and images too).
    void reloadAll();
    // Problems seen while loading a document the last time: RML and RCSS errors, missing
    // files and fonts. Empty when it loaded cleanly.
    const std::vector<UIProblem>& problems(Entity documentEntity) const;
    // Loads a document on its own, lays it out, and reports what's wrong with it. For
    // player --check and the editor; the document isn't kept.
    std::vector<UIProblem> validate(std::string_view document);
    // Every .rml in the Content folder, for the editor.
    std::vector<std::string> documentFiles() const;

    // ---- Game code (UIWorld forwards here) ----
    // The loaded document for a UIDocument entity, loading it now if needed.
    bool loaded(World& world, Entity documentEntity);
    bool setText(Entity documentEntity, std::string_view elementId, std::string_view text);
    std::optional<std::string> text(Entity documentEntity, std::string_view elementId) const;
    bool setRml(Entity documentEntity, std::string_view elementId, std::string_view rml);
    bool setClass(Entity documentEntity, std::string_view elementId, std::string_view name, bool on);
    bool setAttribute(Entity documentEntity, std::string_view elementId, std::string_view name, std::string_view value);
    bool setProperty(Entity documentEntity, std::string_view elementId, std::string_view name, std::string_view value);
    bool focus(Entity documentEntity, std::string_view elementId);
    // Where an element is laid out, in pixels: x, y, width, height of its border box.
    // nullopt when there's no such element or it isn't laid out. For tests and tools.
    std::optional<glm::vec4> elementRect(Entity documentEntity, std::string_view elementId);
    // Clicks the element as if with the mouse, and hands the events to the listener.
    bool click(Entity documentEntity, std::string_view elementId);
    // The "game" data model, bound in documents with data-model="game": {{score}}.
    void setValue(std::string_view name, double value);
    void setValue(std::string_view name, bool value);
    void setValue(std::string_view name, std::string_view value);
    void setValue(std::string_view name, UIValue value);   // lists and records too
    const UIValue& value(std::string_view name) const;   // Unset when game code hasn't set it
    std::string valueText(std::string_view name) const;
    double valueNumber(std::string_view name) const;
    void clearValues();
    // Preview: values game code hasn't set show the UIDocuments' preview values, or
    // their own name in braces ("{{score}}"), so a layout can be judged before Play.
    // The editor turns it on while editing; player --ui-preview turns it on.
    void setPreview(bool on);
    bool preview() const;
    // The {{names}} a document refers to, read from its .rml (for tools and the editor).
    std::vector<std::string> referencedValues(std::string_view document) const;
    // Receives every UI event while set (one listener: the Play session's UIWorld).
    void setEventListener(std::function<void(const UIEvent&)> listener);

    UIStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}   // namespace shockwave
