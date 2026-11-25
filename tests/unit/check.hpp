// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdio>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> body;
};

std::vector<Test>& registry();
void fail(const char* file, int line, const std::string& what);

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

template <class T>
concept Streamable = requires(std::ostream& os, const T& v) { os << v; };

template <class A, class B>
std::string show(const std::pair<A, B>& p);
template <class T>
std::string show(const T& v);

template <class T>
std::string show(const std::optional<T>& v) {
    return v ? "Some(" + show(*v) + ")" : std::string("None");
}

template <class T>
std::string show(const T& v) {
    std::ostringstream o;
    if constexpr (Streamable<T>) {
        o << v;
    } else if constexpr (requires(const T& t) { t.begin(); t.end(); }) {
        o << "[";
        bool first = true;
        for (const auto& x : v) {
            if (!first) o << ", ";
            first = false;
            o << show(x);
        }
        o << "]";
    }
    return o.str();
}

template <class A, class B>
std::string show(const std::pair<A, B>& p) {
    return "(" + show(p.first) + ", " + show(p.second) + ")";
}

}   

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                         \
    static void name();                                                    \
    static ::check::Register CHECK_CAT(register_, name)(#name, name);      \
    static void name()
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) ::check::fail(__FILE__, __LINE__, "CHECK(" #cond ")");        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                    \
    do {                                                                                                                  \
        const auto& check_a = (a);                                                                                        \
        const auto& check_b = (b);                                                                                        \
        if (!(check_a == check_b))                                                                                        \
            ::check::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b "): " + ::check::show(check_a) + " vs " + ::check::show(check_b)); \
    } while (0)
