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
            info && info->fields.size() == 4 && info->fields[0].type == FieldType::UIDocument,
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
        world.add<UIDocument>(hud, {"UI/hud.rml", true, 0, false});
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

        fs::remove_all(content);
        std::cout << "game ui tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
}
