# Notices

DanziLoader-Cpp is a C++20 port of DanziLoader, which is itself based on
victory-perf. It is a derivative work and is distributed under the same
licence: **GPL-3.0-or-later** (see [LICENSE](LICENSE), copied unchanged from
the reference checkout). Every source file carries
`SPDX-License-Identifier: GPL-3.0-or-later`, and every ported file names the
reference file it was ported from.

## Upstream projects (credits as given by the reference)

- **DanziLoader**: Windows frame-pacing experiments based on
  [victory-perf at 97c3aa2](https://github.com/vitutu1/victory-perf/tree/97c3aa24263b90fa6691fca8ca88fca179593a19).
  Reference checkout used for this port: commit
  `38ea5b65121ddf3f0c33e6257b214c1ec01ef45e` (public reference used for this checkpoint).
- **[vitutu1/victory-perf](https://github.com/vitutu1/victory-perf)**
  (GPL-3.0-or-later): performance- and lighting-focused Windows compatibility
  layer.
- **[Marvisak/VIctory](https://github.com/Marvisak/VIctory)**
  (GPL-3.0-or-later): foundational PE32+ loader, runtime HLE, `d3d12_xs`
  graphics layer, BCPack/swizzle texture decoders, and the RDNA2 → DXIL
  shader recompiler.
- **[fxceless/victory-fork](https://github.com/fxceless/victory-fork)**
  (GPL-3.0-or-later): world rendering, shader optimizations, card buffers,
  relay threading, in-place memory work.
- **[ZigZagBoi1/ScarlettPort](https://github.com/ZigZagBoi1/ScarlettPort)**
  (GPL-3.0-or-later): ported modules and design ideas listed in the
  reference README.
- **[ivall/xcl](https://github.com/ivall/xcl)** (GPL-2.0-only): referenced
  by upstream for behavioural comparison. None of its code is used here.
  Note: the reference checkout's `victory-loader/src/console_insn.rs` and
  `console_insn_decode.rs` state that they are ported from XCL
  (GPL-2.0-only); this port does not take them in (milestone 6, M6.4).

## Asset policy

The reference's asset policy (`docs/asset-policy.md` there) applies here
unchanged:

- Nothing from any game is stored in this repository.
- Tests use synthetic data that our own code generates.
- Personal game data, caches and saves live outside source control.

## Third-party code

- `third_party/blake3/`: the BLAKE3 team's C implementation (version 1.8.7,
  from the `blake3` crate's `c/` folder). It is under CC0 1.0 or Apache-2.0
  (with or without the LLVM exception); the licence files are kept beside
  it. The port uses it for the keys of the reference's shader cache.
- The `toml` crate and its companions from the toml-rs project (`toml_edit`,
  `toml_parser`, `toml_datetime`, `toml_writer`; MIT OR Apache-2.0): the
  behaviour reference for `src/core/toml.cpp`, which reproduces what the
  reference's `toml` 1.2.2 (`toml_parser` 1.1.6, `toml_datetime` 1.1.3,
  `toml_writer` 1.2.0) accepts, returns and reports. No code is taken from
  them.
- `third_party/opus/`: libopus 1.6.1 from Xiph.Org (BSD-3-Clause; `COPYING`
  and `AUTHORS` kept beside it; origin and checksum in `VENDOR.md`). The port
  decodes the console's Opus streams (`xapu`) with it, where the reference
  uses the `opus-pure` crate (MIT OR Apache-2.0).

## Console-instruction emulation

`src/loader/emulate.cpp` / `emulate.hpp` (the emulation of the console
CPU's instructions a host CPU lacks: `clzero` and its loops, SSE4a,
`monitorx`/`mwaitx`, `rdpru`, `rdpid`, `clwb`, `clflushopt`, SHA, ADX,
`rdseed`) were written independently for this port from the AMD64
Architecture Programmer's Manual and the Intel SDM, and checked against the
instructions executed natively. They are not
derived from XCL nor from the reference's XCL-derived
`console_insn.rs`/`console_insn_decode.rs`, which this port does not use.
