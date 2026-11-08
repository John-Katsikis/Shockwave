// Player preferences and localization (S08, docs/Preferences.md, docs/Localization.md):
//   - the .swprefs format: header, quoting, comments, unknown and duplicate keys, ranges, NaN
//   - the layers (shipped, profile, session) and where each value came from
//   - apply and save kept apart; save writes only what differs from the shipped values
//   - a display change kept within 15 seconds, reverted otherwise, and undone after a crash
//   - text catalogs: keys, placeholders, the fallback chain, format(), and --check's findings
#include <Engine/Localization.h>
#include <Engine/Preferences.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace shockwave;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                               \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

double fakeNow = 1000;
double fakeClock() { return fakeNow; }

void testFormat() {
    std::map<std::string, std::string> keys;
    std::string error;
    CHECK(parsePreferences(
        "swprefs 1\n# a comment\nmaster_volume 0.5\nlanguage \"pt-BR\"\nwindow_mode borderless\n", "p", keys, error));
    CHECK(keys.size() == 3 && keys["master_volume"] == "0.5" && keys["language"] == "pt-BR");
    CHECK(!parsePreferences("master_volume 1\n", "p", keys, error) && contains(error, "swprefs"));
    CHECK(!parsePreferences("swprefs 1\nloudness 3\n", "p", keys, error) && contains(error, "loudness"));
    CHECK(contains(error, "p:2"));   // with the line
    CHECK(!parsePreferences("swprefs 1\nui_scale 1\nui_scale 1.5\n", "p", keys, error) && contains(error, "ui_scale"));
    CHECK(!parsePreferences("swprefs 1\nmusic_volume 7\n", "p", keys, error) && contains(error, "range"));
    CHECK(!parsePreferences("swprefs 1\nmusic_volume nan\n", "p", keys, error));
    CHECK(!parsePreferences("swprefs 1\nwindow_mode sideways\n", "p", keys, error));
    CHECK(!parsePreferences("swprefs 1\nfield_of_view 20\n", "p", keys, error));   // 0, or 50..120
    // What's written reads back the same.
    keys = {{"language", "pt-BR"}, {"master_volume", "0.25"}};
    std::map<std::string, std::string> again;
    CHECK(parsePreferences(preferencesText(keys), "p", again, error) && again == keys);
    // Every preference is in the table once, with a getter that reads back through its setter.
    PlayerPreferences prefs;
    for (const PreferenceInfo& info : preferenceInfo()) {
        CHECK(findPreference(info.key) == &info);
        PlayerPreferences copy = prefs;
        CHECK(info.set(copy, info.get(prefs), error) && copy == prefs);
    }
    CHECK(contains(preferencesSchema(), "master_volume") && contains(preferencesSchema(), "window_mode"));
    PlayerPreferences bad;
    bad.uiScale = std::nanf("");
    CHECK(!validatePreferences(bad, error));
}

void testLayers(const fs::path& root) {
    PreferenceStack stack;
    std::string error;
    write(root / "Shipped.swprefs", "swprefs 1\nmusic_volume 0.6\nsubtitles false\n");
    write(root / "Profile.swprefs", "swprefs 1\nmusic_volume 0.3\n");
    CHECK(stack.loadShipped(root / "Shipped.swprefs", error));
    CHECK(stack.loadProfile(root / "Profile.swprefs", error));
    CHECK(stack.loadProfile(root / "None.swprefs", error));   // a missing file: no layer
    CHECK(stack.layer(PreferenceLayer::Profile).empty());
    CHECK(stack.loadProfile(root / "Profile.swprefs", error));
    CHECK(stack.setSession("ui_scale", "1.5", error));
    CHECK(!stack.setSession("ui_scale", "9", error));
    const PlayerPreferences now = stack.effective();
    CHECK(std::abs(now.musicVolume - 0.3f) < 1e-6f && !now.subtitles && now.uiScale == 1.5f);
    CHECK(stack.origin("music_volume") == PreferenceLayer::Profile);
    CHECK(stack.origin("subtitles") == PreferenceLayer::Shipped);
    CHECK(stack.origin("ui_scale") == PreferenceLayer::Session);
    CHECK(stack.origin("invert_look_y") == PreferenceLayer::Default);
    CHECK(std::string(preferenceLayerName(PreferenceLayer::Session)) == "session");
    // The profile keeps only what differs from the layers under it.
    PlayerPreferences chosen = stack.below(PreferenceLayer::Profile);
    chosen.invertLookY = true;
    stack.setProfile(chosen);
    CHECK(
        stack.layer(PreferenceLayer::Profile).size() == 1 &&
        stack.layer(PreferenceLayer::Profile).count("invert_look_y"));
    // A layer that doesn't read is left as it was.
    write(root / "Broken.swprefs", "swprefs 1\nmusic_volume loud\n");
    CHECK(!stack.loadShipped(root / "Broken.swprefs", error) && contains(error, "Broken.swprefs"));
    CHECK(stack.layer(PreferenceLayer::Shipped).count("subtitles"));
}

void testApplyAndSave(const fs::path& root) {
    const fs::path config = root / "Config";
    const fs::path profile = config / "Preferences.swprefs";
    write(root / "Shipped.swprefs", "swprefs 1\nmusic_volume 0.6\n");
    std::string error;
    {
        Preferences prefs;
        prefs.setClock(&fakeClock);
        CHECK(prefs.configure(root / "Shipped.swprefs", profile, {{"ui_scale", "1.25"}}, error));
        CHECK(std::abs(prefs.current().musicVolume - 0.6f) < 1e-6f && prefs.current().uiScale == 1.25f);
        CHECK(std::abs(prefs.defaults().musicVolume - 0.6f) < 1e-6f);
        // A bad --pref fails the setup, saying which.
        Preferences other;
        CHECK(!other.configure({}, {}, {{"ui_scale", "40"}}, error) && contains(error, "ui_scale"));

        // Apply doesn't save; save doesn't apply.
        PlayerPreferences next = prefs.current();
        next.voiceVolume = 0.2f;
        CHECK(prefs.apply(next, error) && prefs.current().voiceVolume == 0.2f && !fs::exists(profile));
        PlayerPreferences saved = prefs.current();
        saved.effectsVolume = 0.5f;
        CHECK(prefs.save(saved, error) && fs::exists(profile) && prefs.current().effectsVolume == 1);
        const std::string text = read(profile);
        CHECK(contains(text, "effects_volume 0.5") && contains(text, "voice_volume 0.2"));
        CHECK(!contains(text, "music_volume"));   // the shipped value isn't repeated
        // An invalid set changes nothing.
        PlayerPreferences bad = prefs.current();
        bad.stickDeadZone = 3;
        CHECK(!prefs.apply(bad, error) && prefs.current().stickDeadZone == 0.15f);
        // No profile file: applying works, saving says why.
        Preferences none;
        CHECK(none.configure({}, {}, {}, error) && none.apply(next, error));
        CHECK(!none.save(next, error) && contains(error, "profile"));

        // A display change: kept within 15 seconds...
        PlayerPreferences display = prefs.current();
        display.windowMode = WindowMode::Borderless;
        CHECK(prefs.apply(display, error) && prefs.displayPending());
        CHECK(fs::exists(config / "Display.pending"));
        CHECK(std::abs(prefs.displaySecondsLeft() - Preferences::kKeepSeconds) < 1e-6);
        // ...saved meanwhile as the display that worked...
        CHECK(prefs.save(prefs.current(), error) && !contains(read(profile), "borderless"));
        fakeNow += 5;
        prefs.update();
        CHECK(prefs.displayPending() && prefs.current().windowMode == WindowMode::Borderless);
        prefs.keepDisplay();
        CHECK(!prefs.displayPending() && !fs::exists(config / "Display.pending"));
        CHECK(contains(read(profile), "window_mode borderless"));
        // ...or put back once the time runs out.
        display.windowMode = WindowMode::Fullscreen;
        display.windowWidth = 1280;
        display.windowHeight = 720;
        CHECK(prefs.apply(display, error) && prefs.displayPending());
        fakeNow += Preferences::kKeepSeconds + 0.5;
        prefs.update();
        CHECK(!prefs.displayPending() && prefs.current().windowMode == WindowMode::Borderless);
        CHECK(prefs.current().windowWidth == 0);
        // Revert straight away, as the page's Go back does.
        CHECK(prefs.apply(display, error) && prefs.displayPending());
        prefs.revertDisplay();
        CHECK(!prefs.displayPending() && prefs.current().windowMode == WindowMode::Borderless);

        // A change still waiting when the game stops (a crash, say)...
        CHECK(prefs.apply(display, error) && fs::exists(config / "Display.pending"));
        // The crash wrote the attempt to the profile by hand, as a broken save might.
        write(profile, "swprefs 1\nwindow_mode fullscreen\nwindow_width 1280\nwindow_height 720\n");
    }
    // ...is undone on the next start: the display that worked, and the marker gone.
    Preferences restarted;
    restarted.setClock(&fakeClock);
    CHECK(restarted.configure(root / "Shipped.swprefs", profile, {}, error));
    CHECK(restarted.current().windowMode == WindowMode::Borderless && restarted.current().windowWidth == 0);
    CHECK(!fs::exists(config / "Display.pending") && contains(read(profile), "window_mode borderless"));
    // A broken profile doesn't stop the game: the shipped preferences.
    write(profile, "swprefs 1\nmaster_volume very\n");
    Preferences broken;
    CHECK(broken.configure(root / "Shipped.swprefs", profile, {}, error) && broken.current().masterVolume == 1);
}

void testLocalization(const fs::path& root) {
    std::string error;
    // Keys and placeholders.
    CHECK(validTextKey("hud.objective") && validTextKey("a_1.b2") && !validTextKey("Hud.x"));
    CHECK(!validTextKey("hud..x") && !validTextKey(".x") && !validTextKey("x.") && !validTextKey(""));
    std::vector<std::string> names;
    CHECK(textPlaceholders("{a} of {b}, {a} {{literal}}", names, error) && names == std::vector<std::string>{"a", "b"});
    CHECK(!textPlaceholders("{a", names, error) && !textPlaceholders("{Bad}", names, error));
    // The file.
    TextCatalog catalog;
    CHECK(parseTextCatalog("swstrings 1\n# hi\nmenu.play \"Play \\\"now\\\"\"\n", "c", catalog, error));
    CHECK(catalog.find("menu.play") && *catalog.find("menu.play") == "Play \"now\"");
    CHECK(!parseTextCatalog("menu.play \"x\"\n", "c", catalog, error));
    CHECK(!parseTextCatalog("swstrings 1\nmenu.play x\n", "c", catalog, error));
    CHECK(
        !parseTextCatalog("swstrings 1\nmenu.play \"a\"\nmenu.play \"b\"\n", "c", catalog, error) &&
        contains(error, "c:3"));
    CHECK(!parseTextCatalog("swstrings 1\nMenu \"a\"\n", "c", catalog, error));
    CHECK(!parseTextCatalog("swstrings 1\nmenu.x \"{oops\"\n", "c", catalog, error));

    // The fallback chain: pt-BR, pt, the project's default, the engine's own, then missing.
    const fs::path folder = root / "Localization";
    write(
        folder / "en.swstrings",
        "swstrings 1\nmenu.play \"Play\"\nmenu.quit \"Quit\"\nhud.ammo \"{count} rounds\"\nhud.only_en \"English\"\n");
    write(folder / "pt.swstrings", "swstrings 1\nmenu.play \"Jogar\"\nmenu.quit \"Sair\"\n");
    write(folder / "pt-BR.swstrings", "swstrings 1\nmenu.play \"Bora\"\nhud.ammo \"{count} balas {extra}\"\n");
    write(folder / "de.swstrings", "swstrings 1\nmenu.play \"Spielen\"\nhud.stray \"x\"\n");
    write(folder / "xx.swstrings", "not a catalog\n");
    Localization text;
    text.load(folder, "en");
    text.addBuiltin("en", {{"shell.quit", "Quit game"}, {"menu.quit", "Builtin quit"}});
    CHECK(text.problems().size() == 1 && contains(text.problems().front(), "xx.swstrings"));
    CHECK((text.locales() == std::vector<std::string>{"de", "en", "pt", "pt-BR"}));
    const uint64_t before = text.version();
    text.setLocale("pt-BR");
    CHECK(text.version() > before && text.locale() == "pt-BR");
    CHECK(text.text("menu.play") == "Bora");   // the locale
    CHECK(text.text("menu.quit") == "Sair");   // its language
    CHECK(text.text("hud.only_en") == "English");   // the default
    CHECK(text.text("shell.quit") == "Quit game");   // the engine's own
    CHECK(text.text("menu.nothing") == "[missing: menu.nothing]" && !text.has("menu.nothing"));
    text.setLocale("fr");   // no catalog: the default
    CHECK(text.text("menu.play") == "Play");
    text.setLocale("");
    CHECK(text.text("menu.quit") == "Quit");   // a catalog wins over the built-in text
    // format(): placeholders filled, braces kept, a missing value left showing.
    CHECK(text.format("hud.ammo", {{"count", "12"}}) == "12 rounds");
    text.setLocale("pt-BR");
    CHECK(text.format("hud.ammo", {{"count", "3"}}) == "3 balas {extra}");
    text.addBuiltin("en", {{"hud.braces", "{{literal}} {n}"}});
    CHECK(text.format("hud.braces", {{"n", "1"}}) == "{literal} 1");
    // all(): every key with its text now.
    bool found = false;
    for (const auto& [key, value] : text.all()) found |= key == "menu.play" && value == "Bora";
    CHECK(found);
    // check(): the broken file, a stray key, mismatched placeholders, and untranslated keys.
    const auto problems = text.check();
    std::string all;
    for (const auto& p : problems) all += p + "\n";
    CHECK(contains(all, "xx.swstrings"));
    CHECK(contains(all, "de.swstrings:3: hud.stray isn't in en.swstrings"));
    CHECK(contains(all, "pt-BR.swstrings:3: hud.ammo has placeholders"));
    CHECK(contains(all, "warning: ") && contains(all, "not translated yet"));
    // A key only the engine has can be translated without a complaint.
    write(folder / "de.swstrings", "swstrings 1\nmenu.play \"Spielen\"\nshell.quit \"Beenden\"\n");
    fs::remove(folder / "xx.swstrings");
    write(folder / "pt-BR.swstrings", "swstrings 1\nmenu.play \"Bora\"\nhud.ammo \"{count} balas\"\n");
    text.load(folder, "en");
    text.addBuiltin("en", {{"shell.quit", "Quit game"}});
    for (const auto& p : text.check()) {
        CHECK(p.rfind("warning: ", 0) == 0);
    }
    // No default catalog while others exist: nothing to fall back to.
    Localization german;
    german.load(folder, "it");
    bool noDefault = false;
    for (const auto& p : german.check()) noDefault |= contains(p, "the default language \"it\"");
    CHECK(noDefault);
    // A missing folder: no catalogs, only the built-in text.
    Localization none;
    none.load(root / "Nowhere", "");
    CHECK(none.problems().empty() && none.defaultLanguage() == "en" && none.check().empty());
}

}   // namespace

int main() {
    const fs::path root =
        fs::temp_directory_path() /
        ("shockwave-preferences-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    testFormat();
    testLayers(root);
    testApplyAndSave(root);
    testLocalization(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) {
        std::fprintf(stderr, "preferences: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    std::printf("preferences: all checks passed\n");
    return 0;
}
