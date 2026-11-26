// SPDX-License-Identifier: GPL-3.0-or-later
// Rule #1, enforced at run time: game-derived data never lands in a source
// tree (`victory-core/src/policy.rs`; DanziLoader / victory-perf, see
// NOTICE.md). Every tool that writes such output (prepared images, shader
// caches, crash dumps) calls `ensure_outside_repo` on the destination first.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace danzi::policy {


constexpr const char* ROOT_MARKER = ".victory-root";

class PolicyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};


void ensure_outside_repo(const std::filesystem::path& path);


std::filesystem::path compiled_workspace_root();

}   
