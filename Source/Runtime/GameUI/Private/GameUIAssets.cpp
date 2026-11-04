// What RmlUi documents and stylesheets refer to, for the asset graph (S04,
// docs/AssetResidency.md). A small tokenizer for each format: RML tags and attributes
// (comments and CDATA skipped), and RCSS at-rules, properties and functions (comments and
// strings skipped). Paths are relative to the file, as RmlUi resolves them.
#include <Assets/AssetGraph.h>

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

int lineAt(std::string_view text, size_t at) {
    int line = 1;
    for (size_t i = 0; i < at && i < text.size(); ++i) line += text[i] == '\n';
    return line;
}

// Data bindings and generated values can't be followed statically.
bool dynamic(std::string_view value) { return value.find("{{") != std::string_view::npos; }

// RCSS: @spritesheet and @font-face give a file with src; image() and url() name one
// (an argument without an extension is a sprite's name).
void readStylesheet(AssetReader& r, std::string_view text, int firstLine) {
    auto line = [&](size_t at) { return firstLine + lineAt(text, at) - 1; };
    auto trim = [](std::string_view v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
        return v;
    };
    // The first token of a value: quoted, or up to a space or comma.
    auto firstToken = [&](std::string_view v) -> std::string {
        v = trim(v);
        if (v.empty()) return {};
        if (v.front() == '"' || v.front() == '\'') {
            const size_t close = v.find(v.front(), 1);
            return std::string(v.substr(1, close == std::string_view::npos ? v.size() - 1 : close - 1));
        }
        size_t end = 0;
        while (end < v.size() && !std::isspace(static_cast<unsigned char>(v[end])) && v[end] != ',') ++end;
        return std::string(v.substr(0, end));
    };
    auto file = [&](const std::string& value, AssetKind kind, const std::string& via, size_t at) {
        if (value.empty() || dynamic(value) || assetKindOf(value) == AssetKind::Other) return;
        r.relative(value, via, line(at), kind);
    };
    std::string atRule;   // the at-rule whose block we're in
    int depth = 0, atDepth = -1;
    size_t prelude = 0;   // where the text since the last { } or ; starts
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 1;
            continue;
        }
        if (c == '"' || c == '\'') {
            const size_t end = text.find(c, i + 1);
            i = end == std::string_view::npos ? text.size() : end;
            continue;
        }
        if (c == '@' && depth == 0) {
            size_t end = i + 1;
            while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '-'))
                ++end;
            atRule = lower(text.substr(i + 1, end - i - 1));
            i = end - 1;
            continue;
        }
        if (c == '{') {
            ++depth;
            if (!atRule.empty() && atDepth < 0) atDepth = depth;
            prelude = i + 1;
            continue;
        }
        if (c == '}') {
            if (depth == atDepth) {
                atDepth = -1;
                atRule.clear();
            }
            depth = depth > 0 ? depth - 1 : 0;
            prelude = i + 1;
            continue;
        }
        if (c == ';') {
            prelude = i + 1;
            continue;
        }
        if (c != ':' || depth == 0) continue;
        // A declaration's value runs to ; or }, past strings and parentheses. Reaching {
        // first means it was a selector with a pseudo-class.
        size_t v = i + 1;
        int parens = 0;
        bool selector = false;
        for (; v < text.size(); ++v) {
            const char d = text[v];
            if (d == '"' || d == '\'') {
                const size_t close = text.find(d, v + 1);
                v = close == std::string_view::npos ? text.size() - 1 : close;
            } else if (d == '(') {
                ++parens;
            } else if (d == ')') {
                --parens;
            } else if (parens <= 0 && (d == ';' || d == '}')) {
                break;
            } else if (parens <= 0 && d == '{') {
                selector = true;
                break;
            }
        }
        if (selector) {
            i = v - 1;
            continue;
        }
        const std::string property = lower(trim(text.substr(prelude, i - prelude)));
        const std::string_view value = text.substr(i + 1, v - i - 1);
        const AssetKind kind = atRule == "font-face" ? AssetKind::Font : AssetKind::Texture;
        // Functions in the value: image(path flip-horizontal), url(path).
        for (size_t f = 0; f < value.size(); ++f) {
            if (value[f] == '"' || value[f] == '\'') {
                const size_t close = value.find(value[f], f + 1);
                f = close == std::string_view::npos ? value.size() : close;
                continue;
            }
            if (value[f] != '(') continue;
            size_t n = f;
            while (n > 0 && (std::isalnum(static_cast<unsigned char>(value[n - 1])) || value[n - 1] == '-')) --n;
            const std::string fn = lower(value.substr(n, f - n));
            const size_t close = value.find(')', f + 1);
            const size_t stop = close == std::string_view::npos ? value.size() : close;
            if (fn == "image" || fn == "url")
                file(firstToken(value.substr(f + 1, stop - f - 1)), kind, property + ": " + fn + "()", i);
            f = stop;
        }
        if (property == "src" && atDepth >= 0 && value.find('(') == std::string_view::npos)
            file(firstToken(value), kind, "@" + atRule + " src", i);
        i = v < text.size() && text[v] == '}' ? v - 1 : v;
        prelude = v + 1;
    }
}

struct Attribute {
    std::string name, value;
};

// RML: the tags with a file in an attribute, and <style> blocks.
bool readDocument(AssetReader& r, std::string_view text, std::string& error) {
    size_t i = 0;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        if (text.compare(i, 4, "<!--") == 0) {
            const size_t end = text.find("-->", i + 4);
            if (end == std::string_view::npos) {
                error = r.id() + ":" + std::to_string(lineAt(text, i)) + ": a comment that never ends";
                return false;
            }
            i = end + 3;
            continue;
        }
        if (text.compare(i, 9, "<![CDATA[") == 0) {
            const size_t end = text.find("]]>", i + 9);
            i = end == std::string_view::npos ? text.size() : end + 3;
            continue;
        }
        if (i + 1 < text.size() && (text[i + 1] == '?' || text[i + 1] == '!' || text[i + 1] == '/')) {
            const size_t end = text.find('>', i);
            i = end == std::string_view::npos ? text.size() : end + 1;
            continue;
        }
        const size_t start = i;
        size_t p = i + 1;
        while (p < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[p])) || text[p] == '-' || text[p] == '_'))
            ++p;
        const std::string tag = lower(text.substr(i + 1, p - i - 1));
        std::vector<Attribute> attributes;
        // Attributes until > (quoted values may hold >).
        while (p < text.size() && text[p] != '>') {
            if (std::isspace(static_cast<unsigned char>(text[p])) || text[p] == '/') {
                ++p;
                continue;
            }
            size_t n = p;
            while (n < text.size() && text[n] != '=' && text[n] != '>' && text[n] != '/' &&
                   !std::isspace(static_cast<unsigned char>(text[n])))
                ++n;
            Attribute a{lower(text.substr(p, n - p)), {}};
            while (n < text.size() && std::isspace(static_cast<unsigned char>(text[n]))) ++n;
            if (n < text.size() && text[n] == '=') {
                ++n;
                while (n < text.size() && std::isspace(static_cast<unsigned char>(text[n]))) ++n;
                if (n < text.size() && (text[n] == '"' || text[n] == '\'')) {
                    const size_t close = text.find(text[n], n + 1);
                    const size_t stop = close == std::string_view::npos ? text.size() : close;
                    a.value = std::string(text.substr(n + 1, stop - n - 1));
                    n = stop + 1;
                } else {
                    const size_t v = n;
                    while (n < text.size() && text[n] != '>' && !std::isspace(static_cast<unsigned char>(text[n]))) ++n;
                    a.value = std::string(text.substr(v, n - v));
                }
            }
            if (n == p) ++n;
            attributes.push_back(std::move(a));
            p = n;
        }
        const size_t tagEnd = p < text.size() ? p + 1 : text.size();
        const int line = lineAt(text, start);
        auto value = [&](const char* name) -> std::string {
            for (const Attribute& a : attributes)
                if (a.name == name) return a.value;
            return {};
        };
        if (tag == "link") {
            const std::string type = lower(value("type"));
            const std::string href = value("href");
            if (!href.empty() && !dynamic(href)) {
                if (type == "text/rcss" || type == "text/css")
                    r.relative(href, "<link href>", line, AssetKind::Stylesheet);
                else if (type == "text/template")
                    r.relative(href, "<link type=\"text/template\">", line, AssetKind::UIDocument);
            }
        } else if (tag == "img") {
            const std::string src = value("src");
            // A sprite's name has no extension; a file does.
            if (!src.empty() && !dynamic(src) && assetKindOf(src) != AssetKind::Other)
                r.relative(src, "<img src>", line, AssetKind::Texture);
        } else if (tag == "style") {
            const size_t end = text.find("</style", tagEnd);
            const size_t stop = end == std::string_view::npos ? text.size() : end;
            readStylesheet(r, text.substr(tagEnd, stop - tagEnd), line);
            i = stop;
            continue;
        }
        i = tagEnd;
    }
    return true;
}

}   // namespace

void registerGameUIAssetProviders() {
    registerAssetProvider(
        "rml", {".rml"}, [](AssetReader& r, std::string& error) { return readDocument(r, r.text(), error); });
    registerAssetProvider("rcss", {".rcss"}, [](AssetReader& r, std::string&) {
        readStylesheet(r, r.text(), 1);
        return true;
    });
}

}   // namespace shockwave
