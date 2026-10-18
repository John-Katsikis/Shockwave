#include "ArenaGraphicsMenu.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace shockwave;
namespace fs = std::filesystem;
namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}   // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const auto temp =
        fs::temp_directory_path() /
        ("arena-graphics-menu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        GameUI ui(nullptr);
        ui.setContentFolder(fs::path(argv[1]) / "Content");
        World world;
        auto entity = world.create();
        world.add<Name>(entity, {"Arena Menu"});
        world.add<UIDocument>(entity, {"UI/Arena/menu.rml", true, 10, true, {}});
        JobSystem jobs(1);
        SubsystemCollection subsystems;
        auto& controls = subsystems.add<UIWorld>(world, &ui);
        auto& graphics = subsystems.add<Graphics>();
        GameContext game{world, jobs, nullptr, subsystems};
        arena::GraphicsMenu menu;
        menu.bind(game);
        ui.update(world, 0);
        ui.layout();
        for (const auto& problem : ui.problems(entity)) std::fprintf(stderr, "%s\n", problem.message.c_str());
        check(ui.problems(entity).empty(), "menu has no markup/style problems");
        const auto initial = graphics.current();
        check(controls.click("Arena Menu", "graphics"), "open graphics");
        ui.layout();
        check(menu.visible(), "graphics panel open");
        check(controls.focused("Arena Menu") == "preset-low", "opening focuses first preset");
        check(controls.navigate(UINav::Confirm), "keyboard confirms focused preset");
        check(controls.text("Arena Menu", "graphics-scale") == "Render scale: 67%", "navigation edits draft");
        check(controls.click("Arena Menu", "preset-low"), "choose low");
        check(graphics.current() == initial, "draft doesn't change runtime");
        check(controls.navigate(UINav::Back), "back navigation cancels graphics");
        check(!menu.visible() && graphics.current() == initial, "cancel discards draft");
        controls.click("Arena Menu", "graphics");
        controls.click("Arena Menu", "preset-low");
        controls.click("Arena Menu", "graphics-apply");
        check(
            graphics.current().renderScale == 0.67f && graphics.current().shadows == ShadowQuality::Low,
            "apply invokes shared API");
        controls.click("Arena Menu", "graphics-defaults");
        check(graphics.current().renderScale == 0.67f, "defaults only change draft");
        std::string error;
        check(graphics.configure(initial, temp / "Graphics.swgraphics", error), "configure saving");
        controls.click("Arena Menu", "graphics-apply");
        GraphicsSettings loaded;
        check(
            loadGraphicsSettings(graphics.file(), loaded, error) && loaded == initial,
            "apply saves and applies defaults");
        std::ofstream(temp / "blocked") << "regular file";
        graphics.configure(initial, temp / "blocked" / "settings", error);
        controls.click("Arena Menu", "preset-ultra");
        controls.click("Arena Menu", "graphics-apply");
        check(graphics.current() == initial, "failed save leaves live settings alone");
        check(!controls.text("Arena Menu", "graphics-status").empty(), "failure shown to player");
        for (const auto size : {glm::uvec2{1280, 720}, glm::uvec2{800, 600}}) {
            ui.setSize(size);
            ui.layout();
            const auto panel = ui.elementRect(entity, "graphics-panel");
            check(
                panel && panel->x >= 0 && panel->y >= 0 && panel->x + panel->z <= size.x &&
                    panel->y + panel->w <= size.y,
                "graphics panel fits output");
        }
        fs::remove_all(temp);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        fs::remove_all(temp);
        return 1;
    }
}
