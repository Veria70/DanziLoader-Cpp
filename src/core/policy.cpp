// SPDX-License-Identifier: GPL-3.0-or-later
// Rule #1 (`victory-core/src/policy.rs`; DanziLoader / victory-perf, see
// NOTICE.md). See policy.hpp.
#include "policy.hpp"

#include <system_error>
#include <vector>

#ifndef DANZI_SOURCE_ROOT
#define DANZI_SOURCE_ROOT "."
#endif

namespace danzi::policy {

namespace fs = std::filesystem;

namespace {


std::string os_error(const std::error_code& ec) { return ec.message() + " (os error " + std::to_string(ec.value()) + ")"; }


fs::path normalize(const fs::path& path) {
    fs::path out;
    for (const auto& c : path) {
        if (c == ".") {
            continue;
        }
        if (c == "..") {
            if (out.has_relative_path()) {
                out = out.parent_path();
            }
            continue;
        }
        out /= c;
    }
    return out;
}


fs::path resolve(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = path.is_absolute() ? path : fs::current_path(ec) / path;
    if (ec) {
        throw PolicyError("cannot resolve " + path.string() + ": " + os_error(ec));
    }
    fs::path existing = absolute;
    std::vector<fs::path> rest;
    for (;;) {
        fs::path canon = fs::canonical(existing, ec);
        if (!ec) {
            for (auto it = rest.rbegin(); it != rest.rend(); ++it) {
                canon /= *it;
            }
            return normalize(canon);
        }
        if (!existing.has_filename() || existing.parent_path() == existing) {
            throw PolicyError("cannot resolve " + path.string() + ": " + os_error(ec));
        }
        rest.push_back(existing.filename());
        existing = existing.parent_path();
    }
}

bool starts_with(const fs::path& path, const fs::path& root) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p) {
        if (p == path.end() || *p != *r) {
            return false;
        }
    }
    return true;
}


std::string shown(const fs::path& p) {
    std::string s = p.string();
    return s.starts_with(R"(\\)") ? s : R"(\\?\)" + s;
}

std::string inside(const fs::path& path, const fs::path& root) {
    return "refusing to use " + path.string() + " for game-derived data: it is inside the VIctory source tree (" + shown(root) +
           "). Keep game files, caches and outputs outside the repository (see docs/asset-policy.md)";
}

}   

fs::path compiled_workspace_root() { return fs::path(DANZI_SOURCE_ROOT); }

void ensure_outside_repo(const fs::path& path) {
    fs::path resolved = resolve(path);
    std::error_code ec;
    fs::path root = fs::canonical(compiled_workspace_root(), ec);
    if (!ec && fs::is_regular_file(root / ROOT_MARKER, ec) && starts_with(resolved, root)) {
        throw PolicyError(inside(path, root));
    }
    for (fs::path a = resolved;; a = a.parent_path()) {
        if (fs::is_regular_file(a / ROOT_MARKER, ec)) {
            throw PolicyError(inside(path, a));
        }
        if (!a.has_relative_path()) {
            break;
        }
    }
}

}   
