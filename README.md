# DanziLoader-Cpp

A Windows x64 C++20 port of DanziLoader. This development checkpoint covers dirty-page tracking, copy-back, GPU memory management and the first device interfaces of the graphics backend.

## Stage 4.2

The synthetic device and DXGI scenarios compare the C++ and Rust backends through the same test driver. They exercise device creation, interface queries, reference counts, feature responses, descriptor sizes and adapter and output descriptions. Command submission, draw calls and presentation are later milestones. Whole-game FPS has not been measured with this C++ port.

## Build and test

Use Visual Studio 2026 Community with MSVC x64, the Windows SDK, CMake and Ninja, plus Python and the Rust toolchain pinned in the oracle. In PowerShell from the project root:

    git clone https://github.com/Veria70/DanziLoader.git ..\DanziLoader-reference
    git -C ..\DanziLoader-reference checkout 38ea5b65121ddf3f0c33e6257b214c1ec01ef45e
    $env:DANZI_REFERENCE = (Resolve-Path ..\DanziLoader-reference).Path
    cargo fetch --manifest-path oracle/Cargo.toml --locked
    cargo fetch --manifest-path ..\DanziLoader-reference/Cargo.toml --locked
    .\tools\build.cmd release

See [NOTICE.md](NOTICE.md) for project origins and [LICENSE](LICENSE) for licence terms.

## Verification on 26 November 2026

All 10 Release CTest targets passed on Windows x64. The two device and DXGI scenario logs matched byte for byte across the Rust and C++ backends. The observed outputs are in [the verification results](results/m4-command-translation/).
