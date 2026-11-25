// SPDX-License-Identifier: GPL-3.0-or-later
// Port of DanziLoader / victory-perf (see NOTICE.md).


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
