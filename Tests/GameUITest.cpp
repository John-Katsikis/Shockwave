// Game UI without a GPU: documents load from a Content folder, lay out with the bundled
// font, shape text with HarfBuzz, bind the "game" data model, report RML and RCSS
// problems, hot reload, and route clicks to game code through UIWorld.
#include <Engine/Components.h>
#include <Engine/World.h>
#include <GameUI/GameUI.h>
#include <GameUI/UIWorld.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::trunc);
    out << text;
}

// Pushes a file's time stamp forward so a reload notices it on any file system.
void touch(const fs::path& file, int seconds) {
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(seconds));
}

constexpr const char* kStyle = R"(
body { font-family: "Noto Sans"; font-size: 32px; color: #ffffff; width: 100%; height: 100%; }
span { display: inline-block; font-size: 128px; }
.big { font-size: 64px; }
)";

constexpr const char* kHud = R"(<rml>
<head><link type="text/rcss" href="hud.rcss"/><title>HUD</title></head>
<body data-model="game">
  <p id="score">{{score}}</p>
  <p id="label">Ready</p>
  <span id="av">AV</span> <span id="a">A</span> <span id="v">V</span>
  <div id="menu"><button id="resume" onclick="resume">Resume</button></div>
</body>
</rml>)";

}   // namespace

int main() {
    try {
        registerGameUIComponents();
        const ComponentInfo* info = findComponent("UIDocument");
        check(
            info && info->fields.size() == 5 && info->fields[0].type == FieldType::UIDocument,
            "UIDocument is registered with its fields");

        const fs::path content = fs::temp_directory_path() / "shockwave-gameui-test";
        fs::remove_all(content);
        write(content / "UI/hud.rcss", kStyle);
        write(content / "UI/hud.rml", kHud);
        write(content / "UI/broken.rcss", "body { font-family: \"Noto Sans\"; colr: red; width: ; }\n");
        write(
            content / "UI/broken.rml",
            "<rml><head><link type=\"text/rcss\" href=\"broken.rcss\"/></head><body><p>Hi</p></body></rml>");
        write(
            content / "UI/nofont.rml",
            "<rml><head><style>body { font-family: Nonexistent; }</style></head><body>Hello</body></rml>");

        GameUI ui(nullptr);
        ui.setContentFolder(content);
        check(ui.size() == glm::uvec2(1280, 720), "tools lay out at 1280x720 by default");
        check(ui.documentFiles().size() == 3, "documentFiles lists the .rml files");

        World world;
        const Entity hud = world.create();
        world.add<Name>(hud, {"HUD"});
        world.add<UIDocument>(hud, {"UI/hud.rml", true, 0, false, {}});
        ui.update(world, 0.0);
        check(ui.problems(hud).empty(), "hud.rml loads cleanly");
        check(ui.stats().documents == 1 && ui.stats().visible == 1, "one document, shown");

        // Drawing: text is geometry with the glyph atlas texture, in pixels.
        UiDrawList list = ui.layout();
        check(!list.commands.empty() && !list.vertices.empty() && list.indices.size() % 3 == 0, "draw list built");
        check(list.size == glm::uvec2(1280, 720), "draw list carries the layout size");

        // Data model and element text.
        UIWorld game(world, &ui);
        game.set("score", 42);
        ui.layout();
        check(
            game.text("HUD", "score") == "42",
            "{{score}} shows the game value, got '" + game.text("HUD", "score") + "'");
        check(game.number("score") == 42.0 && game.get("score") == "42", "values read back");
        check(game.setText("HUD", "label", "Go <now> & win"), "setText finds the element");
        check(game.text("hud", "label") == "Go <now> & win", "setText is plain text, found by file stem too");
        check(
            !game.setText("HUD", "nothing", "x") && !game.setText("Nope", "label", "x"), "missing targets are refused");

        // HarfBuzz shapes "AV" with the font's kerning (Noto Sans kerns through GPOS only,
        // which a plain FreeType engine ignores).
        const auto av = ui.elementRect(hud, "av"), a = ui.elementRect(hud, "a"), v = ui.elementRect(hud, "v");
        check(av && a && v && av->z > 10.0f, "spans are laid out");
        check(
            av->z < a->z + v->z - 1.0f,
            "AV is kerned: " + std::to_string(av->z) + " vs " + std::to_string(a->z + v->z));

        // Crisp at any size: text is laid out at the output's pixel size.
        ui.setSize({2560, 1440});
        ui.update(world, 0.0);
        check(ui.layout().size == glm::uvec2(2560, 1440), "layout follows the output size");

        // Events: an inline handler names a command; clicks reach element handlers by id,
        // including a parent's.
        int resumed = 0, menuClicks = 0;
        game.on("resume", [&](const UIEvent& e) {
            ++resumed;
            check(e.document == "HUD" && e.element == "resume" && e.type == "click", "command event fields");
        });
        game.onClick("HUD", "menu", [&](const UIEvent& e) {
            ++menuClicks;
            check(e.element == "resume", "the innermost id is the element");
        });
        check(game.click("HUD", "resume"), "click finds the button");
        check(resumed == 1 && menuClicks == 1, "both handlers ran once");

        // Show and hide through the component, as game code does.
        check(game.hide("HUD") && !world.get<UIDocument>(hud)->visible, "hide clears visible");
        ui.update(world, 0.0);
        check(ui.layout().commands.empty(), "a hidden document draws nothing");
        check(game.toggle("HUD") && game.visible("HUD"), "toggle shows it again");
        ui.update(world, 0.0);

        // Problems: RCSS errors, a missing font, a missing file.
        const auto broken = ui.validate("UI/broken.rml");
        check(!broken.empty(), "broken RCSS is reported");
        bool mentionsFile = false;
        for (const auto& p : broken) mentionsFile |= p.message.find("broken.rcss") != std::string::npos;
        check(mentionsFile, "the problem names the stylesheet");
        const auto nofont = ui.validate("UI/nofont.rml");
        check(
            nofont.size() == 1 && nofont[0].message.find("missing font") != std::string::npos,
            "a missing font is reported once");
        check(ui.validate("UI/missing.rml").at(0).message == "file not found in Content", "missing file");
        check(ui.validate("UI/hud.rml").empty(), "hud.rml validates");

        // Hot reload: change the document on disk, and the next poll reloads it.
        std::string edited = kHud;
        edited.replace(edited.find("Ready"), 5, "Edited");
        write(content / "UI/hud.rml", edited);
        touch(content / "UI/hud.rml", 5);
        ui.update(world, 1.0);
        check(game.text("HUD", "label") == "Edited", "the document reloaded from disk");
        // A stylesheet change reloads the documents that use it.
        write(content / "UI/hud.rcss", std::string(kStyle) + "#label { font-size: 10px; }\n");
        touch(content / "UI/hud.rcss", 10);
        const float before = ui.elementRect(hud, "label")->w;
        ui.update(world, 1.0);
        const float after = ui.elementRect(hud, "label")->w;
        check(after < before, "the stylesheet reloaded");

        // ---- Values: lists and records, their text form, preview values ----
        {
            const auto parsed = UIValue::parse("[{name=Sword, count=2}, {name=\"Bow, long\", count=1}]");
            check(parsed && parsed->isList() && parsed->size() == 2, "a list of records parses");
            check((*parsed)[1].field("name")->text() == "Bow, long", "quoted text keeps its comma");
            check((*parsed)[0].field("count")->number() == 2.0, "numbers are numbers");
            check(
                parsed->toText() == "[{name=Sword, count=2}, {name=\"Bow, long\", count=1}]",
                "the text form reads back the same: " + parsed->toText());
            std::string error;
            check(!UIValue::parse("[a, b", &error) && !error.empty(), "an unclosed list is an error");
            const auto preview = parseUIPreview("score=12; name=Ana\nitems=[a, b]");
            check(preview && preview->size() == 3 && (*preview)[2].second.size() == 2, "preview values parse");
            check(!parseUIPreview("score 12", &error), "a preview entry needs '='");
        }

        write(content / "UI/list.rml", R"(<rml>
<head><link type="text/rcss" href="hud.rcss"/></head>
<body data-model="game">
  <p id="title">{{title}}</p>
  <div id="items"><p data-for="item : inventory">{{item.name}} x{{item.count}}</p></div>
  <p id="count">{{inventory.size}}</p>
</body>
</rml>)");
        const Entity listDoc = world.create();
        world.add<Name>(listDoc, {"List"});
        world.add<UIDocument>(listDoc, {"UI/list.rml", true, 1, false, "title=Bag; inventory=[{name=Rope, count=3}]"});
        ui.update(world, 0.0);
        ui.layout();
        check(ui.problems(listDoc).empty(), "list.rml loads cleanly");
        check(game.text("List", "title").empty(), "unset values show nothing outside preview");
        ui.setPreview(true);
        ui.layout();
        check(game.text("List", "title") == "Bag", "preview values fill in: '" + game.text("List", "title") + "'");
        check(game.text("List", "count") == "1", "a preview list has a size");
        game.set(
            "inventory", UIValue::list(
                             {UIValue::record({{"name", "Sword"}, {"count", 2}}),
                              UIValue::record({{"name", "Shield"}, {"count", 1}})}));
        ui.layout();
        const std::string items = game.text("List", "items");
        check(
            items.find("Sword x2") != std::string::npos && items.find("Shield x1") != std::string::npos,
            "data-for lists game code's records: " + items);
        check(game.text("List", "count") == "2", "game values win over preview values");
        check(game.value("inventory").size() == 2 && game.get("inventory").front() == '[', "lists read back");
        world.get<UIDocument>(listDoc)->preview = "title=[oops";
        ui.update(world, 0.0);
        check(!ui.problems(listDoc).empty(), "a broken preview is reported with its document");
        world.get<UIDocument>(listDoc)->preview.clear();
        ui.update(world, 0.0);
        ui.layout();
        check(game.text("List", "title") == "{{title}}", "unset values show their name while previewing");
        ui.setPreview(false);
        const auto names = ui.referencedValues("UI/list.rml");
        check(names.size() == 2 && names[0] == "inventory" && names[1] == "title", "referenced values are found");

        // ---- Menu navigation ----
        write(content / "UI/menu.rml", R"(<rml>
<head><link type="text/rcss" href="hud.rcss"/>
<style> button { display: block; width: 200px; height: 40px; } #row button { display: inline-block; } </style></head>
<body data-model="game" data-back="close_menu">
  <button id="play" onclick="play">Play</button>
  <button id="options">Options</button>
  <div id="row"><button id="left">L</button><button id="right">R</button></div>
  <input id="sound" type="checkbox"/>
</body>
</rml>)");
        const Entity menu = world.create();
        world.add<Name>(menu, {"Menu"});
        world.add<UIDocument>(menu, {"UI/menu.rml", true, 5, true, {}});
        ui.update(world, 0.0);
        ui.layout();
        check(game.focused("Menu") == "play", "a focus_on_show menu focuses its first button: " + game.focused("Menu"));
        check(game.navigate(UINav::Down) && game.focused("Menu") == "options", "down moves to the next button");
        check(game.navigate(UINav::Down) && game.focused("Menu") == "left", "down reaches the row");
        check(game.navigate(UINav::Right) && game.focused("Menu") == "right", "right moves along the row");
        check(game.navigate(UINav::Up) && game.focused("Menu") == "options", "up goes back");
        game.navigate(UINav::Up);
        int played = 0, closed = 0;
        game.on("play", [&](const UIEvent&) { ++played; });
        game.on("close_menu", [&](const UIEvent& e) {
            ++closed;
            check(e.type == "back" && e.document == "Menu", "back carries its type and document");
        });
        check(game.navigate(UINav::Confirm) && played == 1, "confirm clicks the focused button");
        check(game.navigate(UINav::Back) && closed == 1, "back sends the document's data-back command");
        game.focus("Menu", "sound");
        game.navigate(UINav::Confirm);
        ui.layout();
        check(game.focused("Menu") == "sound", "a checkbox can be focused");
        world.get<UIDocument>(menu)->visible = false;
        ui.update(world, 0.0);

        // ---- Gradients and box-shadow ----
        write(content / "UI/fx.rml", R"(<rml>
<head><link type="text/rcss" href="hud.rcss"/>
<style>
  #g { display: block; width: 200px; height: 100px; decorator: linear-gradient(to right, #ff0000, #0000ff); }
  #r { display: block; width: 100px; height: 100px; decorator: radial-gradient(circle, #ffffff, #00000000); }
  #s { display: block; width: 100px; height: 50px; margin: 40px; background-color: #202020; box-shadow: #000000a0 4px 8px 12px 2px; }
</style></head>
<body><div id="g"/><div id="r"/><div id="s"/></body>
</rml>)");
        const uint32_t texturesBefore = ui.stats().textures;
        const Entity fx = world.create();
        world.add<UIDocument>(fx, {"UI/fx.rml", true, 0, false, {}});
        ui.update(world, 0.0);
        ui.layout();
        check(ui.problems(fx).empty(), "fx.rml loads cleanly");
        ui.layout();
        check(
            ui.stats().textures >= texturesBefore + 3,
            "two gradients and a shadow make textures: " + std::to_string(ui.stats().textures - texturesBefore));
        check(ui.validate("UI/fx.rml").empty(), "gradients and shadows validate");

        // ---- --check: nav targets ----
        write(
            content / "UI/badnav.rml", "<rml><head><link type=\"text/rcss\" href=\"hud.rcss\"/></head><body>"
                                       "<button id=\"a\" style=\"nav-down: #nowhere;\">A</button></body></rml>");
        const auto badnav = ui.validate("UI/badnav.rml");
        check(
            badnav.size() == 1 && badnav[0].message.find("#nowhere") != std::string::npos,
            "a nav target that isn't there is reported");

        fs::remove_all(content);
        std::cout << "game ui tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
