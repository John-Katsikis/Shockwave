// Player preferences from the command line (docs/Preferences.md): --preferences lists every
// value and the layer it came from, and --check reads the project's shipped file.
#include "Player.h"

#include <Engine/Localization.h>
#include <Engine/Preferences.h>
#include <GameFramework/AppShell.h>
#include <GameUI/GameUI.h>
#include <Projects/Diagnostic.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace player {

int preferencesInfo(
    const shockwave::Project& project, const fs::path& profile,
    const std::vector<std::pair<std::string, std::string>>& session, const fs::path& json) {
    shockwave::PreferenceStack stack;
    std::string error;
    const fs::path shipped = shockwave::projectPreferencesPath(project);
    if (!stack.loadShipped(shipped, error)) throw std::runtime_error(error);
    if (!stack.loadProfile(profile, error)) throw std::runtime_error(error);
    for (const auto& [key, value] : session)
        if (!stack.setSession(key, value, error))
            throw std::runtime_error("--pref " + key + "=" + value + ": " + error);
    const shockwave::PlayerPreferences prefs = stack.effective();
    std::error_code ec;
    const bool hasShipped = fs::exists(shipped, ec);
    if (!json.empty()) {
        std::string out =
            "{\"format\": 1, \"shipped\": " +
            (hasShipped ? shockwave::diagnosticJsonString(project.relative(shipped)) : std::string("null")) +
            ", \"profile\": " +
            (profile.empty() ? std::string("null") : shockwave::diagnosticJsonString(profile.string())) +
            ", \"preferences\": [";
        bool first = true;
        for (const auto& info : shockwave::preferenceInfo()) {
            out += std::string(first ? "" : ", ") + "{\"key\": \"" + info.key + "\", \"group\": \"" + info.group +
                   "\", \"value\": " + shockwave::diagnosticJsonString(info.get(prefs)) + ", \"origin\": \"" +
                   shockwave::preferenceLayerName(stack.origin(info.key)) + "\"}";
            first = false;
        }
        out += "]}\n";
        if (json == "-")
            std::fputs(out.c_str(), stdout);
        else {
            std::ofstream file(json);
            file << out;
            if (!file) throw std::runtime_error("can't write " + json.string());
            std::printf("Wrote %s\n", json.string().c_str());
        }
        return 0;
    }
    std::printf(
        "# shipped: %s\n# profile: %s\n", hasShipped ? project.relative(shipped).c_str() : "none",
        profile.empty() ? "disabled" : profile.string().c_str());
    for (const auto& info : shockwave::preferenceInfo()) {
        std::string value = info.get(prefs);
        if (value.empty() || value.find(' ') != std::string::npos) value = "\"" + value + "\"";
        std::printf("%s %s   # %s\n", info.key, value.c_str(), shockwave::preferenceLayerName(stack.origin(info.key)));
    }
    return 0;
}

void checkPreferences(const shockwave::Project& project, std::vector<std::string>& problems) {
    const fs::path file = shockwave::projectPreferencesPath(project);
    std::error_code ec;
    if (!fs::exists(file, ec)) return;
    shockwave::PreferenceStack stack;
    std::string error;
    if (!stack.loadShipped(file, error)) {
        if (error.rfind(file.string(), 0) == 0) error.replace(0, file.string().size(), project.relative(file));
        problems.push_back(error);
        return;
    }
    if (!shockwave::validatePreferences(stack.effective(), error)) {
        problems.push_back(project.relative(file) + ": " + error);
        return;
    }
    std::printf(
        "ok %s (%zu preferences)\n", project.relative(file).c_str(),
        stack.layer(shockwave::PreferenceLayer::Shipped).size());
}

void checkLocalization(const shockwave::Project& project, std::vector<std::string>& problems) {
    const fs::path folder = project.assetFolder / "Localization";
    shockwave::Localization text;
    text.load(folder, project.defaultLanguage);
    text.addBuiltin("en", shockwave::AppShell::builtinText());
    auto shown = [&](std::string line) {
        const std::string full = folder.string();
        if (const size_t at = line.find(full); at != std::string::npos)
            line.replace(at, full.size(), project.relative(folder));
        return line;
    };
    const size_t before = problems.size();
    for (const std::string& problem : text.check()) {
        if (problem.rfind("warning: ", 0) == 0)
            std::printf("%s\n", shown(problem).c_str());
        else
            problems.push_back(shown(problem));
    }
    // Every {{text.key}} a document shows must have text somewhere, or it shows nothing.
    std::error_code ec;
    const std::regex reference(R"(\btext\.([a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*))");
    std::set<std::string> missing;
    for (auto it = fs::recursive_directory_iterator(project.assetFolder, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".rml") continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::ostringstream all;
        all << in.rdbuf();
        const std::string body = all.str();
        int line = 1;
        size_t counted = 0;
        for (auto m = std::sregex_iterator(body.begin(), body.end(), reference); m != std::sregex_iterator(); ++m) {
            line += int(std::count(body.begin() + std::ptrdiff_t(counted), body.begin() + m->position(), '\n'));
            counted = size_t(m->position());
            const std::string key = (*m)[1];
            if (text.has(key) || !missing.insert(project.relative(it->path()) + key).second) continue;
            problems.push_back(
                project.relative(it->path()) + ":" + std::to_string(line) + ": text." + key + " has no text in " +
                project.relative(folder) + " (it would show nothing)");
        }
    }
    // Every character needs a font that draws it, or it shows blank: the engine's Noto Sans
    // has Latin, Greek and Cyrillic, and a game adds its own to Content for the rest.
    shockwave::GameUI ui(nullptr, {.dataModel = "check", .forEngine = false});
    ui.setContentFolder(project.assetFolder);
    for (const std::string& locale : text.locales()) {
        text.setLocale(locale);
        std::u32string all;
        size_t keys = 0;
        for (const auto& [key, value] : text.all()) {
            const std::u32string missing = ui.missingGlyphs(value);
            if (missing.empty()) continue;
            ++keys;
            for (const char32_t c : missing)
                if (all.find(c) == std::u32string::npos) all += c;
        }
        if (keys == 0) continue;
        std::string sample;
        for (size_t i = 0; i < all.size() && i < 8; ++i)
            sample += std::format("{}U+{:04X}", i ? " " : "", uint32_t(all[i]));
        problems.push_back(std::format(
            "{}/{}.swstrings: {} text{} {} characters no font draws ({}{}): add a font with them to Content",
            project.relative(folder), locale, keys, keys == 1 ? " has" : "s have", all.size(), sample,
            all.size() > 8 ? " ..." : ""));
    }
    text.setLocale("");
    if (problems.size() == before && fs::is_directory(folder, ec))
        std::printf(
            "ok %s (%zu language%s, %zu keys)\n", project.relative(folder).c_str(), text.locales().size(),
            text.locales().size() == 1 ? "" : "s", text.all().size());
}

}   // namespace player
