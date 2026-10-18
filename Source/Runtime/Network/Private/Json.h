#pragma once

// A small strict JSON reader and writing helpers, for packet capture dumps. Tool code
// only: it allocates freely and never runs during play.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave::net::json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool boolean = false;
    std::string text;   // a string's contents, or a number exactly as written
    std::vector<Value> items;   // an array's
    std::vector<std::string> keys;   // an object's, in order, with their values
    std::vector<Value> values;

    const Value* find(std::string_view key) const;
};

// Parses a whole document. On failure returns false with `error` saying where.
bool parse(std::string_view text, Value& out, std::string& error);

// Appends `text` as a quoted JSON string.
void appendString(std::string& out, std::string_view text);

}   // namespace shockwave::net::json
