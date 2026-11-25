// SPDX-License-Identifier: GPL-3.0-or-later
#include "env.hpp"

#include "win.hpp"

namespace danzi::env {

std::optional<std::wstring> get(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::wstring();
    }
    std::wstring value(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, value.data(), n);
    if (got == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return std::nullopt;
    }
    value.resize(got);
    return value;
}

}   
