// The game's text in each language (Localization.h, docs/Localization.md).
#include <Engine/Localization.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

bool keyPart(std::string_view part) {
    if (part.empty() || !(part[0] >= 'a' && part[0] <= 'z')) return false;
    return std::all_of(
        part.begin(), part.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// Well-formed UTF-8: no stray continuation bytes, overlong forms or surrogates.
bool validUtf8(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (extra < 0 || i + size_t(extra) >= text.size() + (extra == 0 ? 1 : 0)) return false;
        uint32_t code = extra == 0 ? c : extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
        for (int k = 1; k <= extra; ++k) {
            const unsigned char d = static_cast<unsigned char>(text[i + size_t(k)]);
            if ((d >> 6) != 0x2) return false;
            code = (code << 6) | (d & 0x3Fu);
        }
        if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) ||
            code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
        i += size_t(extra) + 1;
    }
    return true;
}

// "pt-BR" -> "pt".
std::string languageOf(const std::string& locale) {
    const size_t dash = locale.find('-');
    return dash == std::string::npos ? locale : locale.substr(0, dash);
}

}   // namespace

bool validTextKey(std::string_view key) {
    if (key.empty() || key.size() > 128) return false;
    size_t start = 0;
    while (true) {
        const size_t dot = key.find('.', start);
        if (!keyPart(key.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start)))
            return false;
        if (dot == std::string_view::npos) return true;
        start = dot + 1;
    }
}

bool textPlaceholders(std::string_view value, std::vector<std::string>& names, std::string& error) {
    names.clear();
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '}') {
            if (i + 1 < value.size() && value[i + 1] == '}') {
                ++i;
                continue;
            }
            error = "a } that no { opens (write }} for a brace)";
            return false;
        }
        if (value[i] != '{') continue;
        if (i + 1 < value.size() && value[i + 1] == '{') {
            ++i;
            continue;
        }
        const size_t close = value.find('}', i);
        if (close == std::string_view::npos) {
            error = "a { that doesn't close (write {{ for a brace)";
            return false;
        }
        const std::string name(value.substr(i + 1, close - i - 1));
        if (!keyPart(name)) {
            error = "{" + name + "} isn't a placeholder name (lowercase letters, digits and _)";
            return false;
        }
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        i = close;
    }
    return true;
}

const std::string* TextCatalog::find(std::string_view key) const {
    for (const auto& [k, v] : entries)
        if (k == key) return &v;
    return nullptr;
}

bool parseTextCatalog(std::string_view text, const std::string& shown, TextCatalog& out, std::string& error) {
    TextCatalog result;
    result.locale = out.locale;
    std::istringstream in{std::string(text)};
    std::string line;
    int number = 0;
    bool header = false;
    auto fail = [&](const std::string& message) {
        error = shown + ":" + std::to_string(number) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (number == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);   // a BOM
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        const size_t keyEnd = line.find_first_of(" \t", start);
        const std::string key = line.substr(start, keyEnd == std::string::npos ? std::string::npos : keyEnd - start);
        const size_t valueStart =
            keyEnd == std::string::npos ? std::string::npos : line.find_first_not_of(" \t", keyEnd);
        if (!header) {
            const std::string version =
                valueStart == std::string::npos
                    ? std::string()
                    : line.substr(valueStart, line.find_first_of(" \t#", valueStart) - valueStart);
            if (key != "swstrings") return fail("expected swstrings 1");
            if (version != "1") return fail("version " + version + " isn't supported (this engine reads swstrings 1)");
            header = true;
            continue;
        }
        if (!validTextKey(key))
            return fail("\"" + key + "\" isn't a text key: lowercase parts joined by dots, like menu.new_game");
        if (valueStart == std::string::npos || line[valueStart] != '"')
            return fail(key + ": the text goes in quotes, like " + key + " \"New game\"");
        // The value: quoted, with \" \\ and \n escapes.
        std::string value;
        size_t i = valueStart + 1;
        bool closed = false;
        for (; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '\\' && i + 1 < line.size()) {
                const char e = line[++i];
                if (e == 'n')
                    value += '\n';
                else if (e == '"' || e == '\\')
                    value += e;
                else
                    return fail(key + ": \\" + std::string(1, e) + " isn't an escape (use \\n, \\\" or \\\\)");
            } else if (c == '"') {
                closed = true;
                ++i;
                break;
            } else {
                value += c;
            }
        }
        if (!closed) return fail(key + ": the quote doesn't close");
        const size_t after = line.find_first_not_of(" \t", i);
        if (after != std::string::npos && line[after] != '#') return fail(key + ": something after the text");
        if (!validUtf8(value)) return fail(key + ": the text isn't valid UTF-8");
        if (result.find(key)) return fail("duplicate key " + key);
        std::vector<std::string> names;
        std::string why;
        if (!textPlaceholders(value, names, why)) return fail(key + ": " + why);
        result.entries.emplace_back(key, std::move(value));
        result.lines[key] = number;
    }
    if (!header) {
        number = std::max(number, 1);
        return fail("expected swstrings 1");
    }
    out = std::move(result);
    error.clear();
    return true;
}

// ---- Localization -------------------------------------------------------------------------

void Localization::load(const fs::path& folder, const std::string& defaultLanguage) {
    m_folder = folder;
    m_default = defaultLanguage.empty() ? "en" : defaultLanguage;
    m_catalogs.clear();
    m_problems.clear();
    std::error_code ec;
    std::vector<fs::path> files;
    if (!folder.empty() && fs::is_directory(folder, ec))
        for (const auto& entry : fs::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && entry.path().extension() == ".swstrings") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        TextCatalog catalog;
        catalog.locale = file.stem().string();
        std::string error;
        if (!in || !parseTextCatalog(text.str(), file.string(), catalog, error)) {
            m_problems.push_back(error.empty() ? file.string() + ": can't read it" : error);
            continue;
        }
        m_catalogs.push_back(std::move(catalog));
    }
    ++m_version;
}

void Localization::addBuiltin(const std::string& locale, std::vector<std::pair<std::string, std::string>> entries) {
    for (auto& catalog : m_builtin)
        if (catalog.locale == locale) {
            for (auto& [key, value] : entries)
                if (!catalog.find(key)) catalog.entries.emplace_back(std::move(key), std::move(value));
            ++m_version;
            return;
        }
    TextCatalog catalog;
    catalog.locale = locale;
    catalog.entries = std::move(entries);
    m_builtin.push_back(std::move(catalog));
    ++m_version;
}

void Localization::setLocale(const std::string& locale) {
    if (locale == m_locale) return;
    m_locale = locale;
    ++m_version;
}

std::vector<std::string> Localization::locales() const {
    std::set<std::string> found;
    for (const auto& catalog : m_catalogs) found.insert(catalog.locale);
    if (found.empty()) found.insert(m_default);
    return {found.begin(), found.end()};
}

const TextCatalog* Localization::catalog(std::string_view locale) const {
    for (const auto& catalog : m_catalogs)
        if (catalog.locale == locale) return &catalog;
    return nullptr;
}

std::vector<const TextCatalog*> Localization::chain() const {
    std::vector<const TextCatalog*> out;
    auto add = [&](const TextCatalog* c) {
        if (c && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    };
    const std::string wanted = m_locale.empty() ? m_default : m_locale;
    add(catalog(wanted));
    add(catalog(languageOf(wanted)));
    add(catalog(m_default));
    add(catalog(languageOf(m_default)));
    // The engine's own text: the locale's, then English.
    for (const std::string& locale : {wanted, languageOf(wanted), std::string("en")})
        for (const auto& b : m_builtin)
            if (b.locale == locale) add(&b);
    return out;
}

bool Localization::has(std::string_view key) const {
    for (const TextCatalog* c : chain())
        if (c->find(key)) return true;
    return false;
}

std::string Localization::text(std::string_view key) const {
    for (const TextCatalog* c : chain())
        if (const std::string* value = c->find(key)) return *value;
    return "[missing: " + std::string(key) + "]";
}

std::string
Localization::format(std::string_view key, const std::vector<std::pair<std::string, std::string>>& values) const {
    const std::string pattern = text(key);
    std::string out;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if ((c == '{' || c == '}') && i + 1 < pattern.size() && pattern[i + 1] == c) {
            out += c;
            ++i;
            continue;
        }
        if (c == '{') {
            const size_t close = pattern.find('}', i);
            if (close != std::string::npos) {
                const std::string name = pattern.substr(i + 1, close - i - 1);
                const auto it =
                    std::find_if(values.begin(), values.end(), [&](const auto& v) { return v.first == name; });
                if (it != values.end()) {
                    out += it->second;
                    i = close;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> Localization::all() const {
    std::set<std::string> keys;
    for (const auto& c : m_catalogs)
        for (const auto& [k, v] : c.entries) keys.insert(k);
    for (const auto& c : m_builtin)
        for (const auto& [k, v] : c.entries) keys.insert(k);
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& key : keys) out.emplace_back(key, text(key));
    return out;
}

std::vector<std::string> Localization::check() const {
    std::vector<std::string> out = m_problems;
    const TextCatalog* base = catalog(m_default);
    auto where = [&](const TextCatalog& c, const std::string& key) {
        const auto line = c.lines.find(key);
        return (m_folder / (c.locale + ".swstrings")).string() +
               (line != c.lines.end() ? ":" + std::to_string(line->second) : "");
    };
    if (!m_catalogs.empty() && !base) {
        out.push_back(
            (m_folder / (m_default + ".swstrings")).string() + ": the default language \"" + m_default +
            "\" has no catalog, so text missing from a locale has nothing to fall back to");
        return out;
    }
    for (const auto& c : m_catalogs) {
        if (&c == base) continue;
        for (const auto& [key, value] : c.entries) {
            const std::string* original = base->find(key);
            if (!original) {
                out.push_back(
                    where(c, key) + ": " + key + " isn't in " + m_default + ".swstrings, the default language");
                continue;
            }
            std::vector<std::string> mine, theirs;
            std::string ignored;
            textPlaceholders(value, mine, ignored);
            textPlaceholders(*original, theirs, ignored);
            std::sort(mine.begin(), mine.end());
            std::sort(theirs.begin(), theirs.end());
            if (mine != theirs) {
                auto list = [](const std::vector<std::string>& names) {
                    std::string text;
                    for (const auto& n : names) text += (text.empty() ? "{" : ", {") + n + "}";
                    return text.empty() ? std::string("none") : text;
                };
                out.push_back(
                    where(c, key) + ": " + key + " has placeholders " + list(mine) + ", but " + m_default + " has " +
                    list(theirs));
            }
        }
        size_t missing = 0;
        std::string first;
        for (const auto& [key, value] : base->entries)
            if (!c.find(key)) {
                if (missing++ == 0) first = key;
            }
        if (missing)
            out.push_back(
                "warning: " + (m_folder / (c.locale + ".swstrings")).string() + ": " + std::to_string(missing) +
                " key" + (missing == 1 ? "" : "s") + " not translated yet (" + first + (missing > 1 ? ", ..." : "") +
                "); they show in " + m_default);
    }
    return out;
}

}   // namespace shockwave
