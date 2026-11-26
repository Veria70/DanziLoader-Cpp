// SPDX-License-Identifier: GPL-3.0-or-later
// TOML values, as the reference holds its configuration (the `toml`
// crate's `Value` and `Table`; DanziLoader / victory-perf, see NOTICE.md).
// The parser is milestone 7's (`config.rs`); this is the model the modules
// that read a table (`kbm`'s `[controls]`, ...) are written against.
// `parse` (toml.cpp) reproduces `toml::from_str::<toml::Table>` of the
// `toml` crate version 1.2.2 (TOML 1.1) the reference uses, errors included.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace danzi::toml {

struct Value;


struct Table {
    std::vector<std::pair<std::string, Value>> entries;

    const Value* get(std::string_view key) const;

    void set(std::string key, Value value);
    bool empty() const { return entries.empty(); }
};

using Array = std::vector<Value>;


struct Datetime {
    std::string text;
    bool operator==(const Datetime&) const = default;
};

struct Value {
    std::variant<std::string, std::int64_t, double, bool, Datetime, Array, Table> v;

    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_integer() const { return std::holds_alternative<std::int64_t>(v); }
    bool is_float() const { return std::holds_alternative<double>(v); }
    bool is_bool() const { return std::holds_alternative<bool>(v); }
    bool is_datetime() const { return std::holds_alternative<Datetime>(v); }
    bool is_array() const { return std::holds_alternative<Array>(v); }
    bool is_table() const { return std::holds_alternative<Table>(v); }
    const std::string* as_string() const { return std::get_if<std::string>(&v); }
    const std::int64_t* as_integer() const { return std::get_if<std::int64_t>(&v); }
    const double* as_float() const { return std::get_if<double>(&v); }
    const bool* as_bool() const { return std::get_if<bool>(&v); }
    const Array* as_array() const { return std::get_if<Array>(&v); }
    const Table* as_table() const { return std::get_if<Table>(&v); }


    const char* type_str() const;
};

inline const Value* Table::get(std::string_view key) const {
    for (const auto& [k, value] : entries) {
        if (k == key) {
            return &value;
        }
    }
    return nullptr;
}

inline void Table::set(std::string key, Value value) {
    auto at = entries.begin();
    while (at != entries.end() && at->first < key) {
        ++at;
    }
    if (at != entries.end() && at->first == key) {
        at->second = std::move(value);
        return;
    }
    entries.emplace(at, std::move(key), std::move(value));
}

inline const char* Value::type_str() const {
    switch (v.index()) {
    case 0: return "string";
    case 1: return "integer";
    case 2: return "float";
    case 3: return "boolean";
    case 4: return "datetime";
    case 5: return "array";
    default: return "table";
    }
}


struct ParseError {


    std::string message;


    std::size_t line = 0;
    std::size_t column = 0;

    bool has_span = false;
    std::size_t span_start = 0;
    std::size_t span_end = 0;
};


std::variant<Table, ParseError> parse(std::string_view text);


std::string display(const ParseError& e, std::string_view text);


struct Field;
struct Node {
    enum class Kind : std::uint8_t { String, Integer, Float, Boolean, Datetime, Array, Table };
    Kind kind = Kind::Table;


    std::size_t start = 0;
    std::size_t end = 0;


    std::string text;
    std::uint32_t radix = 10;
    bool boolean = false;
    std::vector<Node> items;     
    std::vector<Field> fields;   
};
struct Field {
    std::string key;
    std::size_t key_start = 0;   
    std::size_t key_end = 0;
    Node value;
};


std::variant<Node, ParseError> parse_document(std::string_view text);


ParseError error_at(std::string_view text, std::string message, std::size_t start, std::size_t end);


bool unexpected(const Node& n, std::string& text, std::string& error);


std::string to_string(const Value& v);

std::string to_string(const Datetime& d);

}   
