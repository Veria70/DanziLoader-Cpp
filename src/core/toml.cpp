// SPDX-License-Identifier: GPL-3.0-or-later
// A TOML parser that reproduces the behaviour of the `toml` crate version
// 1.2.2 (TOML 1.1; with `toml_parser` 1.1.6, `toml_datetime` 1.1.3 and
// `toml_writer` 1.2.0, default features) used by the reference
// (DanziLoader / victory-perf, see NOTICE.md): `toml::from_str::<Table>`,
// the `Display` of its errors and of `toml::Value`. Written for this port;
// the crate (MIT OR Apache-2.0) served as the behaviour reference only.
//
// The crate works in stages, and the first error any stage reports is the
// one returned, so the stages are kept apart here too:
//   1. lex the text into tokens;
//   2. parse the tokens into events, reporting grammar errors and invalid
//      comment / newline bytes as they are met;
//   3. build tables from the events, decoding keys and values (escapes,
//      numbers, dates) and checking table rules, in document order;
//   4. convert to `Value`s in key order: integers outside i64 and floats
//      that overflow fail only here.
// Every error is final, so reporting one throws.

#include "core/toml.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>

namespace danzi::toml {
namespace {


struct Span {
    std::size_t start = 0;
    std::size_t end = 0;
};
Span before(Span s) { return {s.start, s.start}; }
Span after(Span s) { return {s.end, s.end}; }

struct Expected {
    bool literal;
    std::string_view text;
};
Expected lit(std::string_view s) { return {true, s}; }
Expected desc(std::string_view s) { return {false, s}; }


struct Failure {
    std::string message;
    bool has_span = false;
    Span span;
};

std::string render(const Expected& e) {
    if (!e.literal) {
        return std::string(e.text);
    }
    if (e.text == "\n") {
        return "newline";
    }
    if (e.text == "`") {
        return "'`'";
    }
    return "`" + std::string(e.text) + "`";
}


[[noreturn]] void fail(std::string_view description, std::initializer_list<Expected> expected,
                       bool has_expected, std::optional<Span> unexpected) {
    Failure f;
    f.message = std::string(description);
    if (has_expected) {
        f.message += ", expected ";
        if (expected.size() == 0) {
            f.message += "nothing";
        }
        bool first = true;
        for (const auto& e : expected) {
            if (!first) {
                f.message += ", ";
            }
            first = false;
            f.message += render(e);
        }
    }
    if (unexpected) {
        f.has_span = true;
        f.span = *unexpected;
    }
    throw f;
}
[[noreturn]] void fail(std::string_view description, std::initializer_list<Expected> expected,
                       Span unexpected) {
    fail(description, expected, true, unexpected);
}
[[noreturn]] void fail_plain(std::string_view description, Span unexpected) {
    fail(description, {}, false, unexpected);
}


struct Utf8Check {
    bool ok = true;
    std::size_t valid_up_to = 0;
    int error_len = 0;   
};


Utf8Check check_utf8(std::string_view s) {
    const auto* b = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto bad = [&](int len) {
        Utf8Check r;
        r.ok = false;
        r.valid_up_to = i;
        r.error_len = len;
        return r;
    };
    while (i < n) {
        const unsigned c = b[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        int width = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            width = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            width = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            width = 4;
        } else {
            return bad(1);
        }
        auto at = [&](std::size_t k) -> int { return i + k < n ? b[i + k] : -1; };
        auto cont = [](int x) { return x >= 0x80 && x <= 0xBF; };
        int c1 = at(1);
        if (c1 < 0) {
            return bad(0);
        }
        if (width == 2) {
            if (!cont(c1)) {
                return bad(1);
            }
        } else if (width == 3) {
            const bool ok1 = (c == 0xE0 && c1 >= 0xA0 && c1 <= 0xBF) ||
                             (c >= 0xE1 && c <= 0xEC && cont(c1)) ||
                             (c == 0xED && c1 >= 0x80 && c1 <= 0x9F) ||
                             (c >= 0xEE && c <= 0xEF && cont(c1));
            if (!ok1) {
                return bad(1);
            }
            const int c2 = at(2);
            if (c2 < 0) {
                return bad(0);
            }
            if (!cont(c2)) {
                return bad(2);
            }
        } else {
            const bool ok1 = (c == 0xF0 && c1 >= 0x90 && c1 <= 0xBF) ||
                             (c >= 0xF1 && c <= 0xF3 && cont(c1)) ||
                             (c == 0xF4 && c1 >= 0x80 && c1 <= 0x8F);
            if (!ok1) {
                return bad(1);
            }
            const int c2 = at(2);
            if (c2 < 0) {
                return bad(0);
            }
            if (!cont(c2)) {
                return bad(2);
            }
            const int c3 = at(3);
            if (c3 < 0) {
                return bad(0);
            }
            if (!cont(c3)) {
                return bad(3);
            }
        }
        i += static_cast<std::size_t>(width);
    }
    return {};
}

std::size_t count_chars(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

void push_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}


std::pair<std::size_t, std::size_t> translate_position(std::string_view input, std::size_t index) {
    if (input.empty()) {
        return {0, index};
    }
    const std::size_t safe = std::min(index, input.size() - 1);
    const std::size_t column_offset = index - safe;
    index = safe;
    std::size_t line_start = 0;
    for (std::size_t k = index; k > 0; --k) {
        if (input[k - 1] == '\n') {
            line_start = k;
            break;
        }
    }
    std::size_t line = 0;
    for (std::size_t k = 0; k < line_start; ++k) {
        if (input[k] == '\n') {
            ++line;
        }
    }
    const std::string_view upto = input.substr(line_start, index - line_start + 1);
    std::size_t column = check_utf8(upto).ok ? count_chars(upto) - 1 : index - line_start;
    return {line, column + column_offset};
}


enum class Tok : std::uint8_t {
    Dot,
    Equals,
    Comma,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Whitespace,
    Comment,
    Newline,
    LiteralString,
    BasicString,
    MlLiteralString,
    MlBasicString,
    Atom,
    Eof,
};

enum class Enc : std::uint8_t { None, Literal, Basic, MlLiteral, MlBasic };

Enc encoding_of(Tok t) {
    switch (t) {
    case Tok::LiteralString: return Enc::Literal;
    case Tok::BasicString: return Enc::Basic;
    case Tok::MlLiteralString: return Enc::MlLiteral;
    case Tok::MlBasicString: return Enc::MlBasic;
    default: return Enc::None;
    }
}

struct Token {
    Tok kind;
    Span span;
};

bool atom_end(unsigned char c) {
    switch (c) {
    case '.': case '=': case ',': case '[': case ']': case '{': case '}':
    case ' ': case '\t': case '#': case '\r': case '\n':
        return true;
    default:
        return false;
    }
}

std::vector<Token> lex(std::string_view in) {
    std::vector<Token> out;
    const std::size_t n = in.size();
    std::size_t i = 0;
    if (in.substr(0, 3) == "\xEF\xBB\xBF") {
        i = 3;
    }
    auto starts = [&](std::size_t at, std::string_view s) { return in.substr(at, s.size()) == s; };
    while (i < n) {
        const std::size_t start = i;
        const char c = in[i];
        Tok kind = Tok::Atom;
        switch (c) {
        case '.': kind = Tok::Dot; ++i; break;
        case '=': kind = Tok::Equals; ++i; break;
        case ',': kind = Tok::Comma; ++i; break;
        case '[': kind = Tok::LBracket; ++i; break;
        case ']': kind = Tok::RBracket; ++i; break;
        case '{': kind = Tok::LBrace; ++i; break;
        case '}': kind = Tok::RBrace; ++i; break;
        case ' ':
        case '\t':
            kind = Tok::Whitespace;
            while (i < n && (in[i] == ' ' || in[i] == '\t')) {
                ++i;
            }
            break;
        case '#':
            kind = Tok::Comment;
            while (i < n && in[i] != '\r' && in[i] != '\n') {
                ++i;
            }
            break;
        case '\r':
            kind = Tok::Newline;
            ++i;
            if (i < n && in[i] == '\n') {
                ++i;
            }
            break;
        case '\n': kind = Tok::Newline; ++i; break;
        case '\'':
            if (starts(i, "'''")) {
                kind = Tok::MlLiteralString;
                i += 3;
                const std::size_t p = in.find("'''", i);
                i = p == std::string_view::npos ? n : p + 3;
                if (i < n && in[i] == '\'') {
                    ++i;
                    if (i < n && in[i] == '\'') {
                        ++i;
                    }
                }
            } else {
                kind = Tok::LiteralString;
                ++i;
                const std::size_t p = in.find_first_of("'\n", i);
                if (p == std::string_view::npos) {
                    i = n;
                } else {
                    i = in[p] == '\'' ? p + 1 : p;
                }
            }
            break;
        case '"':
            if (starts(i, "\"\"\"")) {
                kind = Tok::MlBasicString;
                i += 3;
                for (;;) {

                    std::size_t p = i;
                    while (p < n && in[p] != '\\' && !starts(p, "\"\"\"")) {
                        ++p;
                    }
                    if (p >= n) {
                        i = n;
                        break;
                    }
                    if (in[p] == '"') {
                        i = p + 3;
                        break;
                    }
                    i = p + 1;
                    if (i < n && (in[i] == '\\' || in[i] == '"')) {
                        ++i;
                    }
                }
                if (i < n && in[i] == '"') {
                    ++i;
                    if (i < n && in[i] == '"') {
                        ++i;
                    }
                }
            } else {
                kind = Tok::BasicString;
                ++i;
                for (;;) {
                    const std::size_t p = in.find_first_of("\"\\\n", i);
                    if (p == std::string_view::npos) {
                        i = n;
                        break;
                    }
                    if (in[p] == '"') {
                        i = p + 1;
                        break;
                    }
                    if (in[p] == '\\') {
                        i = p + 1;
                        if (i < n && (in[i] == '\\' || in[i] == '"')) {
                            ++i;
                        }
                        continue;
                    }
                    i = p;   
                    break;
                }
            }
            break;
        default:
            kind = Tok::Atom;
            while (i < n && !atom_end(static_cast<unsigned char>(in[i]))) {
                ++i;
            }
            break;
        }
        out.push_back({kind, {start, i}});
    }
    out.push_back({Tok::Eof, {i, i}});
    return out;
}


enum class Ev : std::uint8_t {
    StdTableOpen,
    StdTableClose,
    ArrayTableOpen,
    ArrayTableClose,
    InlineTableOpen,
    InlineTableClose,
    ArrayOpen,
    ArrayClose,
    SimpleKey,
    KeySep,
    KeyValSep,
    Scalar,
    ValueSep,
    Whitespace,
    Comment,
    Newline,
    Error,
};

struct Event {
    Ev kind;
    Enc enc;
    Span span;
};


constexpr std::int64_t kDepthLimit = 80;

bool non_eol(unsigned char b) { return b == 0x09 || (b >= 0x20 && b <= 0x7E) || b >= 0x80; }


class Parser {
public:
    Parser(std::string_view text, const std::vector<Token>& toks, std::vector<Event>& events)
        : text_(text), toks_(toks), ev_(events) {}

    void run() {
        document();
        eof();
    }

private:
    std::string_view text_;
    const std::vector<Token>& toks_;
    std::vector<Event>& ev_;
    std::size_t pos_ = 0;
    std::int64_t depth_ = 0;

    const Token* next() { return pos_ < toks_.size() ? &toks_[pos_++] : nullptr; }
    const Token* first() const { return pos_ < toks_.size() ? &toks_[pos_] : nullptr; }
    const Token* peek(std::size_t k) const {
        return pos_ + k < toks_.size() ? &toks_[pos_ + k] : nullptr;
    }
    const Token* next_if(Tok k) {
        const Token* t = first();
        return t && t->kind == k ? next() : nullptr;
    }
    void seek_back() { pos_ = pos_ == 0 ? toks_.size() : pos_ - 1; }


    Span previous_span() const {
        for (std::size_t k = pos_; k > 0; --k) {
            const Token& t = toks_[k - 1];
            if (t.kind != Tok::Whitespace && t.kind != Tok::Comment && t.kind != Tok::Newline &&
                t.kind != Tok::Eof) {
                return t.span;
            }
        }
        return {};
    }


    void emit(Ev k, Span s, Enc e = Enc::None) { ev_.push_back({k, e, s}); }
    void whitespace(Span s) { emit(Ev::Whitespace, s); }
    void comment(Span s) {
        const std::string_view raw = text_.substr(s.start, s.end - s.start);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (!non_eol(static_cast<unsigned char>(raw[i]))) {
                fail("invalid comment character", {desc("printable characters")},
                     Span{s.start + i, s.start + i});
            }
        }
        emit(Ev::Comment, s);
    }
    void newline(Span s) {
        if (s.end - s.start == 1 && text_[s.start] == '\r') {
            fail("carriage return must be followed by newline", {lit("\n")}, Span{s.end, s.end});
        }
        emit(Ev::Newline, s);
    }
    void open_nested(Ev k, Span s) {
        emit(k, s);
        ++depth_;
        if (depth_ > kDepthLimit) {
            fail_plain("cannot recurse further; max recursion depth met", s);
        }
    }
    void close_nested(Ev k, Span s) {
        --depth_;
        emit(k, s);
    }
    void simple_key(Span s, Enc e) { emit(Ev::SimpleKey, s, e); }


    void document() {
        while (const Token* t = next()) {
            switch (t->kind) {
            case Tok::LBracket: on_table(*t); break;
            case Tok::RBracket:
                fail("missing table open", {lit("[")}, before(t->span));
            case Tok::LiteralString:
            case Tok::BasicString:
            case Tok::MlLiteralString:
            case Tok::MlBasicString:
            case Tok::Atom: on_expression_key(*t); break;
            case Tok::Equals:
                simple_key(before(t->span), Enc::None);
                on_expression_key_val_sep(*t);
                break;
            case Tok::Dot: on_expression_dot(*t); break;
            case Tok::Comma:
            case Tok::RBrace:
            case Tok::LBrace:
                fail("invalid key-value pair", {desc("key")}, before(t->span));
            case Tok::Whitespace: whitespace(t->span); break;
            case Tok::Newline: newline(t->span); break;
            case Tok::Comment: on_comment(*t); break;
            case Tok::Eof: return;
            }
        }
    }

    void on_table(const Token& open) {
        bool is_array = false;
        if (const Token* second = next_if(Tok::LBracket)) {
            emit(Ev::ArrayTableOpen, Span{open.span.start, second->span.end});
            is_array = true;
        } else {
            emit(Ev::StdTableOpen, open.span);
        }
        opt_whitespace();
        const bool valid_key = key("invalid table");
        opt_whitespace();
        bool success = false;
        if (const Token* close = next_if(Tok::RBracket)) {
            if (is_array) {
                if (const Token* second = next_if(Tok::RBracket)) {
                    emit(Ev::ArrayTableClose, Span{close->span.start, second->span.end});
                    success = true;
                } else {
                    fail("unclosed array table", {lit("]")}, after(close->span));
                }
            } else {
                emit(Ev::StdTableClose, close->span);
                success = true;
            }
        } else if (valid_key) {
            Span last = open.span;
            for (std::size_t k = pos_; k > 0; --k) {
                if (toks_[k - 1].kind != Tok::Whitespace) {
                    last = toks_[k - 1].span;
                    break;
                }
            }
            if (is_array) {
                fail("unclosed array table", {lit("]]")}, after(last));
            }
            fail("unclosed table", {lit("]")}, after(last));
        }
        if (success) {
            ws_comment_newline();
        } else {
            ignore_to_newline();
        }
    }

    bool key(std::string_view invalid_description) {
        while (const Token* t = next()) {
            switch (t->kind) {
            case Tok::RBracket:
            case Tok::Comment:
            case Tok::Equals:
            case Tok::Comma:
            case Tok::LBracket:
            case Tok::LBrace:
            case Tok::RBrace:
            case Tok::Newline:
            case Tok::Eof:
                simple_key(before(t->span), Enc::None);
                seek_back();
                return false;
            case Tok::Whitespace: whitespace(t->span); continue;
            case Tok::Dot:
                simple_key(before(t->span), Enc::None);
                emit(Ev::KeySep, t->span);
                continue;
            default:
                simple_key(t->span, encoding_of(t->kind));
                return opt_dot_keys();
            }
        }
        fail(invalid_description, {desc("key")}, after(previous_span()));
    }

    void on_expression_key(const Token& key_token) {
        simple_key(key_token.span, encoding_of(key_token.kind));
        opt_dot_keys();
        opt_whitespace();
        const Token* eq = next_if(Tok::Equals);
        if (!eq) {
            if (const Token* p = first()) {
                fail("key with no value", {lit("=")}, before(p->span));
            }
            ignore_to_newline();
            return;
        }
        on_expression_key_val_sep(*eq);
    }

    void on_expression_dot(const Token& dot) {
        simple_key(before(dot.span), Enc::None);
        seek_back();
        opt_dot_keys();
        opt_whitespace();
        const Token* eq = next_if(Tok::Equals);
        if (!eq) {
            if (const Token* p = first()) {
                fail("missing value for key", {lit("=")}, before(p->span));
            }
            ignore_to_newline();
            return;
        }
        on_expression_key_val_sep(*eq);
    }

    void on_expression_key_val_sep(const Token& eq) {
        emit(Ev::KeyValSep, eq.span);
        opt_whitespace();
        value();
        ws_comment_newline();
    }

    bool opt_dot_keys() {
        opt_whitespace();
        bool success = true;
        while (const Token* dot = next_if(Tok::Dot)) {
            emit(Ev::KeySep, dot->span);
            bool got_key = false;
            while (const Token* t = next()) {
                bool done = false;
                switch (t->kind) {
                case Tok::Equals:
                case Tok::Comma:
                case Tok::LBracket:
                case Tok::RBracket:
                case Tok::LBrace:
                case Tok::RBrace:
                case Tok::Comment:
                case Tok::Newline:
                case Tok::Eof:
                    simple_key(before(t->span), Enc::None);
                    seek_back();
                    return false;
                case Tok::Whitespace: whitespace(t->span); continue;
                case Tok::Dot:
                    simple_key(before(t->span), Enc::None);
                    emit(Ev::KeySep, t->span);
                    continue;
                default:
                    simple_key(t->span, encoding_of(t->kind));
                    opt_whitespace();
                    done = true;
                    break;
                }
                if (done) {
                    got_key = true;
                    break;
                }
            }
            if (!got_key) {
                simple_key(after(dot->span), Enc::None);
            }
        }
        return success;
    }

    void value() {
        const Token* t = nullptr;
        for (;;) {
            t = next();
            if (!t) {
                fail("missing value", {desc("value")}, after(previous_span()));
            }
            if (t->kind != Tok::Equals) {
                break;
            }
            fail("extra `=`", {}, t->span);
        }
        switch (t->kind) {
        case Tok::Comment:
        case Tok::Comma:
        case Tok::Newline:
        case Tok::Eof:
        case Tok::Whitespace:
            emit(Ev::Scalar, before(t->span));
            seek_back();
            break;
        case Tok::LBrace: on_inline_table_open(*t); break;
        case Tok::RBrace:
            fail("missing inline table opening", {lit("{")}, before(t->span));
        case Tok::LBracket: on_array_open(*t); break;
        case Tok::RBracket:
            fail("missing array opening", {lit("[")}, before(t->span));
        default: on_scalar(*t); break;
        }
    }

    void on_scalar(const Token& scalar) {
        Span span = scalar.span;
        Enc enc = encoding_of(scalar.kind);
        if (scalar.kind == Tok::Dot || scalar.kind == Tok::Atom) {


            while (const Token* t = first()) {
                if (t->kind == Tok::Whitespace) {
                    const Token* second = peek(1);
                    if (second && second->kind == Tok::Atom) {
                        span.end = second->span.end;
                        pos_ += 2;
                        continue;
                    }
                    break;
                }
                if (t->kind == Tok::Dot || t->kind == Tok::Atom) {
                    span.end = t->span.end;
                    ++pos_;
                    continue;
                }
                break;
            }
        }
        emit(Ev::Scalar, span, enc);
    }

    void on_array_open(const Token& open) {
        open_nested(Ev::ArrayOpen, open.span);
        bool needs_comma = false;
        auto check_comma = [&](const Token& t) {
            if (needs_comma) {
                fail("missing comma between array elements", {lit(",")}, before(t.span));
            }
        };
        while (const Token* t = next()) {
            switch (t->kind) {
            case Tok::Comment: on_comment(*t); break;
            case Tok::Whitespace: whitespace(t->span); break;
            case Tok::Newline: newline(t->span); break;
            case Tok::Eof: goto unclosed;
            case Tok::Comma:
                if (!needs_comma) {
                    fail("extra comma in array", {desc("value")}, t->span);
                }
                emit(Ev::ValueSep, t->span);
                needs_comma = false;
                break;
            case Tok::Equals:
                fail("unexpected `=` in array", {desc("value"), lit("]")}, t->span);
            case Tok::LBrace:
                check_comma(*t);
                on_inline_table_open(*t);
                needs_comma = true;
                break;
            case Tok::RBrace:
                check_comma(*t);
                fail("missing inline table opening", {lit("{")}, before(t->span));
            case Tok::LBracket:
                check_comma(*t);
                on_array_open(*t);
                needs_comma = true;
                break;
            case Tok::RBracket:
                close_nested(Ev::ArrayClose, t->span);
                return;
            default:
                check_comma(*t);
                on_scalar(*t);
                needs_comma = true;
                break;
            }
        }
    unclosed:
        fail("unclosed array", {lit("]")}, after(previous_span()));
    }

    enum class InState { NeedsKey, NeedsEquals, NeedsValue, NeedsComma };


    [[noreturn]] static void fail_in(InState state, std::string_view what, Span at) {
        switch (state) {
        case InState::NeedsKey: fail(what, {desc("key")}, at);
        case InState::NeedsEquals: fail(what, {lit("=")}, at);
        case InState::NeedsValue: fail(what, {desc("value")}, at);
        case InState::NeedsComma: break;
        }
        fail(what, {lit(",")}, at);
    }

    void on_inline_table_open(const Token& open) {
        open_nested(Ev::InlineTableOpen, open.span);
        InState state = InState::NeedsKey;
        auto start_key = [&](const Token& t) {
            if (t.kind == Tok::Dot) {
                simple_key(before(t.span), Enc::None);
                seek_back();
            } else {
                simple_key(t.span, encoding_of(t.kind));
            }
            opt_dot_keys();
            state = InState::NeedsEquals;
        };
        while (const Token* t = next()) {
            switch (t->kind) {
            case Tok::Comment: on_comment(*t); break;
            case Tok::Whitespace: whitespace(t->span); break;
            case Tok::Newline: newline(t->span); break;
            case Tok::Eof: goto unclosed;
            case Tok::Comma:
                if (state != InState::NeedsComma) {
                    fail_in(state, "extra comma in inline table", before(t->span));
                }
                emit(Ev::ValueSep, t->span);
                state = InState::NeedsKey;
                break;
            case Tok::Equals:
                if (state == InState::NeedsKey) {
                    simple_key(before(t->span), Enc::None);
                    emit(Ev::KeyValSep, t->span);
                    state = InState::NeedsValue;
                } else if (state == InState::NeedsEquals) {
                    emit(Ev::KeyValSep, t->span);
                    state = InState::NeedsValue;
                } else {
                    fail_in(state, "extra assignment between key-value pairs", before(t->span));
                }
                break;
            case Tok::LBrace:
            case Tok::LBracket:
                if (state == InState::NeedsKey || state == InState::NeedsComma) {
                    fail_in(state, "missing key for inline table element", before(t->span));
                }
                if (state == InState::NeedsEquals) {
                    fail_in(state, "missing assignment between key-value pairs", before(t->span));
                }
                if (t->kind == Tok::LBrace) {
                    on_inline_table_open(*t);
                } else {
                    on_array_open(*t);
                }
                state = InState::NeedsComma;
                break;
            case Tok::RBrace:
                if (state == InState::NeedsEquals) {
                    emit(Ev::KeyValSep, before(t->span));
                    emit(Ev::Scalar, before(t->span), Enc::Literal);
                } else if (state == InState::NeedsValue) {
                    emit(Ev::Scalar, before(t->span), Enc::Literal);
                }
                close_nested(Ev::InlineTableClose, t->span);
                return;
            case Tok::RBracket:
                if (state == InState::NeedsValue) {
                    fail("missing array opening", {lit("[")}, before(t->span));
                }
                fail_in(state, "invalid inline table element", before(t->span));
            default:
                switch (state) {
                case InState::NeedsKey: start_key(*t); break;
                case InState::NeedsEquals:
                    fail_in(state, "missing assignment between key-value pairs", before(t->span));
                case InState::NeedsValue:
                    on_scalar(*t);
                    state = InState::NeedsComma;
                    break;
                case InState::NeedsComma:
                    fail_in(state, "missing comma between key-value pairs", before(t->span));
                }
                break;
            }
        }
    unclosed:
        fail("unclosed inline table", {lit("}")}, after(previous_span()));
    }

    void opt_whitespace() {
        if (const Token* t = next_if(Tok::Whitespace)) {
            whitespace(t->span);
        }
    }

    void ws_comment_newline() {
        while (const Token* t = next()) {
            switch (t->kind) {
            case Tok::Comment: on_comment(*t); return;
            case Tok::Whitespace: whitespace(t->span); continue;
            case Tok::Newline: newline(t->span); return;
            case Tok::Eof: return;
            default:
                fail("unexpected key or value", {lit("\n"), lit("#")}, before(t->span));
            }
        }
    }

    void on_comment(const Token& c) {
        comment(c.span);
        const Token* t = next();
        if (!t) {
            return;
        }
        switch (t->kind) {
        case Tok::Newline: newline(t->span); return;
        case Tok::Eof: return;
        default:
            fail("unexpected content between comment and newline", {lit("\n")}, before(t->span));
        }
    }

    void eof() {
        const Token* t = next();
        if (!t || t->kind == Tok::Eof) {
            return;
        }
        fail("unexpected content", {}, before(t->span));
    }

    void ignore_to_newline() {
        while (const Token* t = next()) {
            switch (t->kind) {
            case Tok::Comment: on_comment(*t); return;
            case Tok::Whitespace: whitespace(t->span); break;
            case Tok::Newline: newline(t->span); return;
            case Tok::Eof: return;
            default: emit(Ev::Error, t->span); break;
            }
        }
    }
};


bool is_digit(unsigned char b) { return b >= '0' && b <= '9'; }
bool is_hex(unsigned char b) {
    return is_digit(b) || (b >= 'a' && b <= 'f') || (b >= 'A' && b <= 'F');
}
bool literal_char(unsigned char b) {
    return b == 0x09 || (b >= 0x20 && b <= 0x26) || (b >= 0x28 && b <= 0x7E) || b >= 0x80;
}
bool basic_unescaped(unsigned char b) {
    return b == ' ' || b == '\t' || b == 0x21 || (b >= 0x23 && b <= 0x5B) ||
           (b >= 0x5D && b <= 0x7E) || b >= 0x80;
}
bool bare_key_byte(unsigned char b) {
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || is_digit(b) || b == '-' || b == '_';
}

enum class ScalarKind { String, True, False, DateTime, Float, Integer };

struct Scalar {
    ScalarKind kind = ScalarKind::String;
    std::uint32_t radix = 10;
    std::string decoded;
};


class Decoder {
public:
    Decoder(std::string_view raw, std::size_t base) : raw_(raw), base_(base), n_(raw.size()) {}

    std::string key(Enc enc) {
        std::string out;
        switch (enc) {
        case Enc::Literal: literal_string(out); break;
        case Enc::Basic: basic_string(out); break;
        case Enc::MlLiteral:
            fail("keys cannot be multi-line literal strings",
                 {desc("basic string"), desc("literal string")}, at(0, n_));
        case Enc::MlBasic:
            fail("keys cannot be multi-line basic strings",
                 {desc("basic string"), desc("literal string")}, at(0, n_));
        case Enc::None: unquoted_key(out); break;
        }
        return out;
    }

    Scalar scalar(Enc enc) {
        Scalar s;
        switch (enc) {
        case Enc::Literal: literal_string(s.decoded); break;
        case Enc::Basic: basic_string(s.decoded); break;
        case Enc::MlLiteral: ml_literal_string(s.decoded); break;
        case Enc::MlBasic: ml_basic_string(s.decoded); break;
        case Enc::None: unquoted_scalar(s); break;
        }
        return s;
    }

private:
    std::string_view raw_;
    std::size_t base_;
    std::size_t n_;

    Span at(std::size_t start, std::size_t end) const { return {base_ + start, base_ + end}; }
    Span at(std::size_t p) const { return at(p, p); }
    unsigned char byte(std::size_t p) const { return static_cast<unsigned char>(raw_[p]); }


    void literal_string(std::string& out) {
        constexpr std::string_view kInvalid = "invalid literal string";
        std::size_t b = 0;
        std::size_t e = n_;
        if (n_ > 0 && raw_[0] == '\'') {
            b = 1;
        } else {
            fail(kInvalid, {lit("'")}, at(0));
        }
        if (e > b && raw_[e - 1] == '\'') {
            --e;
        } else {
            fail(kInvalid, {lit("'")}, at(n_));
        }
        for (std::size_t i = b; i < e; ++i) {
            if (!literal_char(byte(i))) {
                fail(kInvalid, {desc("non-single-quote visible characters")}, at(i));
            }
        }
        out.assign(raw_.substr(b, e - b));
    }

    std::size_t strip_start_newline(std::size_t b) const {
        if (b < n_ && raw_[b] == '\n') {
            return b + 1;
        }
        if (raw_.substr(b, 2) == "\r\n") {
            return b + 2;
        }
        return b;
    }

    void ml_literal_string(std::string& out) {
        constexpr std::string_view kInvalid = "invalid multi-line literal string";
        std::size_t b = 0;
        std::size_t e = n_;
        if (raw_.substr(0, 3) == "'''") {
            b = 3;
        } else {
            fail(kInvalid, {lit("'")}, at(0));
        }
        b = strip_start_newline(b);
        if (e >= b + 3 && raw_.substr(e - 3, 3) == "'''") {
            e -= 3;
        } else {
            fail(kInvalid, {lit("'")}, at(n_));
        }
        for (std::size_t i = b; i < e; ++i) {
            const unsigned char c = byte(i);
            if (c == '\'' || c == '\n') {
                continue;
            }
            if (c == '\r') {
                if (i + 1 >= e || raw_[i + 1] != '\n') {
                    fail("carriage return must be followed by newline", {lit("\n")}, at(i + 1));
                }
            } else if (!literal_char(c)) {
                fail(kInvalid, {desc("non-single-quote characters")}, at(i));
            }
        }
        out.assign(raw_.substr(b, e - b));
    }

    void basic_string(std::string& out) {
        constexpr std::string_view kInvalid = "invalid basic string";
        std::size_t p = 0;
        std::size_t e = n_;
        if (n_ > 0 && raw_[0] == '"') {
            p = 1;
        } else {
            fail(kInvalid, {lit("\"")}, at(0));
        }
        if (e > p && raw_[e - 1] == '"') {
            --e;
        } else {
            fail(kInvalid, {lit("\"")}, at(n_));
        }
        while (p < e) {
            const unsigned char c = byte(p);
            if (basic_unescaped(c)) {
                out += static_cast<char>(c);
                ++p;
            } else if (c == '\\') {
                ++p;
                escape(p, e, out);
            } else {
                std::size_t q = p;
                while (q < e && !basic_unescaped(byte(q)) && byte(q) != '\\') {
                    ++q;
                }
                fail(kInvalid, {desc("non-double-quote visible characters"), lit("\\")}, at(p, q));
            }
        }
    }


    void escape(std::size_t& p, std::size_t e, std::string& out) {
        auto missing = [&]() {
            fail("missing escaped value",
                 {lit("b"), lit("e"), lit("f"), lit("n"), lit("r"), lit("\\"), lit("\""), lit("x"),
                  lit("u"), lit("U")},
                 at(p));
        };
        if (p >= e) {
            missing();
        }
        switch (raw_[p]) {
        case 'b': out += '\b'; ++p; return;
        case 'e': out += '\x1b'; ++p; return;
        case 'f': out += '\f'; ++p; return;
        case 'n': out += '\n'; ++p; return;
        case 'r': out += '\r'; ++p; return;
        case 't': out += '\t'; ++p; return;
        case '\\': out += '\\'; ++p; return;
        case '"': out += '"'; ++p; return;
        case 'x': ++p; hex_escape(p, e, 2, out); return;
        case 'u': ++p; hex_escape(p, e, 4, out); return;
        case 'U': ++p; hex_escape(p, e, 8, out); return;
        default: missing();
        }
    }

    void hex_escape(std::size_t& p, std::size_t e, std::size_t digits, std::string& out) {
        const std::size_t start = p;
        std::size_t count = 0;
        while (count < digits && p + count < e && is_hex(byte(p + count))) {
            ++count;
        }
        p += count;
        if (count != digits) {
            fail("too few unicode value digits", {desc("unicode hexadecimal value")}, at(p));
        }
        std::uint32_t v = 0;
        for (std::size_t i = start; i < p; ++i) {
            const unsigned char c = byte(i);
            const std::uint32_t d = is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
            v = v * 16 + d;
        }
        if ((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
            fail("invalid value", {desc("unicode hexadecimal value")}, at(start));
        }
        push_utf8(out, v);
    }

    void ml_basic_string(std::string& out) {
        constexpr std::string_view kInvalid = "invalid multi-line basic string";
        std::size_t p = 0;
        std::size_t e = n_;
        if (raw_.substr(0, 3) == "\"\"\"") {
            p = 3;
        } else {
            fail(kInvalid, {lit("\"")}, at(0));
        }
        p = strip_start_newline(p);
        if (e >= p + 3 && raw_.substr(e - 3, 3) == "\"\"\"") {
            e -= 3;
        } else {
            fail(kInvalid, {lit("\"")}, at(n_));
        }
        auto unescaped = [](unsigned char c) { return basic_unescaped(c) || c == '"' || c == '\n'; };
        while (p < e) {
            const unsigned char c = byte(p);
            if (unescaped(c)) {
                out += static_cast<char>(c);
                ++p;
            } else if (c == '\\') {
                ++p;
                if (p < e && (raw_[p] == ' ' || raw_[p] == '\t' || raw_[p] == '\r' || raw_[p] == '\n')) {
                    escaped_newline(p, e);
                } else {
                    escape(p, e, out);
                }
            } else if (c == '\r') {
                if (p + 1 < e && raw_[p + 1] == '\n') {
                    out += "\r\n";
                    p += 2;
                } else {
                    fail("carriage return must be followed by newline", {lit("\n")}, at(p + 1));
                }
            } else {
                std::size_t q = p;
                while (q < e && !unescaped(byte(q)) && byte(q) != '\\' && byte(q) != '\r') {
                    ++q;
                }
                fail(kInvalid, {lit("\\"), desc("characters")}, at(p, q));
            }
        }
    }


    void escaped_newline(std::size_t& p, std::size_t e) {
        while (p < e && (raw_[p] == ' ' || raw_[p] == '\t')) {
            ++p;
        }
        if (p < e && raw_[p] == '\n') {
            ++p;
        } else if (p < e && raw_[p] == '\r') {
            ++p;
            if (p < e && raw_[p] == '\n') {
                ++p;
            } else {
                fail("carriage return must be followed by newline", {lit("\n")}, at(p));
            }
        } else {
            fail("invalid multi-line basic string", {lit("\n")}, at(p));
        }
        for (;;) {
            const std::size_t start = p;
            while (p < e && (raw_[p] == ' ' || raw_[p] == '\t' || raw_[p] == '\n')) {
                ++p;
            }
            if (p < e && raw_[p] == '\r') {
                if (p + 1 < e && raw_[p + 1] == '\n') {
                    p += 2;
                } else {
                    fail("carriage return must be followed by newline", {lit("\n")}, at(p + 1));
                }
            }
            if (p == start) {
                break;
            }
        }
    }

    void unquoted_key(std::string& out) {
        if (n_ == 0) {
            fail("unquoted keys cannot be empty",
                 {desc("letters"), desc("numbers"), lit("-"), lit("_")}, at(0));
        }
        for (std::size_t i = 0; i < n_; ++i) {
            if (!bare_key_byte(byte(i))) {
                std::size_t j = i + 1;
                while (j < n_ && !bare_key_byte(byte(j))) {
                    ++j;
                }
                fail("invalid unquoted key", {desc("letters"), desc("numbers"), lit("-"), lit("_")},
                     at(i, j));
            }
        }
        out.assign(raw_);
    }


    void unquoted_scalar(Scalar& s) {
        if (n_ == 0) {
            invalid();
        }
        const unsigned char first = byte(0);
        if (!is_digit(first) && raw_.find(' ') != std::string_view::npos) {
            invalid();
        }
        switch (first) {
        case '+':
        case '-': sign_prefix(s); return;
        case '_': datetime_or_float_or_integer(0, s); return;
        case '0': zero_prefix(0, false, s); return;
        case '.':
            ensure_float(0);
            float_or_integer(0, ScalarKind::Float, 10, s);
            return;
        case 't':
        case 'T': symbol("true", ScalarKind::True, "invalid boolean", s); return;
        case 'f':
        case 'F': symbol("false", ScalarKind::False, "invalid boolean", s); return;
        case 'i':
        case 'I': symbol("inf", ScalarKind::Float, "invalid float", s); return;
        case 'n':
        case 'N': symbol("nan", ScalarKind::Float, "invalid float", s); return;
        default:
            if (is_digit(first)) {
                datetime_or_float_or_integer(0, s);
                return;
            }
            invalid();
        }
    }

    void sign_prefix(Scalar& s) {
        std::size_t v = 1;
        for (;;) {
            if (v >= n_) {
                invalid();
            }
            if (raw_[v] != '+' && raw_[v] != '-') {
                break;
            }
            fail("redundant numeric sign", {}, at(v, v + 1));
        }
        const unsigned char c = byte(v);
        if (c == '_' || (c >= '1' && c <= '9')) {
            datetime_or_float_or_integer(v, s);
        } else if (c == '0') {
            zero_prefix(v, true, s);
        } else if (c == '.') {
            ensure_float(0);
            float_or_integer(0, ScalarKind::Float, 10, s);
        } else if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
            const std::string_view sym = (c == 'i' || c == 'I') ? "inf" : "nan";
            if (raw_.substr(v) != sym) {
                fail("invalid float", {lit(sym)}, at(v, n_));
            }
            s.kind = ScalarKind::Float;
            s.decoded.assign(raw_);
        } else {
            invalid();
        }
    }

    void zero_prefix(std::size_t v, bool is_signed, Scalar& s) {
        if (n_ - v == 1) {
            float_or_integer(0, ScalarKind::Integer, 10, s);
            return;
        }
        const char r = raw_[v + 1];
        std::uint32_t radix = 0;
        std::string_view prefix;
        switch (r) {
        case 'x': case 'X': radix = 16; prefix = "0x"; break;
        case 'o': case 'O': radix = 8; prefix = "0o"; break;
        case 'b': case 'B': radix = 2; prefix = "0b"; break;
        case 'd': case 'D': radix = 10; break;
        default: datetime_or_float_or_integer(v, s); return;
        }
        if (raw_.substr(v).find(' ') != std::string_view::npos) {
            invalid();
        }
        if (is_signed) {
            fail("integers with a radix cannot be signed", {}, at(0, 1));
        }
        if (radix == 10) {
            fail("redundant integer number prefix", {}, at(0, 2));
        }
        if (r >= 'A' && r <= 'Z') {
            fail("radix must be lowercase", {lit(prefix)}, at(v, v + 2));
        }
        ensure_radixed_value(v + 2, radix);
        float_or_integer(v + 2, ScalarKind::Integer, radix, s);
    }

    void datetime_or_float_or_integer(std::size_t v, Scalar& s) {
        std::size_t d = v;
        while (d < n_ && is_digit(byte(d))) {
            ++d;
        }
        if (d == n_) {
            ensure_no_leading_zero(v);
            float_or_integer(0, ScalarKind::Integer, 10, s);
            return;
        }
        const std::string_view rest = raw_.substr(d);
        if (rest[0] == '-' || rest[0] == ':') {
            s.kind = ScalarKind::DateTime;
            s.decoded.assign(raw_);
        } else if (rest.find(' ') != std::string_view::npos) {
            invalid();
        } else if (rest.find_first_of(".eE") != std::string_view::npos) {
            ensure_float(v);
            float_or_integer(0, ScalarKind::Float, 10, s);
        } else if (rest[0] == '_') {
            ensure_no_leading_zero(v);
            float_or_integer(0, ScalarKind::Integer, 10, s);
        } else {
            invalid();
        }
    }

    void ensure_float(std::size_t p) {
        ensure_dec_uint(p, false, "invalid mantissa");
        if (p < n_ && raw_[p] == '.') {
            ++p;
            ensure_dec_uint(p, true, "invalid fraction");
        }
        if (p < n_ && (raw_[p] == 'e' || raw_[p] == 'E')) {
            ++p;
            if (p < n_ && (raw_[p] == '+' || raw_[p] == '-')) {
                ++p;
            }
            ensure_dec_uint(p, true, "invalid exponent");
        }
        if (p != n_) {
            fail("invalid float", {}, at(p, n_));
        }
    }

    void ensure_dec_uint(std::size_t& p, bool zero_prefix, std::string_view description) {
        const std::size_t start = p;
        std::size_t digits = 0;
        while (p < n_) {
            const unsigned char c = byte(p);
            if (is_digit(c)) {
                ++digits;
            } else if (c != '_') {
                break;
            }
            ++p;
        }
        if (digits == 0) {
            fail(description, {desc("digits")}, at(start));
        }
        if (digits > 1 && raw_[start] == '0' && !zero_prefix) {
            fail("unexpected leading zero", {}, at(start, start + 1));
        }
    }

    void ensure_no_leading_zero(std::size_t v) {
        if (v < n_ && raw_[v] == '0') {
            fail("unexpected leading zero", {}, at(v, v + 1));
        }
    }

    void ensure_radixed_value(std::size_t v, std::uint32_t radix) {
        if (v < n_ && (raw_[v] == '+' || raw_[v] == '-')) {
            const std::size_t pos = raw_.find_first_of("+-");
            fail("unexpected sign", {}, at(pos, pos + 1));
        }
        for (std::size_t i = v; i < n_; ++i) {
            const unsigned char c = byte(i);
            bool ok = c == '_';
            if (radix == 16) {
                ok = ok || is_hex(c);
            } else if (radix == 8) {
                ok = ok || (c >= '0' && c <= '7');
            } else {
                ok = ok || c == '0' || c == '1';
            }
            if (!ok) {
                fail_plain(radix == 16  ? "invalid hexadecimal number"
                           : radix == 8 ? "invalid octal number"
                                        : "invalid binary number",
                           at(i));
            }
        }
    }


    void float_or_integer(std::size_t start, ScalarKind kind, std::uint32_t radix, Scalar& s) {
        s.kind = kind;
        s.radix = radix;
        s.decoded.clear();
        auto any_digit = [&](unsigned char b) { return kind == ScalarKind::Float ? is_digit(b) : is_hex(b); };
        std::size_t p = start;
        while (p < n_) {
            const std::size_t sep = raw_.find('_', p);
            const std::size_t part_end = sep == std::string_view::npos ? n_ : sep;
            const std::size_t part_start = p;
            s.decoded.append(raw_.substr(part_start, part_end - part_start));
            p = part_end;
            if (sep != std::string_view::npos) {
                ++p;
                bool invalid_sep = false;
                if (part_end > part_start) {
                    invalid_sep = !any_digit(byte(part_end - 1));
                } else if (part_start == start) {
                    invalid_sep = true;
                }
                if (p < n_) {
                    invalid_sep = invalid_sep || !any_digit(byte(p));
                } else {
                    invalid_sep = true;
                }
                if (invalid_sep) {
                    fail("`_` may only go between digits", {}, at(part_end, part_end + 1));
                }
            }
        }
    }

    void symbol(std::string_view sym, ScalarKind kind, std::string_view invalid_description, Scalar& s) {
        if (raw_ != sym) {
            if (raw_.find(' ') != std::string_view::npos) {
                invalid();
            }
            fail(invalid_description, {lit(sym)}, at(0, n_));
        }
        s.kind = kind;
        s.decoded.assign(sym);
    }

    [[noreturn]] void invalid() {
        auto ends = [&](std::string_view t) {
            return n_ >= t.size() && raw_.substr(n_ - t.size()) == t;
        };
        if (ends("'''")) {
            fail("missing opening quote", {lit("'''")}, at(0));
        }
        if (ends("\"\"\"")) {
            fail("missing opening quote", {lit("\"\"\"")}, at(0));
        }
        if (ends("'")) {
            fail("missing opening quote", {lit("'")}, at(0));
        }
        if (ends("\"")) {
            fail("missing opening quote", {lit("\"")}, at(0));
        }
        fail("string values must be quoted", {desc("literal string")}, at(0, n_));
    }
};


bool parse_datetime(std::string_view in, std::string& display, std::string& error) {
    enum K { Digits, Dash, Colon, Dot, T, Space, Z, Plus, Unknown };
    struct DTok {
        K kind;
        std::string_view raw;
    };
    std::string_view rest = in;
    auto next = [&]() -> std::optional<DTok> {
        if (rest.empty()) {
            return std::nullopt;
        }
        std::size_t end = 1;
        K kind = Unknown;
        switch (rest[0]) {
        case '-': kind = Dash; break;
        case ':': kind = Colon; break;
        case 'T': case 't': kind = T; break;
        case ' ': kind = Space; break;
        case 'Z': case 'z': kind = Z; break;
        case '+': kind = Plus; break;
        case '.': kind = Dot; break;
        default:
            if (is_digit(static_cast<unsigned char>(rest[0]))) {
                kind = Digits;
                while (end < rest.size() && is_digit(static_cast<unsigned char>(rest[end]))) {
                    ++end;
                }
            } else {
                end = rest.size();
            }
        }
        DTok t{kind, rest.substr(0, end)};
        rest.remove_prefix(end);
        return t;
    };
    auto peek_kind = [&]() -> std::optional<K> {
        const std::string_view saved = rest;
        auto t = next();
        rest = saved;
        if (!t) {
            return std::nullopt;
        }
        return t->kind;
    };
    auto err = [&](const char* what, const char* expected) {
        error = what ? std::string("invalid ") + what : std::string("invalid datetime");
        if (expected) {
            error += ", expected ";
            error += expected;
        }
        return false;
    };
    auto number = [](std::string_view digits) {
        std::uint32_t v = 0;
        for (char c : digits) {
            v = v * 10 + static_cast<std::uint32_t>(c - '0');
        }
        return v;
    };

    bool has_date = false;
    bool has_time = false;
    std::uint32_t year = 0, month = 0, day = 0, hour = 0, minute = 0;
    std::optional<std::uint32_t> second, nanosecond;
    std::string offset;

    auto digits = next();
    if (!digits || digits->kind != Digits) {
        return err(nullptr, "year or hour");
    }
    auto sep = next();
    if (!sep) {
        return err(nullptr, "`-` (YYYY-MM) or `:` (HH:MM)");
    }
    if (sep->kind == Dash) {
        auto m = next();
        if (!m || m->kind != Digits) {
            return err("date", "month");
        }
        auto sep2 = next();
        if (!sep2 || sep2->kind != Dash) {
            return err("date", "`-` (MM-DD)");
        }
        auto d = next();
        if (!d || d->kind != Digits) {
            return err("date", "day");
        }
        if (digits->raw.size() != 4) {
            return err("date", "a four-digit year (YYYY)");
        }
        if (m->raw.size() != 2) {
            return err("date", "a two-digit month (MM)");
        }
        if (d->raw.size() != 2) {
            return err("date", "a two-digit day (DD)");
        }
        year = number(digits->raw);
        month = number(m->raw);
        day = number(d->raw);
        if (month < 1 || month > 12) {
            return err("date", "month between 01 and 12");
        }
        const bool leap = (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
        std::uint32_t max_day = 31;
        const char* expected_day = "day between 01 and 31";
        if (month == 2) {
            max_day = leap ? 29 : 28;
            expected_day = leap ? "day between 01 and 29" : "day between 01 and 28";
        } else if (month == 4 || month == 6 || month == 9 || month == 11) {
            max_day = 30;
            expected_day = "day between 01 and 30";
        }
        if (day < 1 || day > max_day) {
            return err("date", expected_day);
        }
        has_date = true;
    } else if (sep->kind == Colon) {
        rest = in;
    } else {
        return err(nullptr, "`-` (YYYY-MM) or `:` (HH:MM)");
    }

    bool partial_time = !has_date;
    if (has_date) {
        auto t = next();
        if (t) {
            if (t->kind != T && t->kind != Space) {
                return err("date-time", "`T` between date and time");
            }
            partial_time = true;
        }
    }

    if (partial_time) {
        auto h = next();
        if (!h || h->kind != Digits) {
            return err("time", "hour");
        }
        auto c = next();
        if (!c || c->kind != Colon) {
            return err("time", "`:` (HH:MM)");
        }
        auto mi = next();
        if (!mi || mi->kind != Digits) {
            return err("time", "minute");
        }
        std::optional<DTok> sec;
        if (peek_kind() == Colon) {
            next();
            sec = next();
            if (!sec || sec->kind != Digits) {
                return err("time", "second");
            }
        }
        std::optional<DTok> frac;
        if (sec && peek_kind() == Dot) {
            next();
            frac = next();
            if (!frac || frac->kind != Digits) {
                return err("time", "nanosecond");
            }
        }
        if (h->raw.size() != 2) {
            return err("time", "a two-digit hour (HH)");
        }
        if (mi->raw.size() != 2) {
            return err("time", "a two-digit minute (MM)");
        }
        if (sec && sec->raw.size() != 2) {
            return err("time", "a two-digit second (SS)");
        }
        hour = number(h->raw);
        minute = number(mi->raw);
        if (sec) {
            second = number(sec->raw);
        }
        if (frac) {
            std::uint32_t ns = 0;
            for (std::size_t i = 0; i < frac->raw.size() && i < 9; ++i) {
                std::uint32_t p = 1;
                for (std::size_t k = i; k < 8; ++k) {
                    p *= 10;
                }
                ns += p * static_cast<std::uint32_t>(frac->raw[i] - '0');
            }
            nanosecond = ns;
        }
        if (hour > 23) {
            return err("time", "hour between 00 and 23");
        }
        if (minute > 59) {
            return err("time", "minute between 00 and 59");
        }
        if (second.value_or(0) > 60) {
            return err("time", "second between 00 and 60");
        }
        has_time = true;
    }

    if (has_date && has_time) {
        auto t = next();
        if (t) {
            if (t->kind == Z) {
                offset = "Z";
            } else if (t->kind == Plus || t->kind == Dash) {
                const int sign = t->kind == Plus ? 1 : -1;
                auto oh = next();
                if (!oh || oh->kind != Digits) {
                    return err("offset", "hour");
                }
                auto oc = next();
                if (!oc || oc->kind != Colon) {
                    return err("offset", "`:` (HH:MM)");
                }
                auto om = next();
                if (!om || om->kind != Digits) {
                    return err("offset", "minute");
                }
                if (oh->raw.size() != 2) {
                    return err("offset", "a two-digit hour (HH)");
                }
                if (om->raw.size() != 2) {
                    return err("offset", "a two-digit minute (MM)");
                }
                const std::uint32_t hours = number(oh->raw);
                const std::uint32_t minutes = number(om->raw);
                if (hours > 23) {
                    return err("offset", "hours between 00 and 23");
                }
                if (minutes > 59) {
                    return err("offset", "minutes between 00 and 59");
                }
                int total = sign * static_cast<int>(hours * 60 + minutes);
                char buf[16];
                const char sgn = total < 0 ? '-' : '+';
                total = total < 0 ? -total : total;
                std::snprintf(buf, sizeof buf, "%c%02d:%02d", sgn, total / 60, total % 60);
                offset = buf;
            } else {
                return err("offset", "`Z`, +OFFSET, -OFFSET");
            }
        }
    }

    if (!rest.empty()) {
        return err(nullptr, nullptr);
    }

    char buf[64];
    display.clear();
    if (has_date) {
        std::snprintf(buf, sizeof buf, "%04u-%02u-%02u", year, month, day);
        display += buf;
    }
    if (has_time) {
        if (has_date) {
            display += 'T';
        }
        std::snprintf(buf, sizeof buf, "%02u:%02u", hour, minute);
        display += buf;
        if (second || nanosecond) {
            std::snprintf(buf, sizeof buf, ":%02u", second.value_or(0));
            display += buf;
        }
        if (nanosecond) {
            std::snprintf(buf, sizeof buf, "%09u", *nanosecond);
            std::string f = buf;
            while (!f.empty() && f.back() == '0') {
                f.pop_back();
            }
            display += '.';
            display += f.empty() ? "0" : f;
        }
    }
    display += offset;
    return true;
}


struct DTable;


struct DVal {
    enum Kind { String, Integer, Float, Boolean, Datetime, Array, Table } kind = Integer;
    Span span;
    std::string str;   
    std::uint32_t radix = 10;
    bool boolean = false;
    std::vector<DVal> array;
    bool array_of_tables = false;
    std::unique_ptr<DTable> table;

    const char* type_str() const {
        switch (kind) {
        case String: return "string";
        case Integer: return "integer";
        case Float: return "float";
        case Boolean: return "boolean";
        case Datetime: return "datetime";
        case Array: return "array";
        default: return "table";
        }
    }
};

struct DEntry {
    Span key_span;
    DVal value;
};

struct DTable {
    std::map<std::string, DEntry> map;   
    bool implicit = false;
    bool dotted = false;
    bool is_inline = false;
};

DVal table_value(DTable t, Span span) {
    DVal v;
    v.kind = DVal::Table;
    v.span = span;
    v.table = std::make_unique<DTable>(std::move(t));
    return v;
}

struct Key {
    std::string name;
    Span span;
};

struct Header {
    std::vector<Key> path;
    std::optional<Key> key;
    Span span;
    bool is_array = false;
};

[[noreturn]] void cannot_extend(const char* type, Span key_span) {
    fail_plain(std::string("cannot extend value of type ") + type + " with a dotted key", key_span);
}


class Builder {
public:
    Builder(std::string_view text, const std::vector<Event>& events) : text_(text), ev_(events) {}

    DTable run() {
        while (const Event* e = next()) {
            switch (e->kind) {
            case Ev::StdTableOpen:
            case Ev::ArrayTableOpen: {
                finish_table();
                Header h = on_table(*e);
                start_table(std::move(h));
                break;
            }
            case Ev::SimpleKey: {
                auto [path, key] = on_key(*e);
                const Event* sep = next();
                if (!sep) {
                    goto done;
                }
                if (sep->kind == Ev::Whitespace) {
                    sep = next();
                    if (!sep) {
                        goto done;
                    }
                }
                if (sep->kind != Ev::KeyValSep) {
                    goto done;
                }
                if (const Event* f = first(); f && f->kind == Ev::Whitespace) {
                    next();
                }
                DVal v = value();
                capture_key_value(std::move(path), std::move(*key), std::move(v));
                break;
            }
            default: break;
            }
        }
    done:
        finish_table();
        return std::move(root_);
    }

private:
    std::string_view text_;
    const std::vector<Event>& ev_;
    std::size_t pos_ = 0;
    DTable root_;
    DTable current_;
    std::optional<Header> header_;

    const Event* next() { return pos_ < ev_.size() ? &ev_[pos_++] : nullptr; }
    const Event* first() const { return pos_ < ev_.size() ? &ev_[pos_] : nullptr; }
    const Event* peek(std::size_t k) const { return pos_ + k < ev_.size() ? &ev_[pos_ + k] : nullptr; }
    std::string_view raw(Span s) const { return text_.substr(s.start, s.end - s.start); }

    bool more_key() const {
        const Event* a = peek(0);
        const Event* b = peek(1);
        if (a && a->kind == Ev::KeySep) {
            return true;
        }
        return a && a->kind == Ev::Whitespace && b && b->kind == Ev::KeySep;
    }

    Key decode_key(const Event& e) const {
        return {Decoder(raw(e.span), e.span.start).key(e.enc), e.span};
    }


    std::pair<std::vector<Key>, std::optional<Key>> on_key(const Event& key_event) {
        std::vector<Key> path;
        std::optional<Key> last;
        std::optional<Event> current = key_event;
        auto close_key = [&]() {
            if (!current) {
                return;
            }
            Key k = decode_key(*current);
            current.reset();
            if (last) {
                path.push_back(std::move(*last));
            }
            last = std::move(k);
        };
        if (more_key()) {
            while (const Event* e = next()) {
                if (e->kind == Ev::SimpleKey) {
                    current = *e;
                    if (!more_key()) {
                        break;
                    }
                } else if (e->kind == Ev::KeySep) {
                    close_key();
                }
            }
        }
        close_key();
        if (path.size() >= static_cast<std::size_t>(kDepthLimit)) {
            Failure f;
            f.message = "recursion limit";
            throw f;
        }
        return {std::move(path), std::move(last)};
    }

    Header on_table(const Event& open) {
        Header h;
        h.is_array = open.kind == Ev::ArrayTableOpen;
        h.span = open.span;
        while (const Event* e = next()) {
            if (e->kind == Ev::ArrayTableClose || e->kind == Ev::StdTableClose) {
                h.span.end = e->span.end;
                break;
            }
            if (e->kind == Ev::SimpleKey) {
                auto [path, key] = on_key(*e);
                h.path = std::move(path);
                h.key = std::move(key);
            }
        }
        return h;
    }

    DVal value() {
        if (const Event* e = next()) {
            switch (e->kind) {
            case Ev::InlineTableOpen: return on_inline_table(*e);
            case Ev::ArrayOpen: return on_array(*e);
            case Ev::Scalar: return on_scalar(*e);
            default: break;
            }
        }
        DVal v;
        v.kind = DVal::Integer;
        v.str = "0";
        return v;
    }

    DVal on_scalar(const Event& e) {
        Scalar s = Decoder(raw(e.span), e.span.start).scalar(e.enc);
        DVal v;
        v.span = e.span;
        switch (s.kind) {
        case ScalarKind::String:
            v.kind = DVal::String;
            v.str = std::move(s.decoded);
            break;
        case ScalarKind::True:
        case ScalarKind::False:
            v.kind = DVal::Boolean;
            v.boolean = s.kind == ScalarKind::True;
            break;
        case ScalarKind::DateTime: {
            std::string display, error;
            if (!parse_datetime(s.decoded, display, error)) {
                fail_plain(error, e.span);
            }
            v.kind = DVal::Datetime;
            v.str = std::move(display);
            break;
        }
        case ScalarKind::Float:
            v.kind = DVal::Float;
            v.str = std::move(s.decoded);
            break;
        case ScalarKind::Integer:
            v.kind = DVal::Integer;
            v.str = std::move(s.decoded);
            v.radix = s.radix;
            break;
        }
        return v;
    }

    DVal on_array(const Event& open) {
        DVal result;
        result.kind = DVal::Array;
        Span close = open.span;
        std::optional<DVal> current;
        while (const Event* e = next()) {
            close = e->span;
            bool stop = false;
            switch (e->kind) {
            case Ev::StdTableOpen:
            case Ev::ArrayTableOpen:
            case Ev::InlineTableClose:
            case Ev::SimpleKey:
            case Ev::KeySep:
            case Ev::KeyValSep:
            case Ev::StdTableClose:
            case Ev::ArrayTableClose: stop = true; break;
            case Ev::InlineTableOpen: current = on_inline_table(*e); break;
            case Ev::ArrayOpen: current = on_array(*e); break;
            case Ev::Scalar: current = on_scalar(*e); break;
            case Ev::ValueSep:
                if (current) {
                    result.array.push_back(std::move(*current));
                    current.reset();
                }
                break;
            case Ev::ArrayClose:
                if (current) {
                    result.array.push_back(std::move(*current));
                    current.reset();
                }
                stop = true;
                break;
            default: break;
            }
            if (stop) {
                break;
            }
        }
        result.span = {open.span.start, close.end};
        return result;
    }


    DTable* inline_descend(DTable* t, const std::vector<Key>& path) {
        for (const Key& k : path) {
            auto it = t->map.find(k.name);
            if (it == t->map.end()) {
                DTable nt;
                nt.implicit = true;
                nt.dotted = true;
                nt.is_inline = true;
                it = t->map.emplace(k.name, DEntry{k.span, table_value(std::move(nt), k.span)}).first;
                t = it->second.value.table.get();
                continue;
            }
            DVal& v = it->second.value;
            if (v.kind != DVal::Table) {
                cannot_extend(v.type_str(), k.span);
            }
            if (!v.table->implicit) {
                fail_plain("duplicate key", k.span);
            }
            t = v.table.get();
        }
        return t;
    }

    DVal on_inline_table(const Event& open) {
        DTable result;
        result.is_inline = true;
        Span close = open.span;
        std::optional<std::pair<std::vector<Key>, Key>> cur_key;
        std::optional<DVal> cur_value;
        auto finish_value = [&]() {
            auto key = std::move(cur_key);
            auto value = std::move(cur_value);
            cur_key.reset();
            cur_value.reset();
            if (!key || !value) {
                return;
            }
            auto& [path, k] = *key;
            DTable* t = inline_descend(&result, path);
            if (t->dotted == path.empty()) {
                fail_plain("duplicate key", k.span);
            }
            if (t->map.count(k.name)) {
                fail_plain("duplicate key", k.span);
            }
            t->map.emplace(k.name, DEntry{k.span, std::move(*value)});
        };
        while (const Event* e = next()) {
            close = e->span;
            bool stop = false;
            switch (e->kind) {
            case Ev::StdTableOpen:
            case Ev::ArrayTableOpen:
            case Ev::StdTableClose:
            case Ev::ArrayClose:
            case Ev::ArrayTableClose:
            case Ev::KeySep: stop = true; break;
            case Ev::SimpleKey: {
                auto [path, key] = on_key(*e);
                if (key) {
                    cur_key.emplace(std::move(path), std::move(*key));
                }
                break;
            }
            case Ev::InlineTableOpen: cur_value = on_inline_table(*e); break;
            case Ev::ArrayOpen: cur_value = on_array(*e); break;
            case Ev::Scalar: cur_value = on_scalar(*e); break;
            case Ev::ValueSep: finish_value(); break;
            case Ev::InlineTableClose:
                finish_value();
                stop = true;
                break;
            default: break;
            }
            if (stop) {
                break;
            }
        }
        return table_value(std::move(result), {open.span.start, close.end});
    }


    DTable* descend(DTable* t, const std::vector<Key>& path, bool dotted) {
        for (const Key& k : path) {
            auto it = t->map.find(k.name);
            if (it == t->map.end()) {
                DTable nt;
                nt.implicit = true;
                nt.dotted = dotted;
                it = t->map.emplace(k.name, DEntry{k.span, table_value(std::move(nt), k.span)}).first;
                t = it->second.value.table.get();
                continue;
            }
            DVal& v = it->second.value;
            if (v.kind == DVal::Array) {
                if (!v.array_of_tables) {
                    cannot_extend("array", k.span);
                }
                DVal& last = v.array.back();
                if (last.kind != DVal::Table) {
                    cannot_extend(last.type_str(), k.span);
                }
                t = last.table.get();
            } else if (v.kind == DVal::Table) {
                DTable* child = v.table.get();
                if (child->is_inline) {
                    cannot_extend("inline table", k.span);
                }
                if (dotted && child->implicit) {
                    child->dotted = true;
                }
                if (dotted && !child->implicit) {
                    fail_plain("duplicate key", k.span);
                }
                t = child;
            } else {
                cannot_extend(v.type_str(), k.span);
            }
        }
        return t;
    }

    void capture_key_value(std::vector<Key> path, Key key, DVal value) {
        const bool dotted = !path.empty();
        DTable* parent = descend(&current_, path, dotted);
        if (dotted && !parent->implicit) {
            fail_plain("duplicate key", key.span);
        }
        if (parent->map.count(key.name)) {
            fail_plain("duplicate key", key.span);
        }
        parent->map.emplace(std::move(key.name), DEntry{key.span, std::move(value)});
    }

    void finish_table() {
        DTable prev = std::move(current_);
        current_ = DTable{};
        if (!header_) {
            root_ = std::move(prev);
            return;
        }
        Header h = std::move(*header_);
        header_.reset();
        if (!h.key) {
            return;
        }
        DVal prev_value = table_value(std::move(prev), h.span);
        DTable* parent = descend(&root_, h.path, false);
        const Key& key = *h.key;
        auto it = parent->map.find(key.name);
        if (h.is_array) {
            if (it == parent->map.end()) {
                DVal arr;
                arr.kind = DVal::Array;
                arr.array_of_tables = true;
                arr.span = h.span;
                it = parent->map.emplace(key.name, DEntry{key.span, std::move(arr)}).first;
            }
            DVal& entry = it->second.value;
            if (entry.kind != DVal::Array || !entry.array_of_tables) {
                fail_plain("duplicate key", key.span);
            }
            entry.array.push_back(std::move(prev_value));
        } else if (it == parent->map.end()) {
            parent->map.emplace(key.name, DEntry{key.span, std::move(prev_value)});
        } else {
            it->second.value = std::move(prev_value);
        }
    }

    void start_table(Header h) {
        if (!h.is_array) {
            DTable* parent = descend(&root_, h.path, false);
            if (h.key) {
                auto it = parent->map.find(h.key->name);
                if (it != parent->map.end()) {
                    DVal old = std::move(it->second.value);
                    parent->map.erase(it);
                    if (old.kind == DVal::Table && old.table->implicit && !old.table->dotted) {
                        current_ = std::move(*old.table);
                    } else {
                        fail_plain("duplicate key", h.key->span);
                    }
                }
            }
        }
        current_.implicit = false;
        current_.dotted = false;
        header_ = std::move(h);
    }
};


struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
};

bool mul_add(U128& v, std::uint32_t m, std::uint32_t a) {

    const std::uint64_t lo_lo = (v.lo & 0xFFFFFFFFu) * m + a;
    const std::uint64_t lo_hi = (v.lo >> 32) * m + (lo_lo >> 32);
    const std::uint64_t hi_lo = (v.hi & 0xFFFFFFFFu) * m + (lo_hi >> 32);
    const std::uint64_t hi_hi = (v.hi >> 32) * m + (hi_lo >> 32);
    if (hi_hi >> 32) {
        return false;
    }
    v.lo = (lo_lo & 0xFFFFFFFFu) | (lo_hi << 32);
    v.hi = (hi_lo & 0xFFFFFFFFu) | (hi_hi << 32);
    return true;
}

bool le(const U128& a, const U128& b) { return a.hi < b.hi || (a.hi == b.hi && a.lo <= b.lo); }

std::string to_decimal(U128 v) {
    if (v.hi == 0 && v.lo == 0) {
        return "0";
    }
    std::string out;
    while (v.hi != 0 || v.lo != 0) {

        std::uint32_t limbs[4] = {static_cast<std::uint32_t>(v.hi >> 32), static_cast<std::uint32_t>(v.hi),
                                  static_cast<std::uint32_t>(v.lo >> 32), static_cast<std::uint32_t>(v.lo)};
        std::uint64_t rem = 0;
        for (auto& l : limbs) {
            const std::uint64_t cur = (rem << 32) | l;
            l = static_cast<std::uint32_t>(cur / 10);
            rem = cur % 10;
        }
        out += static_cast<char>('0' + rem);
        v.hi = (static_cast<std::uint64_t>(limbs[0]) << 32) | limbs[1];
        v.lo = (static_cast<std::uint64_t>(limbs[2]) << 32) | limbs[3];
    }
    return {out.rbegin(), out.rend()};
}

constexpr std::string_view kDatetimeField = "$__toml_private_datetime";


std::string rust_float(double v) {
    if (std::isnan(v)) {
        return "NaN";
    }
    if (std::isinf(v)) {
        return v < 0 ? "-inf" : "inf";
    }
    if (v == 0) {
        return std::signbit(v) ? "-0" : "0";
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string_view s(buf, static_cast<std::size_t>(r.ptr - buf));
    std::string out;
    if (s[0] == '-') {
        out += '-';
        s.remove_prefix(1);
    }
    const std::size_t e = s.find('e');
    std::string digits;
    for (char c : s.substr(0, e)) {
        if (c != '.') {
            digits += c;
        }
    }
    int exp = std::atoi(std::string(s.substr(e + 1)).c_str());


    {
        char exact_buf[1100];
        auto x = std::to_chars(exact_buf, exact_buf + sizeof exact_buf, std::fabs(v),
                               std::chars_format::scientific, 800);
        std::string_view xs(exact_buf, static_cast<std::size_t>(x.ptr - exact_buf));
        const std::size_t xe = xs.find('e');
        std::string exact;
        for (char c : xs.substr(0, xe)) {
            if (c != '.') {
                exact += c;
            }
        }
        while (exact.size() > 1 && exact.back() == '0') {
            exact.pop_back();
        }
        const int exact_exp = std::atoi(std::string(xs.substr(xe + 1)).c_str());
        const std::size_t k = digits.size();
        if (exact_exp == exp && exact.size() == k + 1 && exact.compare(0, k, digits) == 0 && exact[k] == '5') {
            std::size_t i = k;
            while (i > 0 && digits[i - 1] == '9') {
                digits[i - 1] = '0';
                --i;
            }
            if (i == 0) {
                digits.insert(digits.begin(), '1');
                ++exp;
            } else {
                ++digits[i - 1];
            }
            while (digits.size() > 1 && digits.back() == '0') {
                digits.pop_back();
            }
        }
    }

    const int point = exp + 1;
    if (point <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-point), '0');
        out += digits;
    } else if (static_cast<std::size_t>(point) >= digits.size()) {
        out += digits;
        out.append(static_cast<std::size_t>(point) - digits.size(), '0');
    } else {
        out += digits.substr(0, static_cast<std::size_t>(point));
        out += '.';
        out += digits.substr(static_cast<std::size_t>(point));
    }
    return out;
}


bool parse_float(const std::string& s, double& out) {
    std::string_view body = s;
    bool negative = false;
    if (!body.empty() && (body[0] == '+' || body[0] == '-')) {
        negative = body[0] == '-';
        body.remove_prefix(1);
    }
    double v = 0;
    if (body == "inf") {
        v = std::numeric_limits<double>::infinity();
    } else if (body == "nan") {
        v = std::numeric_limits<double>::quiet_NaN();
    } else {
        auto r = std::from_chars(body.data(), body.data() + body.size(), v, std::chars_format::general);
        if (r.ptr != body.data() + body.size()) {
            return false;
        }
        if (r.ec == std::errc::result_out_of_range) {
            v = std::strtod(std::string(body).c_str(), nullptr);
        } else if (r.ec != std::errc{}) {
            return false;
        }
    }
    if (negative) {
        v = -v;
    }
    if (std::isinf(v) && s.find("inf") == std::string::npos) {
        return false;
    }
    out = v;
    return true;
}

class Converter {
public:
    Table root(DTable& t) {
        Table out;
        out.entries.reserve(t.map.size());
        for (auto& [k, e] : t.map) {
            out.entries.emplace_back(k, value(e.value));
        }
        return out;
    }

private:
    Value value(DVal& v) {
        switch (v.kind) {
        case DVal::String: return Value{std::move(v.str)};
        case DVal::Integer: return Value{integer(v)};
        case DVal::Float: {
            double f = 0;
            if (!parse_float(v.str, f)) {
                fail_plain("floating-point number overflowed", v.span);
            }
            return Value{f};
        }
        case DVal::Boolean: return Value{v.boolean};
        case DVal::Datetime: return Value{Datetime{std::move(v.str)}};
        case DVal::Array: {
            Array a;
            a.reserve(v.array.size());
            for (auto& e : v.array) {
                a.push_back(value(e));
            }
            return Value{std::move(a)};
        }
        case DVal::Table: return table(v);
        }
        return {};
    }

    Value table(DVal& v) {
        DTable& t = *v.table;
        if (!t.map.empty() && t.map.begin()->first == kDatetimeField) {
            return datetime_field(t.map.begin()->second.value);
        }
        Table out;
        out.entries.reserve(t.map.size());
        for (auto& [k, e] : t.map) {
            out.entries.emplace_back(k, value(e.value));
        }
        return Value{std::move(out)};
    }


    Value datetime_field(DVal& v) {
        if (v.kind == DVal::String) {
            std::string display, error;
            if (!parse_datetime(v.str, display, error)) {
                fail_plain(error, v.span);
            }
            return Value{Datetime{std::move(display)}};
        }
        std::string unexpected;
        switch (v.kind) {
        case DVal::Integer: {
            Classified c = classify(v);
            switch (c.kind) {
            case Classified::I64:
            case Classified::U64: unexpected = "integer `" + c.text + "`"; break;
            case Classified::I128: unexpected = "integer `" + c.text + "` as i128"; break;
            case Classified::U128: unexpected = "integer `" + c.text + "` as u128"; break;
            case Classified::Overflow: fail_plain("integer number overflowed", v.span);
            }
            break;
        }
        case DVal::Float: {
            double f = 0;
            if (!parse_float(v.str, f)) {
                fail_plain("floating-point number overflowed", v.span);
            }
            std::string s = rust_float(f);
            if (std::isfinite(f) && s.find('.') == std::string::npos) {
                s += ".0";
            }
            unexpected = "floating point `" + s + "`";
            break;
        }
        case DVal::Boolean: unexpected = v.boolean ? "boolean `true`" : "boolean `false`"; break;
        case DVal::Array: unexpected = "sequence"; break;
        default: unexpected = "map"; break;
        }
        fail_plain("invalid type: " + unexpected + ", expected string containing a datetime", v.span);
    }

public:
    struct Classified {
        enum Kind { I64, U64, I128, U128, Overflow } kind = Overflow;
        std::int64_t i64 = 0;
        std::string text;
    };


    static Classified classify(const DVal& v) {
        Classified c;
        std::string_view s = v.str;
        bool negative = false;
        if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
            if (s.size() == 1) {
                return c;
            }
            negative = s[0] == '-';
            s.remove_prefix(1);
        }
        if (s.empty()) {
            return c;
        }
        U128 m;
        for (char ch : s) {
            const auto b = static_cast<unsigned char>(ch);
            std::uint32_t d = 99;
            if (is_digit(b)) {
                d = b - '0';
            } else if (b >= 'a' && b <= 'z') {
                d = b - 'a' + 10;
            } else if (b >= 'A' && b <= 'Z') {
                d = b - 'A' + 10;
            }
            if (d >= v.radix) {
                return c;
            }
            if (!mul_add(m, v.radix, d)) {
                return c;
            }
        }
        const U128 i64_max{0, 0x7FFFFFFFFFFFFFFFull};
        const U128 i64_min_mag{0, 0x8000000000000000ull};
        const U128 u64_max{0, ~0ull};
        const U128 i128_max{0x7FFFFFFFFFFFFFFFull, ~0ull};
        const U128 i128_min_mag{0x8000000000000000ull, 0};
        if (negative) {
            if (le(m, i64_min_mag)) {
                c.kind = Classified::I64;
                c.i64 = m.lo == 0x8000000000000000ull ? std::numeric_limits<std::int64_t>::min()
                                                       : -static_cast<std::int64_t>(m.lo);
                c.text = std::to_string(c.i64);
            } else if (le(m, i128_min_mag)) {
                c.kind = Classified::I128;
                c.text = "-" + to_decimal(m);
            }
            return c;
        }
        c.text = to_decimal(m);
        if (le(m, i64_max)) {
            c.kind = Classified::I64;
            c.i64 = static_cast<std::int64_t>(m.lo);
        } else if (le(m, u64_max)) {
            c.kind = Classified::U64;
        } else if (le(m, i128_max)) {
            c.kind = Classified::I128;
        } else {
            c.kind = Classified::U128;
        }
        return c;
    }

private:
    static std::int64_t integer(const DVal& v) {
        Classified c = classify(v);
        switch (c.kind) {
        case Classified::I64: return c.i64;
        case Classified::U64: fail_plain("u64 value was too large", v.span);
        case Classified::I128:
            fail_plain("invalid type: integer `" + c.text + "` as i128, expected any valid TOML value", v.span);
        case Classified::U128:
            fail_plain("invalid type: integer `" + c.text + "` as u128, expected any valid TOML value", v.span);
        case Classified::Overflow: break;
        }
        fail_plain("integer number overflowed", v.span);
    }
};


void write_escaped(std::string& out, std::string_view s, bool ml) {
    const std::size_t max_quotes = ml ? 2 : 0;
    std::size_t i0 = 0;
    while (i0 < s.size()) {
        std::size_t unescaped_end = 0;
        const char* escaped = nullptr;
        std::size_t quotes = 0;
        for (std::size_t i = i0; i < s.size(); ++i) {
            const auto b = static_cast<unsigned char>(s[i]);
            if (b == '"') {
                ++quotes;
                if (max_quotes < quotes) {
                    escaped = "\\\"";
                    break;
                }
            } else {
                quotes = 0;
            }
            bool stop = false;
            switch (b) {
            case 0x08: escaped = "\\b"; stop = true; break;
            case 0x09: escaped = "\\t"; stop = true; break;
            case 0x0A:
                if (!ml) {
                    escaped = "\\n";
                    stop = true;
                }
                break;
            case 0x0C: escaped = "\\f"; stop = true; break;
            case 0x0D: escaped = "\\r"; stop = true; break;
            case 0x22: break;
            case 0x5C: escaped = "\\\\"; stop = true; break;
            default:
                if (b <= 0x1F || b == 0x7F) {
                    stop = true;
                }
                break;
            }
            if (stop) {
                break;
            }
            unescaped_end = i + 1 - i0;
        }
        out.append(s.substr(i0, unescaped_end));
        if (escaped) {
            out += escaped;
        }
        i0 += unescaped_end + (escaped ? 1 : 0);
        if (!escaped && i0 < s.size()) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(s[i0])));
            out += buf;
            ++i0;
        }
    }
}

void write_key(std::string& out, std::string_view k) {
    bool bare = !k.empty();
    bool single = false, dbl = false, backslash = false, codes = false;
    for (char ch : k) {
        const auto b = static_cast<unsigned char>(ch);
        if (!bare_key_byte(b)) {
            bare = false;
        }
        if (b == '\'') {
            single = true;
        } else if (b == '"') {
            dbl = true;
        } else if (b == '\\') {
            backslash = true;
        } else if (b != '\t' && (b <= 0x1F || b == 0x7F)) {
            codes = true;
        }
    }
    if (bare) {
        out += k;
    } else if (!codes && !backslash && !dbl) {
        out += '"';
        write_escaped(out, k, false);
        out += '"';
    } else if (!codes && !single) {
        out += '\'';
        out += k;
        out += '\'';
    } else {
        out += '"';
        write_escaped(out, k, false);
        out += '"';
    }
}

void write_string(std::string& out, std::string_view s) {
    bool subset = true;
    for (char ch : s) {
        const auto b = static_cast<unsigned char>(ch);
        if (!(bare_key_byte(b) || b == '.' || b == '/' || b == ' ' || b == ':')) {
            subset = false;
            break;
        }
    }
    if (subset) {
        out += '"';
        out += s;
        out += '"';
        return;
    }
    std::size_t max_single = 0, max_double = 0, run_single = 0, run_double = 0;
    bool backslash = false, newline = false, codes = false;
    for (char ch : s) {
        const auto b = static_cast<unsigned char>(ch);
        run_single = b == '\'' ? run_single + 1 : 0;
        run_double = b == '"' ? run_double + 1 : 0;
        max_single = std::max(max_single, run_single);
        max_double = std::max(max_double, run_double);
        if (b == '\\') {
            backslash = true;
        } else if (b == '\n') {
            newline = true;
        } else if (b != '\t' && (b <= 0x1F || b == 0x7F)) {
            codes = true;
        }
    }
    auto basic = [&]() {
        out += '"';
        write_escaped(out, s, false);
        out += '"';
    };
    auto ml_basic = [&]() {
        out += "\"\"\"";
        if (newline) {
            out += '\n';
        }
        write_escaped(out, s, true);
        out += "\"\"\"";
    };
    if (!codes && !backslash && max_double == 0 && !newline) {
        basic();
    } else if (!codes && max_single == 0 && !newline) {
        out += '\'';
        out += s;
        out += '\'';
    } else if (!codes && !backslash && max_double <= 2) {
        ml_basic();
    } else if (!codes && max_single <= 2) {
        out += "'''";
        if (newline) {
            out += '\n';
        }
        out += s;
        out += "'''";
    } else if (newline) {
        ml_basic();
    } else {
        basic();
    }
}

void write_float(std::string& out, double f) {
    if (std::isnan(f)) {
        out += "nan";   
    } else if (f == 0) {
        out += std::signbit(f) ? "-0.0" : "0.0";
    } else {
        out += rust_float(f);
        if (std::fmod(f, 1.0) == 0.0) {
            out += ".0";
        }
    }
}

void write_value(std::string& out, const Value& v) {
    switch (v.v.index()) {
    case 0: write_string(out, std::get<std::string>(v.v)); break;
    case 1: out += std::to_string(std::get<std::int64_t>(v.v)); break;
    case 2: write_float(out, std::get<double>(v.v)); break;
    case 3: out += std::get<bool>(v.v) ? "true" : "false"; break;
    case 4: out += std::get<Datetime>(v.v).text; break;
    case 5: {
        out += '[';
        bool first = true;
        for (const auto& e : std::get<Array>(v.v)) {
            if (!first) {
                out += ", ";
            }
            first = false;
            write_value(out, e);
        }
        out += ']';
        break;
    }
    default: {
        out += '{';
        bool first = true;
        for (const auto& [k, e] : std::get<Table>(v.v).entries) {
            if (!first) {
                out += ',';
            }
            first = false;
            out += ' ';
            write_key(out, k);
            out += " = ";
            write_value(out, e);
        }
        if (!first) {
            out += ' ';
        }
        out += '}';
        break;
    }
    }
}


bool debug_escaped(std::uint32_t c) {
    if (c < 0x80) {
        return c < 0x20 || c == 0x7F;
    }
    struct R {
        std::uint32_t lo, hi;
    };
    static constexpr R kRanges[] = {
    {0x80, 0xA0}, {0xAD, 0xAD}, {0x300, 0x36F}, {0x378, 0x379}, {0x380, 0x383}, {0x38B, 0x38B},
    {0x38D, 0x38D}, {0x3A2, 0x3A2}, {0x483, 0x489}, {0x530, 0x530}, {0x557, 0x558}, {0x58B, 0x58C},
    {0x590, 0x5BD}, {0x5BF, 0x5BF}, {0x5C1, 0x5C2}, {0x5C4, 0x5C5}, {0x5C7, 0x5CF}, {0x5EB, 0x5EE},
    {0x5F5, 0x605}, {0x610, 0x61A}, {0x61C, 0x61C}, {0x64B, 0x65F}, {0x670, 0x670}, {0x6D6, 0x6DD},
    {0x6DF, 0x6E4}, {0x6E7, 0x6E8}, {0x6EA, 0x6ED}, {0x70E, 0x70F}, {0x711, 0x711}, {0x730, 0x74C},
    {0x7A6, 0x7B0}, {0x7B2, 0x7BF}, {0x7EB, 0x7F3}, {0x7FB, 0x7FD}, {0x816, 0x819}, {0x81B, 0x823},
    {0x825, 0x827}, {0x829, 0x82F}, {0x83F, 0x83F}, {0x859, 0x85D}, {0x85F, 0x85F}, {0x86B, 0x86F},
    {0x890, 0x89F}, {0x8CA, 0x902}, {0x93A, 0x93A}, {0x93C, 0x93C}, {0x941, 0x948}, {0x94D, 0x94D},
    {0x951, 0x957}, {0x962, 0x963}, {0x981, 0x981}, {0x984, 0x984}, {0x98D, 0x98E}, {0x991, 0x992},
    {0x9A9, 0x9A9}, {0x9B1, 0x9B1}, {0x9B3, 0x9B5}, {0x9BA, 0x9BC}, {0x9BE, 0x9BE}, {0x9C1, 0x9C6},
    {0x9C9, 0x9CA}, {0x9CD, 0x9CD}, {0x9CF, 0x9DB}, {0x9DE, 0x9DE}, {0x9E2, 0x9E5}, {0x9FE, 0xA02},
    {0xA04, 0xA04}, {0xA0B, 0xA0E}, {0xA11, 0xA12}, {0xA29, 0xA29}, {0xA31, 0xA31}, {0xA34, 0xA34},
    {0xA37, 0xA37}, {0xA3A, 0xA3D}, {0xA41, 0xA58}, {0xA5D, 0xA5D}, {0xA5F, 0xA65}, {0xA70, 0xA71},
    {0xA75, 0xA75}, {0xA77, 0xA82}, {0xA84, 0xA84}, {0xA8E, 0xA8E}, {0xA92, 0xA92}, {0xAA9, 0xAA9},
    {0xAB1, 0xAB1}, {0xAB4, 0xAB4}, {0xABA, 0xABC}, {0xAC1, 0xAC8}, {0xACA, 0xACA}, {0xACD, 0xACF},
    {0xAD1, 0xADF}, {0xAE2, 0xAE5}, {0xAF2, 0xAF8}, {0xAFA, 0xB01}, {0xB04, 0xB04}, {0xB0D, 0xB0E},
    {0xB11, 0xB12}, {0xB29, 0xB29}, {0xB31, 0xB31}, {0xB34, 0xB34}, {0xB3A, 0xB3C}, {0xB3E, 0xB3F},
    {0xB41, 0xB46}, {0xB49, 0xB4A}, {0xB4D, 0xB5B}, {0xB5E, 0xB5E}, {0xB62, 0xB65}, {0xB78, 0xB82},
    {0xB84, 0xB84}, {0xB8B, 0xB8D}, {0xB91, 0xB91}, {0xB96, 0xB98}, {0xB9B, 0xB9B}, {0xB9D, 0xB9D},
    {0xBA0, 0xBA2}, {0xBA5, 0xBA7}, {0xBAB, 0xBAD}, {0xBBA, 0xBBE}, {0xBC0, 0xBC0}, {0xBC3, 0xBC5},
    {0xBC9, 0xBC9}, {0xBCD, 0xBCF}, {0xBD1, 0xBE5}, {0xBFB, 0xC00}, {0xC04, 0xC04}, {0xC0D, 0xC0D},
    {0xC11, 0xC11}, {0xC29, 0xC29}, {0xC3A, 0xC3C}, {0xC3E, 0xC40}, {0xC45, 0xC57}, {0xC5B, 0xC5B},
    {0xC5E, 0xC5F}, {0xC62, 0xC65}, {0xC70, 0xC76}, {0xC81, 0xC81}, {0xC8D, 0xC8D}, {0xC91, 0xC91},
    {0xCA9, 0xCA9}, {0xCB4, 0xCB4}, {0xCBA, 0xCBC}, {0xCBF, 0xCC0}, {0xCC2, 0xCC2}, {0xCC5, 0xCDB},
    {0xCDF, 0xCDF}, {0xCE2, 0xCE5}, {0xCF0, 0xCF0}, {0xCF4, 0xD01}, {0xD0D, 0xD0D}, {0xD11, 0xD11},
    {0xD3B, 0xD3C}, {0xD3E, 0xD3E}, {0xD41, 0xD45}, {0xD49, 0xD49}, {0xD4D, 0xD4D}, {0xD50, 0xD53},
    {0xD57, 0xD57}, {0xD62, 0xD65}, {0xD80, 0xD81}, {0xD84, 0xD84}, {0xD97, 0xD99}, {0xDB2, 0xDB2},
    {0xDBC, 0xDBC}, {0xDBE, 0xDBF}, {0xDC7, 0xDCF}, {0xDD2, 0xDD7}, {0xDDF, 0xDE5}, {0xDF0, 0xDF1},
    {0xDF5, 0xE00}, {0xE31, 0xE31}, {0xE34, 0xE3E}, {0xE47, 0xE4E}, {0xE5C, 0xE80}, {0xE83, 0xE83},
    {0xE85, 0xE85}, {0xE8B, 0xE8B}, {0xEA4, 0xEA4}, {0xEA6, 0xEA6}, {0xEB1, 0xEB1}, {0xEB4, 0xEBC},
    {0xEBE, 0xEBF}, {0xEC5, 0xEC5}, {0xEC7, 0xECF}, {0xEDA, 0xEDB}, {0xEE0, 0xEFF}, {0xF18, 0xF19},
    {0xF35, 0xF35}, {0xF37, 0xF37}, {0xF39, 0xF39}, {0xF48, 0xF48}, {0xF6D, 0xF7E}, {0xF80, 0xF84},
    {0xF86, 0xF87}, {0xF8D, 0xFBD}, {0xFC6, 0xFC6}, {0xFCD, 0xFCD}, {0xFDB, 0xFFF}, {0x102D, 0x1030},
    {0x1032, 0x1037}, {0x1039, 0x103A}, {0x103D, 0x103E}, {0x1058, 0x1059}, {0x105E, 0x1060},
    {0x1071, 0x1074}, {0x1082, 0x1082}, {0x1085, 0x1086}, {0x108D, 0x108D}, {0x109D, 0x109D},
    {0x10C6, 0x10C6}, {0x10C8, 0x10CC}, {0x10CE, 0x10CF}, {0x115F, 0x1160}, {0x1249, 0x1249},
    {0x124E, 0x124F}, {0x1257, 0x1257}, {0x1259, 0x1259}, {0x125E, 0x125F}, {0x1289, 0x1289},
    {0x128E, 0x128F}, {0x12B1, 0x12B1}, {0x12B6, 0x12B7}, {0x12BF, 0x12BF}, {0x12C1, 0x12C1},
    {0x12C6, 0x12C7}, {0x12D7, 0x12D7}, {0x1311, 0x1311}, {0x1316, 0x1317}, {0x135B, 0x135F},
    {0x137D, 0x137F}, {0x139A, 0x139F}, {0x13F6, 0x13F7}, {0x13FE, 0x13FF}, {0x1680, 0x1680},
    {0x169D, 0x169F}, {0x16F9, 0x16FF}, {0x1712, 0x171E}, {0x1732, 0x1734}, {0x1737, 0x173F},
    {0x1752, 0x175F}, {0x176D, 0x176D}, {0x1771, 0x177F}, {0x17B4, 0x17B5}, {0x17B7, 0x17BD},
    {0x17C6, 0x17C6}, {0x17C9, 0x17D3}, {0x17DD, 0x17DF}, {0x17EA, 0x17EF}, {0x17FA, 0x17FF},
    {0x180B, 0x180F}, {0x181A, 0x181F}, {0x1879, 0x187F}, {0x1885, 0x1886}, {0x18A9, 0x18A9},
    {0x18AB, 0x18AF}, {0x18F6, 0x18FF}, {0x191F, 0x1922}, {0x1927, 0x1928}, {0x192C, 0x192F},
    {0x1932, 0x1932}, {0x1939, 0x193F}, {0x1941, 0x1943}, {0x196E, 0x196F}, {0x1975, 0x197F},
    {0x19AC, 0x19AF}, {0x19CA, 0x19CF}, {0x19DB, 0x19DD}, {0x1A17, 0x1A18}, {0x1A1B, 0x1A1D},
    {0x1A56, 0x1A56}, {0x1A58, 0x1A60}, {0x1A62, 0x1A62}, {0x1A65, 0x1A6C}, {0x1A73, 0x1A7F},
    {0x1A8A, 0x1A8F}, {0x1A9A, 0x1A9F}, {0x1AAE, 0x1B03}, {0x1B34, 0x1B3D}, {0x1B42, 0x1B44},
    {0x1B4D, 0x1B4D}, {0x1B6B, 0x1B73}, {0x1B80, 0x1B81}, {0x1BA2, 0x1BA5}, {0x1BA8, 0x1BAD},
    {0x1BE6, 0x1BE6}, {0x1BE8, 0x1BE9}, {0x1BED, 0x1BED}, {0x1BEF, 0x1BFB}, {0x1C2C, 0x1C33},
    {0x1C36, 0x1C3A}, {0x1C4A, 0x1C4C}, {0x1C8B, 0x1C8F}, {0x1CBB, 0x1CBC}, {0x1CC8, 0x1CD2},
    {0x1CD4, 0x1CE0}, {0x1CE2, 0x1CE8}, {0x1CED, 0x1CED}, {0x1CF4, 0x1CF4}, {0x1CF8, 0x1CF9},
    {0x1CFB, 0x1CFF}, {0x1DC0, 0x1DFF}, {0x1F16, 0x1F17}, {0x1F1E, 0x1F1F}, {0x1F46, 0x1F47},
    {0x1F4E, 0x1F4F}, {0x1F58, 0x1F58}, {0x1F5A, 0x1F5A}, {0x1F5C, 0x1F5C}, {0x1F5E, 0x1F5E},
    {0x1F7E, 0x1F7F}, {0x1FB5, 0x1FB5}, {0x1FC5, 0x1FC5}, {0x1FD4, 0x1FD5}, {0x1FDC, 0x1FDC},
    {0x1FF0, 0x1FF1}, {0x1FF5, 0x1FF5}, {0x1FFF, 0x200F}, {0x2028, 0x202F}, {0x205F, 0x206F},
    {0x2072, 0x2073}, {0x208F, 0x208F}, {0x209D, 0x209F}, {0x20C2, 0x20FF}, {0x218C, 0x218F},
    {0x242A, 0x243F}, {0x244B, 0x245F}, {0x2B74, 0x2B75}, {0x2CEF, 0x2CF1}, {0x2CF4, 0x2CF8},
    {0x2D26, 0x2D26}, {0x2D28, 0x2D2C}, {0x2D2E, 0x2D2F}, {0x2D68, 0x2D6E}, {0x2D71, 0x2D7F},
    {0x2D97, 0x2D9F}, {0x2DA7, 0x2DA7}, {0x2DAF, 0x2DAF}, {0x2DB7, 0x2DB7}, {0x2DBF, 0x2DBF},
    {0x2DC7, 0x2DC7}, {0x2DCF, 0x2DCF}, {0x2DD7, 0x2DD7}, {0x2DDF, 0x2DFF}, {0x2E5E, 0x2E7F},
    {0x2E9A, 0x2E9A}, {0x2EF4, 0x2EFF}, {0x2FD6, 0x2FEF}, {0x3000, 0x3000}, {0x302A, 0x302F},
    {0x3040, 0x3040}, {0x3097, 0x309A}, {0x3100, 0x3104}, {0x3130, 0x3130}, {0x3164, 0x3164},
    {0x318F, 0x318F}, {0x31E6, 0x31EE}, {0x321F, 0x321F}, {0xA48D, 0xA48F}, {0xA4C7, 0xA4CF},
    {0xA62C, 0xA63F}, {0xA66F, 0xA672}, {0xA674, 0xA67D}, {0xA69E, 0xA69F}, {0xA6F0, 0xA6F1},
    {0xA6F8, 0xA6FF}, {0xA7DD, 0xA7F0}, {0xA802, 0xA802}, {0xA806, 0xA806}, {0xA80B, 0xA80B},
    {0xA825, 0xA826}, {0xA82C, 0xA82F}, {0xA83A, 0xA83F}, {0xA878, 0xA87F}, {0xA8C4, 0xA8CD},
    {0xA8DA, 0xA8F1}, {0xA8FF, 0xA8FF}, {0xA926, 0xA92D}, {0xA947, 0xA951}, {0xA953, 0xA95E},
    {0xA97D, 0xA982}, {0xA9B3, 0xA9B3}, {0xA9B6, 0xA9B9}, {0xA9BC, 0xA9BD}, {0xA9C0, 0xA9C0},
    {0xA9CE, 0xA9CE}, {0xA9DA, 0xA9DD}, {0xA9E5, 0xA9E5}, {0xA9FF, 0xA9FF}, {0xAA29, 0xAA2E},
    {0xAA31, 0xAA32}, {0xAA35, 0xAA3F}, {0xAA43, 0xAA43}, {0xAA4C, 0xAA4C}, {0xAA4E, 0xAA4F},
    {0xAA5A, 0xAA5B}, {0xAA7C, 0xAA7C}, {0xAAB0, 0xAAB0}, {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8},
    {0xAABE, 0xAABF}, {0xAAC1, 0xAAC1}, {0xAAC3, 0xAADA}, {0xAAEC, 0xAAED}, {0xAAF6, 0xAB00},
    {0xAB07, 0xAB08}, {0xAB0F, 0xAB10}, {0xAB17, 0xAB1F}, {0xAB27, 0xAB27}, {0xAB2F, 0xAB2F},
    {0xAB6C, 0xAB6F}, {0xABE5, 0xABE5}, {0xABE8, 0xABE8}, {0xABED, 0xABEF}, {0xABFA, 0xABFF},
    {0xD7A4, 0xD7AF}, {0xD7C7, 0xD7CA}, {0xD7FC, 0xF8FF}, {0xFA6E, 0xFA6F}, {0xFADA, 0xFAFF},
    {0xFB07, 0xFB12}, {0xFB18, 0xFB1C}, {0xFB1E, 0xFB1E}, {0xFB37, 0xFB37}, {0xFB3D, 0xFB3D},
    {0xFB3F, 0xFB3F}, {0xFB42, 0xFB42}, {0xFB45, 0xFB45}, {0xFDD0, 0xFDEF}, {0xFE00, 0xFE0F},
    {0xFE1A, 0xFE2F}, {0xFE53, 0xFE53}, {0xFE67, 0xFE67}, {0xFE6C, 0xFE6F}, {0xFE75, 0xFE75},
    {0xFEFD, 0xFF00}, {0xFF9E, 0xFFA0}, {0xFFBF, 0xFFC1}, {0xFFC8, 0xFFC9}, {0xFFD0, 0xFFD1},
    {0xFFD8, 0xFFD9}, {0xFFDD, 0xFFDF}, {0xFFE7, 0xFFE7}, {0xFFEF, 0xFFFB}, {0xFFFE, 0xFFFF},
    {0x1000C, 0x1000C}, {0x10027, 0x10027}, {0x1003B, 0x1003B}, {0x1003E, 0x1003E}, {0x1004E, 0x1004F},
    {0x1005E, 0x1007F}, {0x100FB, 0x100FF}, {0x10103, 0x10106}, {0x10134, 0x10136}, {0x1018F, 0x1018F},
    {0x1019D, 0x1019F}, {0x101A1, 0x101CF}, {0x101FD, 0x1027F}, {0x1029D, 0x1029F}, {0x102D1, 0x102E0},
    {0x102FC, 0x102FF}, {0x10324, 0x1032C}, {0x1034B, 0x1034F}, {0x10376, 0x1037F}, {0x1039E, 0x1039E},
    {0x103C4, 0x103C7}, {0x103D6, 0x103FF}, {0x1049E, 0x1049F}, {0x104AA, 0x104AF}, {0x104D4, 0x104D7},
    {0x104FC, 0x104FF}, {0x10528, 0x1052F}, {0x10564, 0x1056E}, {0x1057B, 0x1057B}, {0x1058B, 0x1058B},
    {0x10593, 0x10593}, {0x10596, 0x10596}, {0x105A2, 0x105A2}, {0x105B2, 0x105B2}, {0x105BA, 0x105BA},
    {0x105BD, 0x105BF}, {0x105F4, 0x105FF}, {0x10737, 0x1073F}, {0x10756, 0x1075F}, {0x10768, 0x1077F},
    {0x10786, 0x10786}, {0x107B1, 0x107B1}, {0x107BB, 0x107FF}, {0x10806, 0x10807}, {0x10809, 0x10809},
    {0x10836, 0x10836}, {0x10839, 0x1083B}, {0x1083D, 0x1083E}, {0x10856, 0x10856}, {0x1089F, 0x108A6},
    {0x108B0, 0x108DF}, {0x108F3, 0x108F3}, {0x108F6, 0x108FA}, {0x1091C, 0x1091E}, {0x1093A, 0x1093E},
    {0x1095A, 0x1097F}, {0x109B8, 0x109BB}, {0x109D0, 0x109D1}, {0x10A01, 0x10A0F}, {0x10A14, 0x10A14},
    {0x10A18, 0x10A18}, {0x10A36, 0x10A3F}, {0x10A49, 0x10A4F}, {0x10A59, 0x10A5F}, {0x10AA0, 0x10ABF},
    {0x10AE5, 0x10AEA}, {0x10AF7, 0x10AFF}, {0x10B36, 0x10B38}, {0x10B56, 0x10B57}, {0x10B73, 0x10B77},
    {0x10B92, 0x10B98}, {0x10B9D, 0x10BA8}, {0x10BB0, 0x10BFF}, {0x10C49, 0x10C7F}, {0x10CB3, 0x10CBF},
    {0x10CF3, 0x10CF9}, {0x10D24, 0x10D2F}, {0x10D3A, 0x10D3F}, {0x10D66, 0x10D6D}, {0x10D86, 0x10D8D},
    {0x10D90, 0x10E5F}, {0x10E7F, 0x10E7F}, {0x10EAA, 0x10EAC}, {0x10EAE, 0x10EAF}, {0x10EB2, 0x10EC1},
    {0x10EC8, 0x10ECF}, {0x10ED9, 0x10EFF}, {0x10F28, 0x10F2F}, {0x10F46, 0x10F50}, {0x10F5A, 0x10F6F},
    {0x10F82, 0x10F85}, {0x10F8A, 0x10FAF}, {0x10FCC, 0x10FDF}, {0x10FF7, 0x10FFF}, {0x11001, 0x11001},
    {0x11038, 0x11046}, {0x1104E, 0x11051}, {0x11070, 0x11070}, {0x11073, 0x11074}, {0x11076, 0x11081},
    {0x110B3, 0x110B6}, {0x110B9, 0x110BA}, {0x110BD, 0x110BD}, {0x110C2, 0x110CF}, {0x110E9, 0x110EF},
    {0x110FA, 0x11102}, {0x11127, 0x1112B}, {0x1112D, 0x11135}, {0x11148, 0x1114F}, {0x11173, 0x11173},
    {0x11177, 0x11181}, {0x111B6, 0x111BE}, {0x111C0, 0x111C0}, {0x111C9, 0x111CC}, {0x111CF, 0x111CF},
    {0x111E0, 0x111E0}, {0x111F5, 0x111FF}, {0x11212, 0x11212}, {0x1122F, 0x11231}, {0x11234, 0x11237},
    {0x1123E, 0x1123E}, {0x11241, 0x1127F}, {0x11287, 0x11287}, {0x11289, 0x11289}, {0x1128E, 0x1128E},
    {0x1129E, 0x1129E}, {0x112AA, 0x112AF}, {0x112DF, 0x112DF}, {0x112E3, 0x112EF}, {0x112FA, 0x11301},
    {0x11304, 0x11304}, {0x1130D, 0x1130E}, {0x11311, 0x11312}, {0x11329, 0x11329}, {0x11331, 0x11331},
    {0x11334, 0x11334}, {0x1133A, 0x1133C}, {0x1133E, 0x1133E}, {0x11340, 0x11340}, {0x11345, 0x11346},
    {0x11349, 0x1134A}, {0x1134D, 0x1134F}, {0x11351, 0x1135C}, {0x11364, 0x1137F}, {0x1138A, 0x1138A},
    {0x1138C, 0x1138D}, {0x1138F, 0x1138F}, {0x113B6, 0x113B6}, {0x113B8, 0x113B8}, {0x113BB, 0x113C9},
    {0x113CB, 0x113CB}, {0x113CE, 0x113D0}, {0x113D2, 0x113D2}, {0x113D6, 0x113D6}, {0x113D9, 0x113FF},
    {0x11438, 0x1143F}, {0x11442, 0x11444}, {0x11446, 0x11446}, {0x1145C, 0x1145C}, {0x1145E, 0x1145E},
    {0x11462, 0x1147F}, {0x114B0, 0x114B0}, {0x114B3, 0x114B8}, {0x114BA, 0x114BA}, {0x114BD, 0x114BD},
    {0x114BF, 0x114C0}, {0x114C2, 0x114C3}, {0x114C8, 0x114CF}, {0x114DA, 0x1157F}, {0x115AF, 0x115AF},
    {0x115B2, 0x115B7}, {0x115BC, 0x115BD}, {0x115BF, 0x115C0}, {0x115DC, 0x115FF}, {0x11633, 0x1163A},
    {0x1163D, 0x1163D}, {0x1163F, 0x11640}, {0x11645, 0x1164F}, {0x1165A, 0x1165F}, {0x1166D, 0x1167F},
    {0x116AB, 0x116AB}, {0x116AD, 0x116AD}, {0x116B0, 0x116B7}, {0x116BA, 0x116BF}, {0x116CA, 0x116CF},
    {0x116E4, 0x116FF}, {0x1171B, 0x1171D}, {0x1171F, 0x1171F}, {0x11722, 0x11725}, {0x11727, 0x1172F},
    {0x11747, 0x117FF}, {0x1182F, 0x11837}, {0x11839, 0x1183A}, {0x1183C, 0x1189F}, {0x118F3, 0x118FE},
    {0x11907, 0x11908}, {0x1190A, 0x1190B}, {0x11914, 0x11914}, {0x11917, 0x11917}, {0x11930, 0x11930},
    {0x11936, 0x11936}, {0x11939, 0x1193E}, {0x11943, 0x11943}, {0x11947, 0x1194F}, {0x1195A, 0x1199F},
    {0x119A8, 0x119A9}, {0x119D4, 0x119DB}, {0x119E0, 0x119E0}, {0x119E5, 0x119FF}, {0x11A01, 0x11A0A},
    {0x11A33, 0x11A38}, {0x11A3B, 0x11A3E}, {0x11A47, 0x11A4F}, {0x11A51, 0x11A56}, {0x11A59, 0x11A5B},
    {0x11A8A, 0x11A96}, {0x11A98, 0x11A99}, {0x11AA3, 0x11AAF}, {0x11AF9, 0x11AFF}, {0x11B0A, 0x11B60},
    {0x11B62, 0x11B64}, {0x11B66, 0x11B66}, {0x11B68, 0x11BBF}, {0x11BE2, 0x11BEF}, {0x11BFA, 0x11BFF},
    {0x11C09, 0x11C09}, {0x11C30, 0x11C3D}, {0x11C3F, 0x11C3F}, {0x11C46, 0x11C4F}, {0x11C6D, 0x11C6F},
    {0x11C90, 0x11CA8}, {0x11CAA, 0x11CB0}, {0x11CB2, 0x11CB3}, {0x11CB5, 0x11CFF}, {0x11D07, 0x11D07},
    {0x11D0A, 0x11D0A}, {0x11D31, 0x11D45}, {0x11D47, 0x11D4F}, {0x11D5A, 0x11D5F}, {0x11D66, 0x11D66},
    {0x11D69, 0x11D69}, {0x11D8F, 0x11D92}, {0x11D95, 0x11D95}, {0x11D97, 0x11D97}, {0x11D99, 0x11D9F},
    {0x11DAA, 0x11DAF}, {0x11DDC, 0x11DDF}, {0x11DEA, 0x11EDF}, {0x11EF3, 0x11EF4}, {0x11EF9, 0x11F01},
    {0x11F11, 0x11F11}, {0x11F36, 0x11F3D}, {0x11F40, 0x11F42}, {0x11F5A, 0x11FAF}, {0x11FB1, 0x11FBF},
    {0x11FF2, 0x11FFE}, {0x1239A, 0x123FF}, {0x1246F, 0x1246F}, {0x12475, 0x1247F}, {0x12544, 0x12F8F},
    {0x12FF3, 0x12FFF}, {0x13430, 0x13440}, {0x13447, 0x1345F}, {0x143FB, 0x143FF}, {0x14647, 0x160FF},
    {0x1611E, 0x16129}, {0x1612D, 0x1612F}, {0x1613A, 0x167FF}, {0x16A39, 0x16A3F}, {0x16A5F, 0x16A5F},
    {0x16A6A, 0x16A6D}, {0x16ABF, 0x16ABF}, {0x16ACA, 0x16ACF}, {0x16AEE, 0x16AF4}, {0x16AF6, 0x16AFF},
    {0x16B30, 0x16B36}, {0x16B46, 0x16B4F}, {0x16B5A, 0x16B5A}, {0x16B62, 0x16B62}, {0x16B78, 0x16B7C},
    {0x16B90, 0x16D3F}, {0x16D7A, 0x16E3F}, {0x16E9B, 0x16E9F}, {0x16EB9, 0x16EBA}, {0x16ED4, 0x16EFF},
    {0x16F4B, 0x16F4F}, {0x16F88, 0x16F92}, {0x16FA0, 0x16FDF}, {0x16FE4, 0x16FF1}, {0x16FF7, 0x16FFF},
    {0x18CD6, 0x18CFE}, {0x18D1F, 0x18D7F}, {0x18DF3, 0x1AFEF}, {0x1AFF4, 0x1AFF4}, {0x1AFFC, 0x1AFFC},
    {0x1AFFF, 0x1AFFF}, {0x1B123, 0x1B131}, {0x1B133, 0x1B14F}, {0x1B153, 0x1B154}, {0x1B156, 0x1B163},
    {0x1B168, 0x1B16F}, {0x1B2FC, 0x1BBFF}, {0x1BC6B, 0x1BC6F}, {0x1BC7D, 0x1BC7F}, {0x1BC89, 0x1BC8F},
    {0x1BC9A, 0x1BC9B}, {0x1BC9D, 0x1BC9E}, {0x1BCA0, 0x1CBFF}, {0x1CCFD, 0x1CCFF}, {0x1CEB4, 0x1CEB9},
    {0x1CED1, 0x1CEDF}, {0x1CEF1, 0x1CF4F}, {0x1CFC4, 0x1CFFF}, {0x1D0F6, 0x1D0FF}, {0x1D127, 0x1D128},
    {0x1D165, 0x1D169}, {0x1D16D, 0x1D182}, {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1D1EB, 0x1D1FF},
    {0x1D242, 0x1D244}, {0x1D246, 0x1D2BF}, {0x1D2D4, 0x1D2DF}, {0x1D2F4, 0x1D2FF}, {0x1D357, 0x1D35F},
    {0x1D379, 0x1D3FF}, {0x1D455, 0x1D455}, {0x1D49D, 0x1D49D}, {0x1D4A0, 0x1D4A1}, {0x1D4A3, 0x1D4A4},
    {0x1D4A7, 0x1D4A8}, {0x1D4AD, 0x1D4AD}, {0x1D4BA, 0x1D4BA}, {0x1D4BC, 0x1D4BC}, {0x1D4C4, 0x1D4C4},
    {0x1D506, 0x1D506}, {0x1D50B, 0x1D50C}, {0x1D515, 0x1D515}, {0x1D51D, 0x1D51D}, {0x1D53A, 0x1D53A},
    {0x1D53F, 0x1D53F}, {0x1D545, 0x1D545}, {0x1D547, 0x1D549}, {0x1D551, 0x1D551}, {0x1D6A6, 0x1D6A7},
    {0x1D7CC, 0x1D7CD}, {0x1DA00, 0x1DA36}, {0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75}, {0x1DA84, 0x1DA84},
    {0x1DA8C, 0x1DEFF}, {0x1DF1F, 0x1DF24}, {0x1DF2B, 0x1E02F}, {0x1E06E, 0x1E0FF}, {0x1E12D, 0x1E136},
    {0x1E13E, 0x1E13F}, {0x1E14A, 0x1E14D}, {0x1E150, 0x1E28F}, {0x1E2AE, 0x1E2BF}, {0x1E2EC, 0x1E2EF},
    {0x1E2FA, 0x1E2FE}, {0x1E300, 0x1E4CF}, {0x1E4EC, 0x1E4EF}, {0x1E4FA, 0x1E5CF}, {0x1E5EE, 0x1E5EF},
    {0x1E5FB, 0x1E5FE}, {0x1E600, 0x1E6BF}, {0x1E6DF, 0x1E6DF}, {0x1E6E3, 0x1E6E3}, {0x1E6E6, 0x1E6E6},
    {0x1E6EE, 0x1E6EF}, {0x1E6F5, 0x1E6FD}, {0x1E700, 0x1E7DF}, {0x1E7E7, 0x1E7E7}, {0x1E7EC, 0x1E7EC},
    {0x1E7EF, 0x1E7EF}, {0x1E7FF, 0x1E7FF}, {0x1E8C5, 0x1E8C6}, {0x1E8D0, 0x1E8FF}, {0x1E944, 0x1E94A},
    {0x1E94C, 0x1E94F}, {0x1E95A, 0x1E95D}, {0x1E960, 0x1EC70}, {0x1ECB5, 0x1ED00}, {0x1ED3E, 0x1EDFF},
    {0x1EE04, 0x1EE04}, {0x1EE20, 0x1EE20}, {0x1EE23, 0x1EE23}, {0x1EE25, 0x1EE26}, {0x1EE28, 0x1EE28},
    {0x1EE33, 0x1EE33}, {0x1EE38, 0x1EE38}, {0x1EE3A, 0x1EE3A}, {0x1EE3C, 0x1EE41}, {0x1EE43, 0x1EE46},
    {0x1EE48, 0x1EE48}, {0x1EE4A, 0x1EE4A}, {0x1EE4C, 0x1EE4C}, {0x1EE50, 0x1EE50}, {0x1EE53, 0x1EE53},
    {0x1EE55, 0x1EE56}, {0x1EE58, 0x1EE58}, {0x1EE5A, 0x1EE5A}, {0x1EE5C, 0x1EE5C}, {0x1EE5E, 0x1EE5E},
    {0x1EE60, 0x1EE60}, {0x1EE63, 0x1EE63}, {0x1EE65, 0x1EE66}, {0x1EE6B, 0x1EE6B}, {0x1EE73, 0x1EE73},
    {0x1EE78, 0x1EE78}, {0x1EE7D, 0x1EE7D}, {0x1EE7F, 0x1EE7F}, {0x1EE8A, 0x1EE8A}, {0x1EE9C, 0x1EEA0},
    {0x1EEA4, 0x1EEA4}, {0x1EEAA, 0x1EEAA}, {0x1EEBC, 0x1EEEF}, {0x1EEF2, 0x1EFFF}, {0x1F02C, 0x1F02F},
    {0x1F094, 0x1F09F}, {0x1F0AF, 0x1F0B0}, {0x1F0C0, 0x1F0C0}, {0x1F0D0, 0x1F0D0}, {0x1F0F6, 0x1F0FF},
    {0x1F1AE, 0x1F1E5}, {0x1F203, 0x1F20F}, {0x1F23C, 0x1F23F}, {0x1F249, 0x1F24F}, {0x1F252, 0x1F25F},
    {0x1F266, 0x1F2FF}, {0x1F6D9, 0x1F6DB}, {0x1F6ED, 0x1F6EF}, {0x1F6FD, 0x1F6FF}, {0x1F7DA, 0x1F7DF},
    {0x1F7EC, 0x1F7EF}, {0x1F7F1, 0x1F7FF}, {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F},
    {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8AF}, {0x1F8BC, 0x1F8BF}, {0x1F8C2, 0x1F8CF}, {0x1F8D9, 0x1F8FF},
    {0x1FA58, 0x1FA5F}, {0x1FA6E, 0x1FA6F}, {0x1FA7D, 0x1FA7F}, {0x1FA8B, 0x1FA8D}, {0x1FAC7, 0x1FAC7},
    {0x1FAC9, 0x1FACC}, {0x1FADD, 0x1FADE}, {0x1FAEB, 0x1FAEE}, {0x1FAF9, 0x1FAFF}, {0x1FB93, 0x1FB93},
    {0x1FBFB, 0x1FFFF}, {0x2A6E0, 0x2A6FF}, {0x2B81E, 0x2B81F}, {0x2CEAE, 0x2CEAF}, {0x2EBE1, 0x2EBEF},
    {0x2EE5E, 0x2F7FF}, {0x2FA1E, 0x2FFFF}, {0x3134B, 0x3134F}, {0x3347A, 0x10FFFF},
    };
    std::size_t lo = 0;
    std::size_t hi = std::size(kRanges);
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (kRanges[mid].hi < c) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < std::size(kRanges) && kRanges[lo].lo <= c;
}


std::string debug_str(std::string_view s) {
    std::string out = "\"";
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        len = std::min(len, s.size() - i);
        std::uint32_t c = len == 1 ? b : len == 2 ? b & 0x1F : len == 3 ? b & 0x0F : b & 0x07;
        for (std::size_t k = 1; k < len; ++k) {
            c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        switch (c) {
        case 0: out += "\\0"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default:
            if (debug_escaped(c)) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "\\u{%x}", c);
                out += buf;
            } else {
                out.append(s.substr(i, len));
            }
        }
        i += len;
    }
    return out + "\"";
}

Node to_node(DVal& v);

Node table_node(DTable& t, Span span) {
    Node n;
    n.kind = Node::Kind::Table;
    n.start = span.start;
    n.end = span.end;
    n.fields.reserve(t.map.size());
    for (auto& [k, e] : t.map) {
        n.fields.push_back(Field{k, e.key_span.start, e.key_span.end, to_node(e.value)});
    }
    return n;
}

Node to_node(DVal& v) {
    if (v.kind == DVal::Table) {
        return table_node(*v.table, v.span);
    }
    Node n;
    n.start = v.span.start;
    n.end = v.span.end;
    switch (v.kind) {
    case DVal::String: n.kind = Node::Kind::String; break;
    case DVal::Integer: n.kind = Node::Kind::Integer; break;
    case DVal::Float: n.kind = Node::Kind::Float; break;
    case DVal::Boolean: n.kind = Node::Kind::Boolean; break;
    case DVal::Datetime: n.kind = Node::Kind::Datetime; break;
    default: n.kind = Node::Kind::Array; break;
    }
    n.text = std::move(v.str);
    n.radix = v.radix;
    n.boolean = v.boolean;
    for (auto& e : v.array) {
        n.items.push_back(to_node(e));
    }
    return n;
}

ParseError failure_error(std::string_view text, const Failure& f) {
    ParseError e;
    e.message = f.message;
    e.has_span = f.has_span;
    if (f.has_span) {
        e.span_start = f.span.start;
        e.span_end = f.span.end;
        auto [line, column] = translate_position(text, f.span.start);
        e.line = line + 1;
        e.column = column + 1;
    }
    return e;
}

std::optional<ParseError> utf8_error(std::string_view text) {
    const Utf8Check utf8 = check_utf8(text);
    if (utf8.ok) {
        return std::nullopt;
    }
    ParseError e;
    if (utf8.error_len > 0) {
        e.message = "invalid utf-8 sequence of " + std::to_string(utf8.error_len) + " bytes from index " +
                    std::to_string(utf8.valid_up_to);
    } else {
        e.message = "incomplete utf-8 byte sequence from index " + std::to_string(utf8.valid_up_to);
    }
    return e;
}

}   

std::variant<Node, ParseError> parse_document(std::string_view text) {
    if (auto e = utf8_error(text)) {
        return *e;
    }
    try {
        const std::vector<Token> tokens = lex(text);
        std::vector<Event> events;
        events.reserve(tokens.size());
        Parser(text, tokens, events).run();
        DTable root = Builder(text, events).run();
        return table_node(root, Span{0, 0});
    } catch (const Failure& f) {
        return failure_error(text, f);
    }
}

ParseError error_at(std::string_view text, std::string message, std::size_t start, std::size_t end) {
    Failure f;
    f.message = std::move(message);
    f.has_span = true;
    f.span = {start, end};
    return failure_error(text, f);
}

bool unexpected(const Node& n, std::string& text, std::string& error) {
    switch (n.kind) {
    case Node::Kind::String: text = "string " + debug_str(n.text); return true;
    case Node::Kind::Integer: {
        DVal v;
        v.str = n.text;
        v.radix = n.radix;
        const auto c = Converter::classify(v);
        switch (c.kind) {
        case Converter::Classified::I64:
        case Converter::Classified::U64: text = "integer `" + c.text + "`"; return true;
        case Converter::Classified::I128: text = "integer `" + c.text + "` as i128"; return true;
        case Converter::Classified::U128: text = "integer `" + c.text + "` as u128"; return true;
        case Converter::Classified::Overflow: break;
        }
        error = "integer number overflowed";
        return false;
    }
    case Node::Kind::Float: {
        double f = 0;
        if (!parse_float(n.text, f)) {
            error = "floating-point number overflowed";
            return false;
        }
        std::string s = rust_float(f);
        if (std::isfinite(f) && s.find('.') == std::string::npos) {
            s += ".0";
        }
        text = "floating point `" + s + "`";
        return true;
    }
    case Node::Kind::Boolean: text = n.boolean ? "boolean `true`" : "boolean `false`"; return true;
    case Node::Kind::Array: text = "sequence"; return true;
    default: text = "map"; return true;
    }
}

std::variant<Table, ParseError> parse(std::string_view text) {
    const Utf8Check utf8 = check_utf8(text);
    if (!utf8.ok) {
        ParseError e;
        if (utf8.error_len > 0) {
            e.message = "invalid utf-8 sequence of " + std::to_string(utf8.error_len) + " bytes from index " +
                        std::to_string(utf8.valid_up_to);
        } else {
            e.message = "incomplete utf-8 byte sequence from index " + std::to_string(utf8.valid_up_to);
        }
        return e;
    }
    try {
        const std::vector<Token> tokens = lex(text);
        std::vector<Event> events;
        events.reserve(tokens.size());
        Parser(text, tokens, events).run();
        DTable root = Builder(text, events).run();
        return Converter().root(root);
    } catch (const Failure& f) {
        ParseError e;
        e.message = f.message;
        e.has_span = f.has_span;
        if (f.has_span) {
            e.span_start = f.span.start;
            e.span_end = f.span.end;
            auto [line, column] = translate_position(text, f.span.start);
            e.line = line + 1;
            e.column = column + 1;
        }
        return e;
    }
}

std::string display(const ParseError& e, std::string_view text) {
    std::string out;
    if (e.has_span) {
        auto [line, column] = translate_position(text, e.span_start);
        const std::size_t line_num = line + 1;
        const std::string num = std::to_string(line_num);
        const std::size_t gutter = num.size();
        std::string_view content = text;
        for (std::size_t k = 0; k < line; ++k) {
            const std::size_t nl = content.find('\n');
            content = nl == std::string_view::npos ? std::string_view{} : content.substr(nl + 1);
        }
        content = content.substr(0, content.find('\n'));
        std::size_t highlight = e.span_end - e.span_start;
        const std::size_t room = content.size() > column ? content.size() - column : 0;
        highlight = std::min(highlight, room);

        out += "TOML parse error at line " + num + ", column " + std::to_string(column + 1) + "\n";
        out.append(gutter + 1, ' ');
        out += "|\n";
        out += num + " | ";
        out += content;
        out += '\n';
        out.append(gutter + 1, ' ');
        out += '|';
        out.append(column + 1, ' ');
        out += '^';
        if (highlight > 1) {
            out.append(highlight - 1, '^');
        }
        out += '\n';
    }
    out += e.message;
    out += '\n';
    return out;
}

std::string to_string(const Value& v) {
    std::string out;
    write_value(out, v);
    return out;
}

std::string to_string(const Datetime& d) { return d.text; }

}   
