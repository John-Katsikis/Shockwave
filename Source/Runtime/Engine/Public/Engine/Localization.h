#pragma once

// The game's text in each language (S08, docs/Localization.md): a small catalog of stable
// keys and UTF-8 values, one file per locale in Content/Localization.
//
//   Content/Localization/en.swstrings
//     swstrings 1
//     menu.new_game "New game"
//     hud.objective "Objective: {objective}"
//
// A key is lowercase letters, digits and '_' in parts joined by '.', like hud.objective.
// A value is quoted UTF-8; {name} is a placeholder that format() fills, and {{ and }} are
// literal braces. Lookups fall back in a fixed order: the locale asked for ("pt-BR"), its
// language ("pt"), the project's default language (default_language in the .swproject),
// the engine's own English for its screens, then a visible "[missing: key]".
//
// Documents show text through the "text" value of their data model: {{text.menu.new_game}}.
// Game code formats sentences whole with format(), never by joining translated pieces.
// Values are text, never markup: GameUI escapes them like any other value.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave {

bool validTextKey(std::string_view key);
// The placeholders a value names, in order, without duplicates: "{a} of {b}" -> a, b.
// False with why when a brace doesn't close or a name isn't valid.
bool textPlaceholders(std::string_view value, std::vector<std::string>& names, std::string& error);

// One locale's file. Keys in file order.
struct TextCatalog {
    std::string locale;   // from the file name: "en", "pt-BR"
    std::vector<std::pair<std::string, std::string>> entries;
    std::map<std::string, int> lines;   // key -> line, for messages
    const std::string* find(std::string_view key) const;
};
bool parseTextCatalog(std::string_view text, const std::string& shown, TextCatalog& out, std::string& error);

class Localization {
public:
    // Reads every <locale>.swstrings in the folder (a missing folder: no catalogs).
    // defaultLanguage is the project's; empty is "en". A file that doesn't read is left out
    // and listed in problems().
    void load(const std::filesystem::path& folder, const std::string& defaultLanguage);
    // Built-in text, under every catalog: the engine's own screens (AppShell).
    void addBuiltin(const std::string& locale, std::vector<std::pair<std::string, std::string>> entries);
    // The locale to show; empty for the default. Unknown locales fall back as above.
    void setLocale(const std::string& locale);
    const std::string& locale() const { return m_locale; }
    const std::string& defaultLanguage() const { return m_default; }
    // The locales with a catalog, sorted: the Language setting's choices.
    std::vector<std::string> locales() const;

    // The text for a key, with the fallbacks above.
    std::string text(std::string_view key) const;
    bool has(std::string_view key) const;
    // The text with its placeholders filled: format("hud.ammo", {{"count", "12"}}). A
    // placeholder without a value stays as it is, so the gap shows.
    std::string format(std::string_view key, const std::vector<std::pair<std::string, std::string>>& values) const;
    // Every key any catalog or the built-in text has, sorted, with its text now.
    std::vector<std::pair<std::string, std::string>> all() const;

    // What's wrong across the catalogs, for player --check: files that don't read, a key
    // a locale has that the default doesn't, placeholders that differ from the default's,
    // keys a locale is missing (warnings, prefixed "warning: ").
    std::vector<std::string> check() const;
    const std::vector<std::string>& problems() const { return m_problems; }
    // Goes up whenever the text could have changed: load, setLocale, addBuiltin.
    uint64_t version() const { return m_version; }

private:
    const TextCatalog* catalog(std::string_view locale) const;
    std::vector<const TextCatalog*> chain() const;   // the lookup order for the locale now
    std::filesystem::path m_folder;
    std::vector<TextCatalog> m_catalogs;
    std::vector<TextCatalog> m_builtin;
    std::string m_default = "en", m_locale;
    std::vector<std::string> m_problems;
    uint64_t m_version = 1;
};

}   // namespace shockwave
