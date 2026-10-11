#pragma once

// UIWorld: game code's way into the UI during Play, as game.ui(). One per Play session,
// like AudioWorld and PhysicsWorld. When Play stops it drops your handlers, clears the
// data model and reloads every document, so nothing done while playing sticks.
//
//     game.ui().set("score", score);                        // {{score}} in the document
//     game.ui().setText("HUD", "hint", "Press Space");     // <p id="hint">
//     game.ui().show("PauseMenu");                          // the UIDocument entity's name
//     game.ui().on("resume", [&](const UIEvent&) { ... }); // <button onclick="resume">
//     game.ui().onClick("HUD", "menu", [&](const UIEvent&) { ... });   // <button id="menu">
//     game.ui().set("items", UIValue::list({"Sword", "Shield"}));  // data-for="item : items"
//
// Documents are named by their UIDocument entity's Name, or by their file ("UI/hud.rml")
// or its stem ("hud"). Elements are named by id. Calls that can't find their document or
// element return false and change nothing.
//
// Threading: main thread only (GameMode::beginPlay, frame, fixedUpdate, endPlay), never
// from systems.

#include <Engine/Subsystem.h>
#include <Engine/World.h>
#include <GameUI/GameUI.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

class UIWorld final : public Subsystem {
public:
    using Handler = std::function<void(const UIEvent&)>;

    // ui: null in tools and tests without a GameUI; then everything is a harmless no-op.
    UIWorld(World& world, GameUI* ui);
    ~UIWorld() override;

    // ---- Documents ----
    bool show(std::string_view document, bool visible = true);
    bool hide(std::string_view document) { return show(document, false); }
    bool toggle(std::string_view document);
    bool visible(std::string_view document) const;

    // ---- Elements, by id ----
    bool setText(std::string_view document, std::string_view element, std::string_view text);   // plain text
    std::string text(std::string_view document, std::string_view element) const;
    bool setRml(std::string_view document, std::string_view element, std::string_view rml);   // markup
    bool setClass(std::string_view document, std::string_view element, std::string_view name, bool on = true);
    bool
    setAttribute(std::string_view document, std::string_view element, std::string_view name, std::string_view value);
    // An inline style property, like setProperty("HUD", "bar", "width", "40%").
    bool
    setProperty(std::string_view document, std::string_view element, std::string_view name, std::string_view value);
    bool focus(std::string_view document, std::string_view element);
    // Clicks an element as the mouse would; handlers run before it returns.
    bool click(std::string_view document, std::string_view element);

    // ---- The "game" data model (data-model="game" in a document) ----
    void set(std::string_view name, double value);
    void set(std::string_view name, int value) { set(name, static_cast<double>(value)); }
    void set(std::string_view name, float value) { set(name, static_cast<double>(value)); }
    void set(std::string_view name, bool value);
    void set(std::string_view name, std::string_view value);
    void set(std::string_view name, const char* value) { set(name, std::string_view(value)); }
    void set(std::string_view name, const std::string& value) { set(name, std::string_view(value)); }
    // Lists for data-for and records for {{item.name}}:
    //     game.ui().set("inventory", UIValue::list({UIValue::record({{"name", "Sword"}})}));
    void set(std::string_view name, UIValue value);
    const UIValue& value(std::string_view name) const;
    std::string get(std::string_view name) const;   // as text; "" when unset
    double number(std::string_view name) const;   // 0 when unset or not a number

    // ---- Events ----
    // command: an inline handler's text in any document, like onclick="resume".
    void on(std::string_view command, Handler handler);
    // A click on the element with this id (or inside it) in a document.
    void onClick(std::string_view document, std::string_view element, Handler handler);
    // Any event type ("change", "submit", "mouseover"...) on an element.
    void onEvent(std::string_view document, std::string_view element, std::string_view type, Handler handler);

    // ---- Menu navigation ----
    // One step of keyboard or gamepad navigation, as the ui_* actions do: move focus,
    // press the focused element, or send "back" (on() gets its data-back command).
    bool navigate(UINav nav);
    // The id of the focused element in a document; "" when nothing there has focus.
    std::string focused(std::string_view document) const;

    // ---- Input ----
    // The UI is using the mouse or keyboard this frame (a menu has focus, the mouse is
    // over a button): skip gameplay input that would fight with it.
    bool capturesInput() const;

    // The UIDocument entity a name refers to, or an invalid Entity.
    Entity find(std::string_view document) const;

private:
    void dispatch(const UIEvent& event);

    World& m_world;
    GameUI* m_ui;
    std::map<std::string, std::vector<Handler>, std::less<>> m_commands;
    struct ElementHandler {
        std::string document, element, type;
        Handler handler;
    };
    std::vector<ElementHandler> m_elementHandlers;
};

}   // namespace shockwave
