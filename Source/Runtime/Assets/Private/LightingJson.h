#pragma once

// Helpers shared by the baked lighting and reflection probe files (LightingData.cpp,
// ReflectionData.cpp): the manifests' line tokenizer, and a small strict JSON reader for
// the export/import tools. Private to Assets.

#include <Assets/LightingData.h>

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shockwave::lighting_json {

// Splits a line into words; "quoted strings" may hold spaces, \" and \\.
inline bool tokenize(std::string_view line, std::vector<std::string>& words, std::string& error) {
    words.clear();
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') {
            ++i;
            continue;
        }
        if (line[i] == '#') break;
        std::string word;
        if (line[i] == '"') {
            ++i;
            bool closed = false;
            while (i < line.size()) {
                const char c = line[i++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (i >= line.size() || (line[i] != '"' && line[i] != '\\')) {
                        error = "only \\\" and \\\\ may follow a backslash";
                        return false;
                    }
                    word += line[i++];
                    continue;
                }
                word += c;
            }
            if (!closed) {
                error = "a quoted string isn't closed";
                return false;
            }
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') word += line[i++];
        }
        words.push_back(std::move(word));
    }
    return true;
}
inline std::string quotedText(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}
inline bool u32Value(const std::string& text, uint32_t& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

inline std::string number(float v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", double(v));
    return text;
}

// A small strict JSON reader: objects, arrays, numbers, strings without escapes beyond \" \\.
struct Json {
    enum class Type { Null, Number, String, Array, Object, Bool } type = Type::Null;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;
    const Json* field(std::string_view name) const {
        for (const auto& [k, v] : fields)
            if (k == name) return &v;
        return nullptr;
    }
};
struct JsonParser {
    std::string_view s;
    size_t i = 0;
    std::string error;
    int depth = 0;
    void space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
    }
    bool fail(const std::string& message) {
        if (error.empty()) error = "JSON at byte " + std::to_string(i) + ": " + message;
        return false;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                if (++i >= s.size() || (s[i] != '"' && s[i] != '\\')) return fail("unsupported escape");
            }
            out += s[i++];
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i;
        return true;
    }
    bool value(Json& out) {
        if (++depth > 64) return fail("nested too deeply");
        space();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        bool ok = true;
        if (c == '{') {
            out.type = Json::Type::Object;
            ++i;
            space();
            if (i < s.size() && s[i] == '}') {
                ++i;
            } else {
                while (ok) {
                    space();
                    std::string key;
                    if (!string(key)) return false;
                    space();
                    if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                    ++i;
                    Json v;
                    if (!value(v)) return false;
                    out.fields.push_back({std::move(key), std::move(v)});
                    space();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == '}') {
                        ++i;
                        break;
                    }
                    return fail("expected ',' or '}'");
                }
            }
        } else if (c == '[') {
            out.type = Json::Type::Array;
            ++i;
            space();
            if (i < s.size() && s[i] == ']') {
                ++i;
            } else {
                while (true) {
                    Json v;
                    if (!value(v)) return false;
                    out.items.push_back(std::move(v));
                    space();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == ']') {
                        ++i;
                        break;
                    }
                    return fail("expected ',' or ']'");
                }
            }
        } else if (c == '"') {
            out.type = Json::Type::String;
            if (!string(out.text)) return false;
        } else if (s.substr(i, 4) == "true" || s.substr(i, 5) == "false") {
            out.type = Json::Type::Bool;
            out.number = s[i] == 't' ? 1 : 0;
            i += s[i] == 't' ? 4 : 5;
        } else {
            size_t end = i;
            while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '-' ||
                                      s[end] == '+' || s[end] == '.' || s[end] == 'e' || s[end] == 'E'))
                ++end;
            if (end == i) return fail("unexpected character");
            const std::string token(s.substr(i, end - i));
            char* stop = nullptr;
            out.number = std::strtod(token.c_str(), &stop);
            if (stop != token.c_str() + token.size() || !std::isfinite(out.number)) return fail("bad number");
            out.type = Json::Type::Number;
            i = end;
        }
        --depth;
        return ok;
    }
};
inline bool parseJson(std::string_view text, Json& out, std::string& error) {
    JsonParser p{text, 0, {}, 0};
    if (!p.value(out)) {
        error = p.error;
        return false;
    }
    p.space();
    if (p.i != text.size()) {
        error = "JSON has text after its end";
        return false;
    }
    return true;
}
inline bool numbers(const Json* j, std::vector<double>& out, size_t expected, std::string& error, const char* what) {
    if (!j || j->type != Json::Type::Array) {
        error = std::string(what) + " must be an array";
        return false;
    }
    if (expected != SIZE_MAX && j->items.size() != expected) {
        error = std::string(what) + " must have " + std::to_string(expected) + " numbers";
        return false;
    }
    out.clear();
    for (const Json& item : j->items) {
        if (item.type != Json::Type::Number) {
            error = std::string(what) + " must hold numbers";
            return false;
        }
        out.push_back(item.number);
    }
    return true;
}
inline bool idField(const Json& j, const char* name, BindingId& id, std::string& error) {
    const Json* f = j.field(name);
    if (!f || f->type != Json::Type::String || !parseBindingId(f->text, id)) {
        error = std::string(name) + " must be a 32-digit binding id";
        return false;
    }
    return true;
}
inline bool hashField(const Json& j, const char* name, uint64_t& hash, std::string& error) {
    const Json* f = j.field(name);
    if (!f || f->type != Json::Type::String || !parseLightingHash(f->text, hash)) {
        error = std::string(name) + " must be 16 hex digits";
        return false;
    }
    return true;
}
inline bool uintField(const Json& j, const char* name, uint32_t& v, std::string& error) {
    const Json* f = j.field(name);
    if (!f || f->type != Json::Type::Number || f->number < 0 || f->number > double(UINT32_MAX) ||
        f->number != std::floor(f->number)) {
        error = std::string(name) + " must be a whole number";
        return false;
    }
    v = uint32_t(f->number);
    return true;
}

}   // namespace shockwave::lighting_json
