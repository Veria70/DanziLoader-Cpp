// SPDX-License-Identifier: GPL-3.0-or-later
// Port of DanziLoader / victory-perf (see NOTICE.md).
//
// The reference reads its switches with `std::env::var_os(NAME)` and
// compares the whole value; each switch is read once, at first use
// (`OnceLock`). These helpers give the same answers: a variable set to the
// empty string is present (and is not "0" nor "1").
#pragma once

#include <optional>
#include <string>

namespace danzi::env {


std::optional<std::wstring> get(const wchar_t* name);


inline bool is(const wchar_t* name, const wchar_t* value) {
    auto v = get(name);
    return v && *v == value;
}


inline bool not_set_or_not(const wchar_t* name, const wchar_t* value) {
    auto v = get(name);
    return !v || *v != value;
}

}   
