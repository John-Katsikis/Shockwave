#include "Json.h"

#include <format>

namespace shockwave::net::json {

const Value* Value::find(std::string_view key) const {
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return &values[i];
    return nullptr;
}

namespace {

class Parser {
public:
    Parser(std::string_view text, std::string& error) : m_text(text), m_error(error) {}

    bool document(Value& out) {
        if (!value(out, 0)) return false;
        skipSpace();
        if (m_at != m_text.size()) return fail("unexpected text after the document");
        return true;
    }

private:
    bool fail(const char* message) {
        size_t line = 1;
        for (size_t i = 0; i < m_at && i < m_text.size(); ++i)
            if (m_text[i] == '\n') ++line;
        m_error = std::format("line {}: {}", line, message);
        return false;
    }
    void skipSpace() {
        while (m_at < m_text.size() &&
               (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r'))
            ++m_at;
    }
    bool literal(std::string_view word) {
        if (m_text.substr(m_at, word.size()) != word) return false;
        m_at += word.size();
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > 32) return fail("nested too deeply");
        skipSpace();
        if (m_at >= m_text.size()) return fail("unexpected end");
        const char c = m_text[m_at];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.type = Value::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.type = Value::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.type = Value::Bool;
            return true;
        }
        if (literal("null")) {
            out.type = Value::Null;
            return true;
        }
        return number(out);
    }
    bool object(Value& out, int depth) {
        out.type = Value::Object;
        ++m_at;
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == '}') {
            ++m_at;
            return true;
        }
        for (;;) {
            skipSpace();
            std::string key;
            if (m_at >= m_text.size() || m_text[m_at] != '"') return fail("expected a key");
            if (!string(key)) return false;
            if (out.find(key)) return fail("duplicate key");
            skipSpace();
            if (m_at >= m_text.size() || m_text[m_at] != ':') return fail("expected ':'");
            ++m_at;
            Value member;
            if (!value(member, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.values.push_back(std::move(member));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == '}') {
                ++m_at;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }
    bool array(Value& out, int depth) {
        out.type = Value::Array;
        ++m_at;
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == ']') {
            ++m_at;
            return true;
        }
        for (;;) {
            Value item;
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skipSpace();
            if (m_at < m_text.size() && m_text[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_text.size() && m_text[m_at] == ']') {
                ++m_at;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }
    bool number(Value& out) {
        const size_t start = m_at;
        if (m_at < m_text.size() && m_text[m_at] == '-') ++m_at;
        auto digits = [&] {
            const size_t from = m_at;
            while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9') ++m_at;
            return m_at > from;
        };
        if (!digits()) return fail("expected a value");
        if (m_at < m_text.size() && m_text[m_at] == '.' && (++m_at, !digits())) return fail("bad number");
        if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E')) {
            ++m_at;
            if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-')) ++m_at;
            if (!digits()) return fail("bad number");
        }
        out.type = Value::Number;
        out.text = std::string(m_text.substr(start, m_at - start));
        return true;
    }
    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& out) {
        if (m_text.size() - m_at < 4) return fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_at++];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= uint32_t(c - 'A' + 10);
            else
                return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string& out) {
        ++m_at;   // opening quote
        for (;;) {
            if (m_at >= m_text.size()) return fail("unterminated string");
            const char c = m_text[m_at++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_at >= m_text.size()) return fail("unterminated string");
            const char e = m_text[m_at++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low;
                    if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low > 0xDFFF)
                        return fail("unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }

    std::string_view m_text;
    std::string& m_error;
    size_t m_at = 0;
};

}   // namespace

bool parse(std::string_view text, Value& out, std::string& error) {
    out = {};
    return Parser(text, error).document(out);
}

void appendString(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += std::format("\\u{:04x}", int(c));
            else
                out += c;
        }
    }
    out += '"';
}

}   // namespace shockwave::net::json
