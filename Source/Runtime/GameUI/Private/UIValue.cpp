// UIValue: the values game code shows through {{name}}, their text form (preview values,
// player --ui-preview), and finding the names a document refers to.
#include <GameUI/GameUI.h>

#include "UIText.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace shockwave {

namespace {

bool parseNumber(std::string_view text, double& out) {
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    if (*first == '+') ++first;
    const auto [end, ec] = std::from_chars(first, last, out);
    return ec == std::errc() && end == last && std::isfinite(out);
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

// A recursive-descent reader for the text form.
struct Reader {
    std::string_view text;
    size_t at = 0;
    std::string error;

    void skipSpace() {
        while (at < text.size() && isSpace(text[at])) ++at;
    }
    bool fail(const std::string& message) {
        if (error.empty()) error = message + " at character " + std::to_string(at + 1);
        return false;
    }

    // stops: characters that end a bare word here.
    bool value(UIValue& out, std::string_view stops) {
        skipSpace();
        if (at >= text.size()) {
            out = UIValue(std::string_view());
            return true;
        }
        const char c = text[at];
        if (c == '[') {
            ++at;
            out = UIValue::list();
            skipSpace();
            if (at < text.size() && text[at] == ']') {
                ++at;
                return true;
            }
            for (;;) {
                UIValue item;
                if (!value(item, ",]")) return false;
                out.push(std::move(item));
                skipSpace();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == ']') {
                    ++at;
                    return true;
                }
                return fail("expected ',' or ']' in a list");
            }
        }
        if (c == '{') {
            ++at;
            out = UIValue::record();
            skipSpace();
            if (at < text.size() && text[at] == '}') {
                ++at;
                return true;
            }
            for (;;) {
                skipSpace();
                const size_t start = at;
                while (at < text.size() && text[at] != '=' && text[at] != ',' && text[at] != '}') ++at;
                const std::string_view key = trim(text.substr(start, at - start));
                if (key.empty()) return fail("expected a field name in a record");
                if (at >= text.size() || text[at] != '=') return fail("expected '=' after '" + std::string(key) + "'");
                ++at;
                UIValue field;
                if (!value(field, ",}")) return false;
                out.set(key, std::move(field));
                skipSpace();
                if (at < text.size() && text[at] == ',') {
                    ++at;
                    continue;
                }
                if (at < text.size() && text[at] == '}') {
                    ++at;
                    return true;
                }
                return fail("expected ',' or '}' in a record");
            }
        }
        if (c == '"') {
            ++at;
            std::string s;
            while (at < text.size() && text[at] != '"') {
                if (text[at] == '\\' && at + 1 < text.size()) ++at;
                s += text[at++];
            }
            if (at >= text.size()) return fail("text in quotes has no closing '\"'");
            ++at;
            out = UIValue(std::move(s));
            return true;
        }
        const size_t start = at;
        while (at < text.size() && stops.find(text[at]) == std::string_view::npos) ++at;
        const std::string_view word = trim(text.substr(start, at - start));
        double number = 0.0;
        if (word == "true")
            out = UIValue(true);
        else if (word == "false")
            out = UIValue(false);
        else if (parseNumber(word, number))
            out = UIValue(number);
        else
            out = UIValue(word);
        return true;
    }
};

std::string numberText(double v) {
    char buffer[64];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), v);
    return ec == std::errc() ? std::string(buffer, end) : std::string("0");
}

// Inside a list or record, text that wouldn't read back the same is quoted.
std::string quoted(const std::string& s) {
    double number = 0.0;
    const bool plain = !s.empty() && s == trim(s) && s != "true" && s != "false" && !parseNumber(s, number) &&
                       s.find_first_of("[]{},=;\"\\\n") == std::string::npos;
    if (plain) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string toTextNested(const UIValue& v, bool nested) {
    switch (v.kind()) {
    case UIValue::Kind::Unset: return {};
    case UIValue::Kind::Number: return numberText(v.number());
    case UIValue::Kind::Bool: return v.flag() ? "true" : "false";
    case UIValue::Kind::Text: return nested ? quoted(v.text()) : v.text();
    case UIValue::Kind::List: {
        std::string out = "[";
        for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + toTextNested(v[i], true);
        return out + "]";
    }
    case UIValue::Kind::Record: {
        std::string out = "{";
        for (size_t i = 0; i < v.size(); ++i)
            out += (i ? ", " : "") + v.keys()[i] + "=" + toTextNested(v.items()[i], true);
        return out + "}";
    }
    }
    return {};
}

bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdent(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

// The root names in a data expression: "health < 25 && player.alive", "score | format(2)".
void expressionNames(std::string_view e, const std::vector<std::string>& aliases, std::vector<std::string>& out) {
    char previous = 0;   // the last non-space character before a word
    for (size_t i = 0; i < e.size();) {
        const char c = e[i];
        if (c == '\'' || c == '"') {
            const size_t close = e.find(c, i + 1);
            i = close == std::string_view::npos ? e.size() : close + 1;
            previous = c;
            continue;
        }
        if (isIdentStart(c)) {
            size_t end = i;
            while (end < e.size() && isIdent(e[end])) ++end;
            const std::string word(e.substr(i, end - i));
            size_t next = end;
            while (next < e.size() && isSpace(e[next])) ++next;
            const bool call = next < e.size() && e[next] == '(';
            const bool member = previous == '.' || previous == '|';
            const bool reserved = word == "true" || word == "false" || word == "it" || word == "it_index" ||
                                  word == "ev" || std::find(aliases.begin(), aliases.end(), word) != aliases.end();
            if (!call && !member && !reserved && std::find(out.begin(), out.end(), word) == out.end())
                out.push_back(word);
            previous = 'a';
            i = end;
            continue;
        }
        if (c >= '0' && c <= '9') {   // numbers, including 1.5e3
            while (i < e.size() && (isIdent(e[i]) || e[i] == '.')) ++i;
            previous = '0';
            continue;
        }
        if (!isSpace(c)) previous = c;
        ++i;
    }
}

}   // namespace

UIValue UIValue::list(std::initializer_list<UIValue> items) {
    UIValue v;
    v.m_kind = Kind::List;
    v.m_items.assign(items.begin(), items.end());
    return v;
}

UIValue UIValue::record(std::initializer_list<std::pair<std::string, UIValue>> fields) {
    UIValue v;
    v.m_kind = Kind::Record;
    for (const auto& [key, value] : fields) v.set(key, value);
    return v;
}

std::optional<UIValue> UIValue::parse(std::string_view text, std::string* error) {
    Reader reader{text, 0, {}};
    UIValue out;
    // A bare word at the top runs to the end: "Hello, world" is one piece of text.
    if (!reader.value(out, "")) {
        if (error) *error = reader.error;
        return std::nullopt;
    }
    reader.skipSpace();
    if (reader.at < text.size()) {
        reader.fail("unexpected '" + std::string(1, text[reader.at]) + "'");
        if (error) *error = reader.error;
        return std::nullopt;
    }
    return out;
}

double UIValue::number() const {
    if (m_kind == Kind::Number || m_kind == Kind::Bool) return m_number;
    if (m_kind == Kind::Text) {
        double out = 0.0;
        return parseNumber(trim(m_text), out) ? out : 0.0;
    }
    return 0.0;
}

std::string UIValue::toText() const { return toTextNested(*this, false); }

UIValue& UIValue::push(UIValue item) {
    if (m_kind != Kind::List) *this = list();
    m_items.push_back(std::move(item));
    return *this;
}

UIValue& UIValue::set(std::string_view key, UIValue value) {
    if (m_kind != Kind::Record) *this = record();
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i] == key) {
            m_items[i] = std::move(value);
            return *this;
        }
    }
    m_keys.emplace_back(key);
    m_items.push_back(std::move(value));
    return *this;
}

const UIValue* UIValue::field(std::string_view key) const {
    if (m_kind != Kind::Record) return nullptr;
    for (size_t i = 0; i < m_keys.size(); ++i)
        if (m_keys[i] == key) return &m_items[i];
    return nullptr;
}

bool UIValue::operator==(const UIValue& other) const {
    return m_kind == other.m_kind && m_number == other.m_number && m_text == other.m_text && m_keys == other.m_keys &&
           m_items == other.m_items;
}

std::optional<std::vector<std::pair<std::string, UIValue>>> parseUIPreview(std::string_view text, std::string* error) {
    std::vector<std::pair<std::string, UIValue>> out;
    Reader reader{text, 0, {}};
    for (;;) {
        reader.skipSpace();
        while (reader.at < text.size() && (text[reader.at] == ';' || text[reader.at] == '\n')) {
            ++reader.at;
            reader.skipSpace();
        }
        if (reader.at >= text.size()) break;
        const size_t start = reader.at;
        while (reader.at < text.size() && isIdent(text[reader.at])) ++reader.at;
        const std::string name(text.substr(start, reader.at - start));
        if (name.empty() || !isIdentStart(name[0])) {
            reader.fail("expected a value name");
            break;
        }
        reader.skipSpace();
        if (reader.at >= text.size() || text[reader.at] != '=') {
            reader.fail("expected '=' after '" + name + "'");
            break;
        }
        ++reader.at;
        UIValue value;
        if (!reader.value(value, ";\n")) break;
        while (reader.at < text.size() &&
               (text[reader.at] == ' ' || text[reader.at] == '\t' || text[reader.at] == '\r'))
            ++reader.at;
        if (reader.at < text.size() && text[reader.at] != ';' && text[reader.at] != '\n') {
            reader.fail("expected ';' after the value of '" + name + "'");
            break;
        }
        out.emplace_back(name, std::move(value));
    }
    if (!reader.error.empty()) {
        if (error) *error = reader.error;
        return std::nullopt;
    }
    return out;
}

namespace ui {

std::vector<std::string> referencedNames(std::string_view rml) {
    std::vector<std::string> names, aliases;
    // data-for="item : items" and "item, i : items" name aliases first, so they're skipped.
    std::vector<std::pair<std::string_view, std::string_view>> attributes;   // name, value
    for (size_t at = rml.find("data-"); at != std::string_view::npos; at = rml.find("data-", at + 5)) {
        if (at > 0 && !isSpace(rml[at - 1])) continue;
        size_t end = at;
        while (end < rml.size() && (isIdent(rml[end]) || rml[end] == '-')) ++end;
        const std::string_view attribute = rml.substr(at, end - at);
        while (end < rml.size() && isSpace(rml[end])) ++end;
        if (end >= rml.size() || rml[end] != '=') continue;
        ++end;
        while (end < rml.size() && isSpace(rml[end])) ++end;
        if (end >= rml.size() || (rml[end] != '"' && rml[end] != '\'')) continue;
        const size_t close = rml.find(rml[end], end + 1);
        if (close == std::string_view::npos) break;
        attributes.emplace_back(attribute, rml.substr(end + 1, close - end - 1));
    }
    for (const auto& [attribute, value] : attributes) {
        if (attribute != "data-for") continue;
        const size_t colon = value.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view left = value.substr(0, colon);
        while (!left.empty()) {
            const size_t comma = left.find(',');
            const std::string_view alias = trim(left.substr(0, comma));
            if (!alias.empty()) aliases.emplace_back(alias);
            if (comma == std::string_view::npos) break;
            left.remove_prefix(comma + 1);
        }
    }
    for (const auto& [attribute, value] : attributes) {
        // Not expressions: the model's name, our back command, and event handlers.
        if (attribute == "data-model" || attribute == "data-back" || attribute.rfind("data-event-", 0) == 0) continue;
        if (attribute == "data-for") {
            const size_t colon = value.find(':');
            expressionNames(colon == std::string_view::npos ? value : value.substr(colon + 1), aliases, names);
        } else if (attribute.rfind("data-alias-", 0) != 0) {
            expressionNames(value, aliases, names);
        }
    }
    for (size_t at = rml.find("{{"); at != std::string_view::npos; at = rml.find("{{", at + 2)) {
        const size_t close = rml.find("}}", at + 2);
        if (close == std::string_view::npos) break;
        expressionNames(rml.substr(at + 2, close - at - 2), aliases, names);
    }
    return names;
}

}   // namespace ui
}   // namespace shockwave
